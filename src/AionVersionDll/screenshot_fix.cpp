#include "code_patch.h"
#include "module_load.h"
#include "mods_log.h"
#include <d3d9.h>
#include <wincodec.h>
#include <io.h>
#include <stdio.h>
#include <stdlib.h>
#include <set>
#include <string>
#include <vector>
#include <detours/detours.h>
#include <memory>

#pragma comment(lib, "windowscodecs.lib")

// The renderer takes a screenshot in the frame: it opens Screenshot\Aion0000.jpg, Aion0001.jpg and so on until one is missing, which with
// many screenshots on a cold hard disk takes seconds, then has D3DX read the back buffer and encode it as JPEG, about 200 ms at 3840x2160,
// and only then lets the game announce it. Here the name comes from one listing of the folder, the frame only copies the image, and a
// thread of its own encodes and writes it, under a temporary name until it is complete, so that the game never lists half a file.
// Whatever cannot be done so (another format of the back buffer, a failing call) is left to D3DX as before.

typedef __int64(__fastcall* Screenshot_t)(void* renderer, char* name, unsigned char customize);
typedef HRESULT(WINAPI* SaveSurface_t)(const char* file, int format, IDirect3DSurface9* surface, const PALETTEENTRY* palette, const RECT* rect);

static Screenshot_t real_Screenshot = nullptr;
static SaveSurface_t real_SaveSurface = nullptr;
static constexpr int D3DXIFF_JPG = 1;
static constexpr float JPEG_QUALITY = 0.9f;

// set while the renderer takes a screenshot with a name from here, so that its save is the one to take over
static bool s_ownName = false;
// set by the PrintScreen fix for the next screenshot, whose picture goes on the clipboard too
static bool s_copyNext = false;
// the numbers taken in each folder (screenshots, character customisation), listed once
static std::set<int> s_taken[2];
static bool s_listed[2];

static const char* const FOLDERS[] = { "Screenshot", "ScreenshotCustomize" };
static const char* const PREFIXES[] = { "Aion", "AionCustomize" };

static LONGLONG Now() {
    LARGE_INTEGER now;
    QueryPerformanceCounter(&now);
    return now.QuadPart;
}

static double MillisSince(LONGLONG start) {
    LARGE_INTEGER frequency;
    QueryPerformanceFrequency(&frequency);
    return (Now() - start) * 1000.0 / frequency.QuadPart;
}

/// The lowest number not taken in the folder, as the renderer would find it, from one listing of the folder.
static bool NextName(bool customize, char* name, size_t size) {
    int kind = customize ? 1 : 0;
    if (!CreateDirectoryA(FOLDERS[kind], nullptr) && GetLastError() != ERROR_ALREADY_EXISTS) {
        return false;
    }
    if (!s_listed[kind]) {
        s_listed[kind] = true;
        char pattern[MAX_PATH];
        sprintf_s(pattern, "%s\\%s*.jpg", FOLDERS[kind], PREFIXES[kind]);
        _finddata_t found;
        intptr_t search = _findfirst(pattern, &found);
        size_t prefix = strlen(PREFIXES[kind]);
        for (bool more = search != -1; more; more = _findnext(search, &found) == 0) {
            int number;
            char rest[8];
            if (sscanf_s(found.name + prefix, "%4d%7s", &number, rest, (unsigned)sizeof(rest)) == 2 && _stricmp(rest, ".jpg") == 0) {
                s_taken[kind].insert(number);
            }
        }
        if (search != -1) {
            _findclose(search);
        }
    }
    for (int number = 0; number < 10000; number++) {
        if (s_taken[kind].insert(number).second) {
            sprintf_s(name, size, "%s\\%s%04d.jpg", FOLDERS[kind], PREFIXES[kind], number);
            return true;
        }
    }
    return false;
}

static __int64 __fastcall zzScreenshot(void* renderer, char* name, unsigned char customize) {
    char own[MAX_PATH];
    if (name || !NextName(customize != 0, own, sizeof(own))) {
        return real_Screenshot(renderer, name, customize);
    }
    LONGLONG started = Now();
    s_ownName = true;
    __int64 result = real_Screenshot(renderer, own, customize);
    s_ownName = false;
    ModsLog("screenshot: %s, %.1f ms in the frame", own, MillisSince(started));
    return result;
}

/// An image copied from the back buffer, rows of 32-bit pixels, to encode on a thread of its own.
struct ScreenshotJob {
    std::string file;
    UINT width;
    UINT height;
    std::vector<BYTE> pixels;
    bool clipboard = false;
};

/// Puts the picture on the clipboard as a bottom-up DIB of 24-bit pixels: the fourth byte of the back buffer is no alpha, and programs that
/// read 32-bit pixels as such would show it transparent.
static bool CopyToClipboard(const ScreenshotJob& job) {
    size_t stride = ((size_t)job.width * 3 + 3) & ~(size_t)3;
    HGLOBAL memory = GlobalAlloc(GMEM_MOVEABLE, sizeof(BITMAPINFOHEADER) + stride * job.height);
    BITMAPINFOHEADER* header = memory ? (BITMAPINFOHEADER*)GlobalLock(memory) : nullptr;
    if (!header) {
        if (memory) {
            GlobalFree(memory);
        }
        return false;
    }
    *header = {};
    header->biSize = sizeof(BITMAPINFOHEADER);
    header->biWidth = (LONG)job.width;
    header->biHeight = (LONG)job.height;
    header->biPlanes = 1;
    header->biBitCount = 24;
    header->biCompression = BI_RGB;
    header->biSizeImage = (DWORD)(stride * job.height);
    BYTE* rows = (BYTE*)(header + 1);
    for (UINT row = 0; row < job.height; row++) {
        const BYTE* source = job.pixels.data() + (size_t)(job.height - 1 - row) * job.width * 4;
        BYTE* target = rows + row * stride;
        for (UINT x = 0; x < job.width; x++) {
            target[x * 3] = source[x * 4];
            target[x * 3 + 1] = source[x * 4 + 1];
            target[x * 3 + 2] = source[x * 4 + 2];
        }
    }
    GlobalUnlock(memory);
    // a clipboard opened without a window has no owner after EmptyClipboard, and SetClipboardData may then fail: a message-only window
    // of this thread owns it, and the data stays on the clipboard when the window goes
    bool copied = false;
    HWND owner = CreateWindowExW(0, L"STATIC", L"", 0, 0, 0, 0, 0, HWND_MESSAGE, nullptr, nullptr, nullptr);
    if (owner && OpenClipboard(owner)) {
        copied = EmptyClipboard() && SetClipboardData(CF_DIB, memory) != nullptr;
        CloseClipboard();
    }
    if (owner) {
        DestroyWindow(owner);
    }
    if (!copied) {
        GlobalFree(memory);
    }
    return copied;
}

/// Encodes the image as JPEG with WIC and writes it under a temporary name, renamed once complete.
static bool WriteJpeg(const ScreenshotJob& job, const std::string& temporary) {
    IWICImagingFactory* factory = nullptr;
    IWICStream* stream = nullptr;
    IWICBitmapEncoder* encoder = nullptr;
    IWICBitmapFrameEncode* frame = nullptr;
    IPropertyBag2* options = nullptr;
    std::wstring path(temporary.begin(), temporary.end());
    bool ok = SUCCEEDED(CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&factory)))
        && SUCCEEDED(factory->CreateStream(&stream)) && SUCCEEDED(stream->InitializeFromFilename(path.c_str(), GENERIC_WRITE))
        && SUCCEEDED(factory->CreateEncoder(GUID_ContainerFormatJpeg, nullptr, &encoder))
        && SUCCEEDED(encoder->Initialize(stream, WICBitmapEncoderNoCache)) && SUCCEEDED(encoder->CreateNewFrame(&frame, &options));
    if (ok) {
        PROPBAG2 option = {};
        option.pstrName = (LPOLESTR)L"ImageQuality";
        VARIANT value = {};
        value.vt = VT_R4;
        value.fltVal = JPEG_QUALITY;
        options->Write(1, &option, &value);
        WICPixelFormatGUID format = GUID_WICPixelFormat24bppBGR;
        ok = SUCCEEDED(frame->Initialize(options)) && SUCCEEDED(frame->SetSize(job.width, job.height))
            && SUCCEEDED(frame->SetPixelFormat(&format)) && IsEqualGUID(format, GUID_WICPixelFormat24bppBGR);
    }
    // the JPEG encoder takes 24-bit pixels: the fourth byte of each goes, a band of rows at a time
    static constexpr UINT BAND = 64;
    std::vector<BYTE> band(ok ? (size_t)job.width * 3 * BAND : 0);
    for (UINT row = 0; ok && row < job.height; row += BAND) {
        UINT rows = min(BAND, job.height - row);
        const BYTE* source = job.pixels.data() + (size_t)row * job.width * 4;
        for (size_t i = 0, count = (size_t)rows * job.width; i < count; i++) {
            band[i * 3] = source[i * 4];
            band[i * 3 + 1] = source[i * 4 + 1];
            band[i * 3 + 2] = source[i * 4 + 2];
        }
        ok = SUCCEEDED(frame->WritePixels(rows, job.width * 3, job.width * 3 * rows, band.data()));
    }
    ok = ok && SUCCEEDED(frame->Commit()) && SUCCEEDED(encoder->Commit());
    for (IUnknown* object : std::initializer_list<IUnknown*>{ options, frame, encoder, stream, factory }) {
        if (object) {
            object->Release();
        }
    }
    return ok;
}

/// Renames the finished file to its name, never over another file: a second client in the same folder lists it on its own and can take the
/// same number. Then the file takes the next free one, and file says which.
static bool Publish(const std::string& temporary, std::string& file) {
    if (MoveFileExA(temporary.c_str(), file.c_str(), 0)) {
        return true;
    }
    size_t digits = file.size() - 8;  // ...Aion0042.jpg
    int number = atoi(file.c_str() + digits);
    for (int next = number + 1; next < 10000 && GetLastError() == ERROR_ALREADY_EXISTS; next++) {
        char name[8];
        sprintf_s(name, "%04d", next);
        std::string other = file.substr(0, digits) + name + ".jpg";
        if (MoveFileExA(temporary.c_str(), other.c_str(), 0)) {
            ModsLog("screenshot: %s was taken by another client, written as %s", file.c_str(), other.c_str());
            file = other;
            return true;
        }
    }
    return false;
}

static DWORD WINAPI WriteScreenshot(LPVOID parameter) {
    std::unique_ptr<ScreenshotJob> job((ScreenshotJob*)parameter);
    LONGLONG started = Now();
    HRESULT com = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    char suffix[24];
    sprintf_s(suffix, ".%lu.part", GetCurrentProcessId());
    std::string temporary = job->file + suffix;
    bool ok = WriteJpeg(*job, temporary) && Publish(temporary, job->file);
    if (!ok) {
        DeleteFileA(temporary.c_str());
    }
    bool copied = job->clipboard && CopyToClipboard(*job);
    if (job->clipboard && !copied) {
        ModsLog("screenshot: %s could not be put on the clipboard (error %lu)", job->file.c_str(), GetLastError());
    }
    if (SUCCEEDED(com)) {
        CoUninitialize();
    }
    ModsLog("screenshot: %s %s%s in %.0f ms", job->file.c_str(), ok ? "written" : "could not be written", copied ? " and put on the clipboard" : "",
        MillisSince(started));
    return 0;
}

/// Copies the back buffer into memory: resolved first if multisampled, then through a surface in system memory. Only 32-bit formats.
static std::unique_ptr<ScreenshotJob> CopyBackBuffer(IDirect3DSurface9* surface, const char* file) {
    D3DSURFACE_DESC desc;
    IDirect3DDevice9* device = nullptr;
    if (FAILED(surface->GetDesc(&desc)) || (desc.Format != D3DFMT_X8R8G8B8 && desc.Format != D3DFMT_A8R8G8B8)
        || FAILED(surface->GetDevice(&device))) {
        return nullptr;
    }
    IDirect3DSurface9* resolved = nullptr;
    IDirect3DSurface9* copy = nullptr;
    IDirect3DSurface9* source = surface;
    std::unique_ptr<ScreenshotJob> job;
    bool ok = true;
    if (desc.MultiSampleType != D3DMULTISAMPLE_NONE) {
        ok = SUCCEEDED(device->CreateRenderTarget(desc.Width, desc.Height, desc.Format, D3DMULTISAMPLE_NONE, 0, FALSE, &resolved, nullptr))
            && SUCCEEDED(device->StretchRect(surface, nullptr, resolved, nullptr, D3DTEXF_NONE));
        source = resolved;
    }
    D3DLOCKED_RECT locked;
    ok = ok && SUCCEEDED(device->CreateOffscreenPlainSurface(desc.Width, desc.Height, desc.Format, D3DPOOL_SYSTEMMEM, &copy, nullptr))
        && SUCCEEDED(device->GetRenderTargetData(source, copy)) && SUCCEEDED(copy->LockRect(&locked, nullptr, D3DLOCK_READONLY));
    if (ok) {
        job.reset(new ScreenshotJob{ file, desc.Width, desc.Height, std::vector<BYTE>((size_t)desc.Width * desc.Height * 4) });
        for (UINT row = 0; row < desc.Height; row++) {
            memcpy(job->pixels.data() + (size_t)row * desc.Width * 4, (const BYTE*)locked.pBits + (size_t)row * locked.Pitch, desc.Width * 4);
        }
        copy->UnlockRect();
    }
    if (copy) {
        copy->Release();
    }
    if (resolved) {
        resolved->Release();
    }
    device->Release();
    return job;
}

static HRESULT WINAPI zzSaveSurface(const char* file, int format, IDirect3DSurface9* surface, const PALETTEENTRY* palette, const RECT* rect) {
    if (!s_ownName || format != D3DXIFF_JPG || palette || rect || !surface || !file) {
        return real_SaveSurface(file, format, surface, palette, rect);
    }
    std::unique_ptr<ScreenshotJob> job = CopyBackBuffer(surface, file);
    if (job) {
        job->clipboard = s_copyNext;
        s_copyNext = false;
    }
    HANDLE thread = job ? CreateThread(nullptr, 0, WriteScreenshot, job.get(), 0, nullptr) : nullptr;
    if (thread) {
        (void)job.release(); // the thread owns it now
    } else {
        job.reset();
        s_copyNext = false;
        ModsLog("screenshot: %s saved in the frame, the image could not be copied", file);
        return real_SaveSurface(file, format, surface, palette, rect);
    }
    CloseHandle(thread);
    return S_OK;
}

bool CopyNextScreenshot() {
    if (!real_Screenshot || !real_SaveSurface) {
        return false;
    }
    s_copyNext = true;
    return true;
}

namespace {

/// The renderer's screenshot names the file Screenshot\Aion%04d.jpg when it is given none.
BYTE* LocateScreenshot(HMODULE xrender) {
    static const char NAME[] = "Screenshot\\Aion%04d.jpg";
    return FindFunctionByString(xrender, NAME);
}

void ApplyScreenshot(BYTE* const& function) {
    HMODULE d3dx = GetModuleHandleW(L"d3dx9_38.dll");
    real_SaveSurface = d3dx ? (SaveSurface_t)GetProcAddress(d3dx, "D3DXSaveSurfaceToFileA") : nullptr;
    ModsLog("screenshot fix: screenshot at %p, D3DXSaveSurfaceToFileA %p", function, real_SaveSurface);
    if (!function || !real_SaveSurface) {
        return;
    }
    real_Screenshot = (Screenshot_t)function;
    DetourTransactionBegin();
    DetourUpdateThread(GetCurrentThread());
    AttachHook(&(PVOID&)real_Screenshot, zzScreenshot);
    AttachHook(&(PVOID&)real_SaveSurface, zzSaveSurface);
    CommitHooks();
}

void InstallOnRenderer(HMODULE xrender) {
    InstallScope scope("screenshot_fix");
    if (!real_Screenshot) {
        LocateThenApply(xrender, LocateScreenshot, ApplyScreenshot);
    }
}

} // namespace

void InstallScreenshotFix() {
    OnModuleLoad(L"XRenderD3D9.dll", InstallOnRenderer, true);
}
