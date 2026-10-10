#include "mods.h"
#include <map>
#include <string>
#include "detours.h"

/// What the XML loader hands its binary parser: the whole file followed by three zero bytes.
struct XmlBuffer {
    BYTE* data;
    DWORD size;
    DWORD position;
    DWORD poolSize;
};

typedef __int64(__fastcall* LoadXmlFile_t)(void* document, const char* path);
typedef __int64(__fastcall* ParseBinaryXml_t)(void* document, XmlBuffer* buffer);
static LoadXmlFile_t real_LoadXmlFile = nullptr;
static ParseBinaryXml_t real_ParseBinaryXml = nullptr;

/// The game's allocator; the document frees the file buffer with it once the document is destroyed. The same object hands
/// out the pak system.
static void** s_allocator = nullptr;
// virtual functions of the allocator: allocate, followed by free, and the pak system
static DWORD s_allocateSlot = 0;
static DWORD s_pakSlot = 0;

struct Edit {
    std::wstring dialogName;
    void (*apply)(XmlNode& dialog);
};
static std::vector<Edit> s_edits;
// files of older clients that hold every dialog of a screen
static const char* const SCREEN_FILES[] = { "ui_game.xml" };
// whether text files can be read through the client's pak system and handed to the binary parser
static bool s_textFiles = false;

static thread_local const char* t_loadingPath = nullptr;

static bool ReadVarint(const BYTE* data, size_t size, size_t& at, DWORD& value) {
    value = 0;
    for (int shift = 0; at < size && shift < 35; shift += 7) {
        BYTE c = data[at++];
        value |= (DWORD)(c & 0x7F) << shift;
        if (!(c & 0x80)) {
            return true;
        }
    }
    return false;
}

static void WriteVarint(std::vector<BYTE>& out, DWORD value) {
    do {
        BYTE c = value & 0x7F;
        value >>= 7;
        out.push_back(value ? c | 0x80 : c);
    } while (value);
}

/// The binary format: 0x80, the size of a UTF-16 string pool in bytes, the pool, then the node tree. A node is its name offset,
/// a kind (1 text, 2 attributes, 4 children), the attributes as a count and name/value pairs, the text and the children.
/// All strings are offsets in UTF-16 units into the pool.
class BinaryXmlReader {
public:
    BinaryXmlReader(const BYTE* data, size_t size) : data(data), size(size) {}

    bool Read(XmlNode& root) {
        size_t at = 1;
        DWORD poolSize;
        if (size < 1 || data[0] != 0x80 || !ReadVarint(data, size, at, poolSize) || at + poolSize > size) {
            return false;
        }
        pool = (const wchar_t*)(data + at);
        poolUnits = poolSize / 2;
        at += poolSize;
        return ReadNode(at, root, 0);
    }

private:
    const BYTE* data;
    size_t size;
    const wchar_t* pool = nullptr;
    size_t poolUnits = 0;

    bool String(size_t& at, std::wstring& value) {
        DWORD offset;
        if (!ReadVarint(data, size, at, offset) || offset >= poolUnits) {
            return false;
        }
        size_t end = offset;
        while (end < poolUnits && pool[end]) {
            end++;
        }
        value.assign(pool + offset, end - offset);
        return end < poolUnits;
    }

    bool ReadNode(size_t& at, XmlNode& node, int depth) {
        DWORD kind, count;
        if (depth > 64 || !String(at, node.name) || !ReadVarint(data, size, at, kind) || kind & ~7u) {
            return false;
        }
        if (kind & 2) {
            if (!ReadVarint(data, size, at, count)) {
                return false;
            }
            node.attributes.resize(count);
            for (auto& attribute : node.attributes) {
                if (!String(at, attribute.first) || !String(at, attribute.second)) {
                    return false;
                }
            }
        }
        node.hasText = kind & 1;
        if (node.hasText && !String(at, node.text)) {
            return false;
        }
        if (kind & 4) {
            if (!ReadVarint(data, size, at, count)) {
                return false;
            }
            node.children.resize(count);
            for (XmlNode& child : node.children) {
                if (!ReadNode(at, child, depth + 1)) {
                    return false;
                }
            }
        }
        return true;
    }
};

/// Writes the pool in the order the client's own files have it: the empty string, then every string as the tree first uses it.
class BinaryXmlWriter {
public:
    std::vector<BYTE> Write(const XmlNode& root) {
        Offset(L"");
        CollectStrings(root);
        std::vector<BYTE> tree;
        WriteNode(tree, root);
        std::vector<BYTE> out;
        out.push_back(0x80);
        WriteVarint(out, (DWORD)(pool.size() * 2));
        const BYTE* poolBytes = (const BYTE*)pool.data();
        out.insert(out.end(), poolBytes, poolBytes + pool.size() * 2);
        out.insert(out.end(), tree.begin(), tree.end());
        return out;
    }

private:
    std::wstring pool;
    std::map<std::wstring, DWORD> offsets;

    DWORD Offset(const std::wstring& value) {
        auto found = offsets.find(value);
        if (found != offsets.end()) {
            return found->second;
        }
        DWORD offset = (DWORD)pool.size();
        offsets[value] = offset;
        pool.append(value);
        pool.push_back(L'\0');
        return offset;
    }

    void CollectStrings(const XmlNode& node) {
        Offset(node.name);
        for (const auto& attribute : node.attributes) {
            Offset(attribute.first);
            Offset(attribute.second);
        }
        if (node.hasText) {
            Offset(node.text);
        }
        for (const XmlNode& child : node.children) {
            CollectStrings(child);
        }
    }

    void WriteNode(std::vector<BYTE>& out, const XmlNode& node) {
        DWORD kind = (node.hasText ? 1 : 0) | (node.attributes.empty() ? 0 : 2) | (node.children.empty() ? 0 : 4);
        WriteVarint(out, Offset(node.name));
        WriteVarint(out, kind);
        if (kind & 2) {
            WriteVarint(out, (DWORD)node.attributes.size());
            for (const auto& attribute : node.attributes) {
                WriteVarint(out, Offset(attribute.first));
                WriteVarint(out, Offset(attribute.second));
            }
        }
        if (kind & 1) {
            WriteVarint(out, Offset(node.text));
        }
        if (kind & 4) {
            WriteVarint(out, (DWORD)node.children.size());
            for (const XmlNode& child : node.children) {
                WriteNode(out, child);
            }
        }
    }
};

const std::wstring* XmlNode::Attribute(const wchar_t* key) const {
    for (const auto& attribute : attributes) {
        if (attribute.first == key) {
            return &attribute.second;
        }
    }
    return nullptr;
}

void XmlNode::SetAttribute(const wchar_t* key, const std::wstring& value) {
    for (auto& attribute : attributes) {
        if (attribute.first == key) {
            attribute.second = value;
            return;
        }
    }
    attributes.emplace_back(key, value);
}

XmlNode* XmlNode::ChildNamed(const std::wstring& nameAttribute) {
    for (XmlNode& child : children) {
        const std::wstring* name = child.Attribute(L"name");
        if (name && *name == nameAttribute) {
            return &child;
        }
    }
    return nullptr;
}

static const char* FileName(const char* path) {
    const char* name = path;
    for (const char* c = path; *c; c++) {
        if (*c == '/' || *c == '\\') {
            name = c + 1;
        }
    }
    return name;
}

/// Reads the text format the client also accepts, UTF-16 or UTF-8: the root element with its attributes, text and children.
/// The declaration, comments and processing instructions are skipped.
class TextXmlReader {
public:
    explicit TextXmlReader(std::wstring text) : text(std::move(text)) {}

    bool Read(XmlNode& root) {
        if (!SkipProlog() || !ReadElement(root, 0)) {
            return false;
        }
        return true;
    }

private:
    std::wstring text;
    size_t at = 0;

    bool StartsWith(const wchar_t* prefix) const {
        return text.compare(at, wcslen(prefix), prefix) == 0;
    }

    void SkipSpace() {
        while (at < text.size() && iswspace(text[at])) {
            at++;
        }
    }

    bool SkipPast(const wchar_t* end) {
        size_t found = text.find(end, at);
        if (found == std::wstring::npos) {
            return false;
        }
        at = found + wcslen(end);
        return true;
    }

    /// Skips whitespace, comments and markup that is not an element. false at the end of the text.
    bool SkipMisc() {
        for (;;) {
            SkipSpace();
            if (StartsWith(L"<!--")) {
                if (!SkipPast(L"-->")) {
                    return false;
                }
            } else if (StartsWith(L"<?") || StartsWith(L"<!")) {
                if (!SkipPast(L">")) {
                    return false;
                }
            } else {
                return at < text.size();
            }
        }
    }

    bool SkipProlog() {
        return SkipMisc() && text[at] == L'<';
    }

    static bool IsNameChar(wchar_t c) {
        return iswalnum(c) || c == L'_' || c == L'-' || c == L'.' || c == L':';
    }

    bool ReadName(std::wstring& name) {
        size_t start = at;
        while (at < text.size() && IsNameChar(text[at])) {
            at++;
        }
        name.assign(text, start, at - start);
        return !name.empty();
    }

    static std::wstring Unescape(const std::wstring& raw) {
        std::wstring out;
        out.reserve(raw.size());
        for (size_t i = 0; i < raw.size(); i++) {
            size_t end = raw[i] == L'&' ? raw.find(L';', i) : std::wstring::npos;
            if (end == std::wstring::npos || end - i > 10) {
                out.push_back(raw[i]);
                continue;
            }
            std::wstring entity = raw.substr(i + 1, end - i - 1);
            if (entity == L"lt") {
                out.push_back(L'<');
            } else if (entity == L"gt") {
                out.push_back(L'>');
            } else if (entity == L"amp") {
                out.push_back(L'&');
            } else if (entity == L"quot") {
                out.push_back(L'"');
            } else if (entity == L"apos") {
                out.push_back(L'\'');
            } else if (entity.size() > 1 && entity[0] == L'#') {
                bool hex = entity[1] == L'x' || entity[1] == L'X';
                out.push_back((wchar_t)wcstoul(entity.c_str() + (hex ? 2 : 1), nullptr, hex ? 16 : 10));
            } else {
                out.push_back(raw[i]);
                continue;
            }
            i = end;
        }
        return out;
    }

    static std::wstring Trim(const std::wstring& value) {
        size_t begin = 0, end = value.size();
        while (begin < end && iswspace(value[begin])) {
            begin++;
        }
        while (end > begin && iswspace(value[end - 1])) {
            end--;
        }
        return value.substr(begin, end - begin);
    }

    bool ReadElement(XmlNode& node, int depth) {
        if (depth > 64 || at >= text.size() || text[at] != L'<') {
            return false;
        }
        at++;
        if (!ReadName(node.name)) {
            return false;
        }
        for (;;) {
            SkipSpace();
            if (at >= text.size()) {
                return false;
            }
            if (StartsWith(L"/>")) {
                at += 2;
                return true;
            }
            if (text[at] == L'>') {
                at++;
                break;
            }
            std::wstring key;
            if (!ReadName(key)) {
                return false;
            }
            SkipSpace();
            if (at >= text.size() || text[at] != L'=') {
                return false;
            }
            at++;
            SkipSpace();
            wchar_t quote = at < text.size() ? text[at] : 0;
            size_t end = quote == L'"' || quote == L'\'' ? text.find(quote, at + 1) : std::wstring::npos;
            if (end == std::wstring::npos) {
                return false;
            }
            node.attributes.emplace_back(key, Unescape(text.substr(at + 1, end - at - 1)));
            at = end + 1;
        }
        std::wstring content;
        for (;;) {
            if (at >= text.size()) {
                return false;
            }
            if (StartsWith(L"</")) {
                at += 2;
                std::wstring closing;
                if (!ReadName(closing) || closing != node.name) {
                    return false;
                }
                SkipSpace();
                if (at >= text.size() || text[at] != L'>') {
                    return false;
                }
                at++;
                break;
            }
            if (StartsWith(L"<!--")) {
                if (!SkipPast(L"-->")) {
                    return false;
                }
            } else if (StartsWith(L"<![CDATA[")) {
                size_t end = text.find(L"]]>", at);
                if (end == std::wstring::npos) {
                    return false;
                }
                content.append(text, at + 9, end - at - 9);
                at = end + 3;
            } else if (StartsWith(L"<?")) {
                if (!SkipPast(L"?>")) {
                    return false;
                }
            } else if (text[at] == L'<') {
                node.children.emplace_back();
                if (!ReadElement(node.children.back(), depth + 1)) {
                    return false;
                }
            } else {
                size_t end = text.find(L'<', at);
                if (end == std::wstring::npos) {
                    return false;
                }
                content.append(Unescape(text.substr(at, end - at)));
                at = end;
            }
        }
        std::wstring trimmed = Trim(content);
        node.hasText = !trimmed.empty();
        node.text = trimmed;
        return true;
    }
};

/// The text of a file in UTF-16 (with or without a byte order mark) or UTF-8.
static bool DecodeText(const std::vector<BYTE>& data, std::wstring& text) {
    size_t size = data.size();
    if (size >= 2 && data[0] == 0xFF && data[1] == 0xFE) {
        text.assign((const wchar_t*)(data.data() + 2), (size - 2) / 2);
        return true;
    }
    if (size >= 2 && data[0] == '<' && data[1] == 0) {
        text.assign((const wchar_t*)data.data(), size / 2);
        return true;
    }
    size_t skip = size >= 3 && data[0] == 0xEF && data[1] == 0xBB && data[2] == 0xBF ? 3 : 0;
    int units = MultiByteToWideChar(CP_UTF8, 0, (const char*)data.data() + skip, (int)(size - skip), nullptr, 0);
    if (units <= 0) {
        return false;
    }
    text.resize(units);
    MultiByteToWideChar(CP_UTF8, 0, (const char*)data.data() + skip, (int)(size - skip), &text[0], units);
    return true;
}

static bool IsEditedFile(const char* path) {
    const char* name = FileName(path);
    for (const char* screen : SCREEN_FILES) {
        if (_stricmp(name, screen) == 0) {
            return true;
        }
    }
    for (const Edit& edit : s_edits) {
        size_t length = edit.dialogName.size();
        bool same = strlen(name) == length + 4 && _stricmp(name + length, ".xml") == 0;
        for (size_t i = 0; same && i < length; i++) {
            same = towlower((wchar_t)(unsigned char)name[i]) == towlower(edit.dialogName[i]);
        }
        if (same) {
            return true;
        }
    }
    return false;
}

static bool IsDialog(const XmlNode& node, const std::wstring& name) {
    const std::wstring* attribute = node.Attribute(L"name");
    return _wcsicmp(node.name.c_str(), L"Dialog") == 0 && attribute && *attribute == name;
}

/// Applies the edits to the root when it is the dialog, or to the dialogs among its children. Returns how many were applied.
static int ApplyEdits(XmlNode& root) {
    int applied = 0;
    for (const Edit& edit : s_edits) {
        if (IsDialog(root, edit.dialogName)) {
            edit.apply(root);
            applied++;
            continue;
        }
        for (XmlNode& child : root.children) {
            if (IsDialog(child, edit.dialogName)) {
                edit.apply(child);
                applied++;
            }
        }
    }
    return applied;
}

typedef BYTE*(__fastcall* Allocate_t)(void* allocator, DWORD size);
typedef void(__fastcall* Free_t)(void* allocator, void* block);

/// A copy of the encoded file in a buffer from the game's allocator, followed by three zero bytes like the loader's.
static BYTE* GameBuffer(const std::vector<BYTE>& encoded) {
    void* allocator = *s_allocator;
    BYTE* buffer = (*(Allocate_t**)allocator)[s_allocateSlot / 8](allocator, (DWORD)encoded.size() + 3);
    if (buffer) {
        memcpy(buffer, encoded.data(), encoded.size());
        memset(buffer + encoded.size(), 0, 3);
    }
    return buffer;
}

/// Reads the whole file through the client's pak system, the way the loader does. false if it cannot be opened.
static bool ReadGameFile(const char* path, std::vector<BYTE>& data) {
    typedef void*(__fastcall* GetPak_t)(void* system);
    typedef void*(__fastcall* Open_t)(void* pak, const char* path, const char* mode, int flags);
    typedef int(__fastcall* Seek_t)(void* pak, void* file, long offset, int origin);
    typedef long(__fastcall* Tell_t)(void* pak, void* file);
    typedef size_t(__fastcall* Read_t)(void* pak, void* buffer, size_t size, size_t count, void* file);
    typedef int(__fastcall* Close_t)(void* pak, void* file);
    void* system = *s_allocator;
    void* pak = system ? (*(GetPak_t**)system)[s_pakSlot / 8](system) : nullptr;
    if (!pak) {
        return false;
    }
    void** table = *(void***)pak;
    void* file = ((Open_t)table[0x78 / 8])(pak, path, "rb", 0);
    if (!file) {
        return false;
    }
    ((Seek_t)table[0x98 / 8])(pak, file, 0, SEEK_END);
    long size = ((Tell_t)table[0xA0 / 8])(pak, file);
    ((Seek_t)table[0x98 / 8])(pak, file, 0, SEEK_SET);
    data.resize(size > 0 ? size : 0);
    size_t read = size > 0 ? ((Read_t)table[0x80 / 8])(pak, data.data(), 1, size, file) : 0;
    ((Close_t)table[0xA8 / 8])(pak, file);
    return size > 0 && read == (size_t)size;
}

/// A text file with a dialog to change goes to the binary parser, edited and encoded: the client's text path cannot be
/// handed a new buffer. Binary files are edited when they reach the parser.
static __int64 __fastcall zzLoadXmlFile(void* document, const char* path) {
    if (s_textFiles && path && IsEditedFile(path)) {
        std::vector<BYTE> data;
        std::wstring text;
        XmlNode root;
        if (ReadGameFile(path, data) && data[0] != 0x80) {
            if (DecodeText(data, text) && TextXmlReader(std::move(text)).Read(root)) {
                int applied = ApplyEdits(root);
                std::vector<BYTE> encoded = applied ? BinaryXmlWriter().Write(root) : std::vector<BYTE>();
                BYTE* buffer = applied ? GameBuffer(encoded) : nullptr;
                if (buffer) {
                    XmlBuffer binary = { buffer, (DWORD)encoded.size() + 3, 0, 0 };
                    ModsLog("ui xml: %s edited (%d dialogs, from text)", path, applied);
                    return real_ParseBinaryXml(document, &binary);
                }
            } else {
                ModsLog("ui xml: %s could not be read as text", path);
            }
        }
    }
    const char* outer = t_loadingPath;
    t_loadingPath = path;
    __int64 result = real_LoadXmlFile(document, path);
    t_loadingPath = outer;
    return result;
}

static __int64 __fastcall zzParseBinaryXml(void* document, XmlBuffer* buffer) {
    const char* path = t_loadingPath;
    if (!path || !IsEditedFile(path)) {
        return real_ParseBinaryXml(document, buffer);
    }
    XmlNode root;
    if (!BinaryXmlReader(buffer->data, buffer->size - 3).Read(root)) {
        ModsLog("ui xml: %s could not be read", path);
        return real_ParseBinaryXml(document, buffer);
    }
    int applied = ApplyEdits(root);
    if (!applied) {
        return real_ParseBinaryXml(document, buffer);
    }
    std::vector<BYTE> encoded = BinaryXmlWriter().Write(root);
    BYTE* replacement = GameBuffer(encoded);
    if (!replacement) {
        return real_ParseBinaryXml(document, buffer);
    }
    void* allocator = *s_allocator;
    (*(Free_t**)allocator)[(s_allocateSlot + 8) / 8](allocator, buffer->data);
    buffer->data = replacement;
    buffer->size = (DWORD)encoded.size() + 3;
    ModsLog("ui xml: %s edited (%d dialogs)", path, applied);
    return real_ParseBinaryXml(document, buffer);
}

void AddUiXmlEdit(const wchar_t* dialogName, void (*apply)(XmlNode& dialog)) {
    s_edits.push_back({ dialogName, apply });
}

/// The XML loader reads the whole file through the pak system into a buffer from the game's allocator and, for the binary
/// format, hands it to the parser right after checking the first byte: cmp byte ptr [buffer], 80h; jnz; ...; call parser.
/// The allocator is the object whose virtual allocate gives that buffer, with free in the next slot: mov rcx, [rip+allocator];
/// ... call [rax+allocate] (390h in 4.x, 3A8h in 5.x). Before it, the loader gets the pak system from the same object the
/// same way (190h, 1A8h).
bool InstallUiXml(HMODULE game) {
    if (s_edits.empty()) {
        return false;
    }
    // followed by a line break in 5.x clients
    static const char CANNOT_OPEN[] = "can not open file %s";
    const void* message = FindBytes(game, CANNOT_OPEN, sizeof(CANNOT_OPEN) - 1);
    BYTE* reference = message ? FindLeaTo(game, message) : nullptr;
    BYTE* loader = reference ? FunctionStart(reference) : nullptr;
    // the checks and calls below sit in chained ranges after the primary one
    BYTE* end = loader ? FunctionBodyEnd(loader, 0x1000) : nullptr;
    BYTE* parser = nullptr;
    for (int pass = 0; pass < 2; pass++) {
        for (BYTE* p = loader; p && p + 8 < end; p++) {
            if (p[0] != 0x48 || p[1] != 0x8B || p[2] != 0x0D || (pass == 1 && (void**)ResolveRip(p + 3, p + 7) != s_allocator)) {
                continue;
            }
            for (BYTE* q = p + 7; q < p + 0x20; q++) {
                DWORD slot = q[0] == 0xFF && q[1] == 0x90 ? *(DWORD*)(q + 2) : 0;
                if (pass == 0 && slot >= 0x300 && slot < 0x400 && !s_allocator) {
                    s_allocator = (void**)ResolveRip(p + 3, p + 7);
                    s_allocateSlot = slot;
                    break;
                }
                if (pass == 1 && slot >= 0x100 && slot < 0x300 && !s_pakSlot) {
                    s_pakSlot = slot;
                    break;
                }
            }
        }
    }
    for (BYTE* p = loader; p && p + 5 < end && !parser; p++) {
        if (p[0] == 0x80 && (p[1] & 0xC0) == 0 && p[2] == 0x80 && p[3] == 0x75) {
            for (BYTE* q = p + 5; q < p + 0x30; q++) {
                if (q[0] == 0xE8) {
                    parser = ResolveRip(q + 1, q + 5);
                    break;
                }
            }
        }
    }
    // the file calls ReadGameFile makes on the pak system: open, seek, tell, read, close, as call [reg+slot]
    static const BYTE PAK_SLOTS[] = { 0x78, 0x98, 0xA0, 0x80, 0xA8 };
    s_textFiles = loader && s_pakSlot;
    for (int i = 0; i < _countof(PAK_SLOTS) && s_textFiles; i++) {
        bool found = false;
        for (BYTE* p = loader; p + 6 < end && !found; p++) {
            BYTE mode = p[1] & 0xF8;
            found = p[0] == 0xFF && (p[1] & 7) != 4
                && ((mode == 0x90 && *(DWORD*)(p + 2) == PAK_SLOTS[i]) || (mode == 0x50 && p[2] == PAK_SLOTS[i]));
        }
        s_textFiles = found;
    }
    ModsLog("ui xml: loader=%p parser=%p allocator=%p+%X pak=+%X text files=%d", loader, parser, s_allocator, s_allocateSlot, s_pakSlot,
        s_textFiles);
    if (!loader || !parser || !s_allocator) {
        return false;
    }
    real_LoadXmlFile = (LoadXmlFile_t)loader;
    real_ParseBinaryXml = (ParseBinaryXml_t)parser;
    DetourAttach(&(PVOID&)real_LoadXmlFile, zzLoadXmlFile);
    DetourAttach(&(PVOID&)real_ParseBinaryXml, zzParseBinaryXml);
    return true;
}
