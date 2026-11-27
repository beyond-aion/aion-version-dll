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

// Listing the directory avoids a separate open for each existing screenshot on a cold disk. Publish a finished image before
// returning: the renderer uses the save result immediately and reports the name it passed in.
typedef __int64(__fastcall* Screenshot_t)(void* renderer, char* name, unsigned char customize);
typedef HRESULT(WINAPI* SaveSurface_t)(const char* file, int format, IDirect3DSurface9* surface, const PALETTEENTRY* palette, const RECT* rect);

static Screenshot_t real_Screenshot = nullptr;
static SaveSurface_t real_SaveSurface = nullptr;
static constexpr int D3DXIFF_JPG = 1;
static constexpr float JPEG_QUALITY = 0.9f;

static thread_local const char* s_screenshotFile = nullptr;

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

/// Claims the lowest free number before the renderer uses it. The sidecar disappears when the handle closes, even if the client exits.
static HANDLE ClaimName(bool customize, char* name, size_t size) {
    int kind = customize ? 1 : 0;
    if (!CreateDirectoryA(FOLDERS[kind], nullptr) && GetLastError() != ERROR_ALREADY_EXISTS) {
        return nullptr;
    }
    std::set<int> taken;
    char pattern[MAX_PATH];
    sprintf_s(pattern, "%s\\%s*.jpg", FOLDERS[kind], PREFIXES[kind]);
    _finddata_t found;
    intptr_t search = _findfirst(pattern, &found);
    size_t prefix = strlen(PREFIXES[kind]);
    if (search != -1) {
        do {
            int number;
            char rest[8];
            if (sscanf_s(found.name + prefix, "%4d%7s", &number, rest, (unsigned)sizeof(rest)) == 2 && _stricmp(rest, ".jpg") == 0) {
                taken.insert(number);
            }
        } while (_findnext(search, &found) == 0);
        _findclose(search);
    }
    for (int number = 0; number < 10000; number++) {
        if (taken.count(number) != 0) {
            continue;
        }
        sprintf_s(name, size, "%s\\%s%04d.jpg", FOLDERS[kind], PREFIXES[kind], number);
        std::string lock = std::string(name) + ".lock";
        HANDLE claim = CreateFileA(lock.c_str(), DELETE, 0, nullptr, OPEN_ALWAYS, FILE_ATTRIBUTE_TEMPORARY | FILE_FLAG_DELETE_ON_CLOSE, nullptr);
        if (claim == INVALID_HANDLE_VALUE) {
            DWORD error = GetLastError();
            if (error == ERROR_SHARING_VIOLATION || (error == ERROR_ACCESS_DENIED && GetFileAttributesA(lock.c_str()) != INVALID_FILE_ATTRIBUTES)) {
                continue;
            }
            return nullptr;
        }
        if (GetFileAttributesA(name) == INVALID_FILE_ATTRIBUTES && GetLastError() == ERROR_FILE_NOT_FOUND) {
            return claim;
        }
        CloseHandle(claim);
    }
    return nullptr;
}

static __int64 __fastcall zzScreenshot(void* renderer, char* name, unsigned char customize) {
    char own[MAX_PATH];
    std::unique_ptr<void, decltype(&CloseHandle)> claim(name ? nullptr : ClaimName(customize != 0, own, sizeof(own)), CloseHandle);
    if (!claim) {
        return real_Screenshot(renderer, name, customize);
    }
    LONGLONG started = Now();
    const char* previous = s_screenshotFile;
    s_screenshotFile = own;
    __int64 result = real_Screenshot(renderer, own, customize);
    s_screenshotFile = previous;
    ModsLog("screenshot: %s, %.1f ms in the frame", own, MillisSince(started));
    return result;
}

struct ScreenshotImage {
    UINT width;
    UINT height;
    std::vector<BYTE> pixels;
};

/// Encodes the image as JPEG with WIC and writes it under a temporary name, renamed once complete.
static bool WriteJpeg(const ScreenshotImage& image, const std::string& temporary) {
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
        ok = SUCCEEDED(frame->Initialize(options)) && SUCCEEDED(frame->SetSize(image.width, image.height))
            && SUCCEEDED(frame->SetPixelFormat(&format)) && IsEqualGUID(format, GUID_WICPixelFormat24bppBGR);
    }
    // the JPEG encoder takes 24-bit pixels: the fourth byte of each goes, a band of rows at a time
    static constexpr UINT BAND = 64;
    std::vector<BYTE> band(ok ? (size_t)image.width * 3 * BAND : 0);
    for (UINT row = 0; ok && row < image.height; row += BAND) {
        UINT rows = min(BAND, image.height - row);
        const BYTE* source = image.pixels.data() + (size_t)row * image.width * 4;
        for (size_t i = 0, count = (size_t)rows * image.width; i < count; i++) {
            band[i * 3] = source[i * 4];
            band[i * 3 + 1] = source[i * 4 + 1];
            band[i * 3 + 2] = source[i * 4 + 2];
        }
        ok = SUCCEEDED(frame->WritePixels(rows, image.width * 3, image.width * 3 * rows, band.data()));
    }
    ok = ok && SUCCEEDED(frame->Commit()) && SUCCEEDED(encoder->Commit());
    for (IUnknown* object : std::initializer_list<IUnknown*>{ options, frame, encoder, stream, factory }) {
        if (object) {
            object->Release();
        }
    }
    return ok;
}

/// Copies the back buffer into memory: resolved first if multisampled, then through a surface in system memory. Only 32-bit formats.
static std::unique_ptr<ScreenshotImage> CopyBackBuffer(IDirect3DSurface9* surface) {
    D3DSURFACE_DESC desc;
    IDirect3DDevice9* device = nullptr;
    if (FAILED(surface->GetDesc(&desc)) || (desc.Format != D3DFMT_X8R8G8B8 && desc.Format != D3DFMT_A8R8G8B8)
        || FAILED(surface->GetDevice(&device))) {
        return nullptr;
    }
    IDirect3DSurface9* resolved = nullptr;
    IDirect3DSurface9* copy = nullptr;
    IDirect3DSurface9* source = surface;
    std::unique_ptr<ScreenshotImage> image;
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
        image.reset(new ScreenshotImage{ desc.Width, desc.Height, std::vector<BYTE>((size_t)desc.Width * desc.Height * 4) });
        for (UINT row = 0; row < desc.Height; row++) {
            memcpy(image->pixels.data() + (size_t)row * desc.Width * 4, (const BYTE*)locked.pBits + (size_t)row * locked.Pitch, desc.Width * 4);
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
    return image;
}

static HRESULT WINAPI zzSaveSurface(const char* file, int format, IDirect3DSurface9* surface, const PALETTEENTRY* palette, const RECT* rect) {
    if (!s_screenshotFile || !file || strcmp(file, s_screenshotFile) != 0 || format != D3DXIFF_JPG || palette || rect || !surface) {
        return real_SaveSurface(file, format, surface, palette, rect);
    }

    char suffix[40];
    sprintf_s(suffix, ".%lu.%lu.part", GetCurrentProcessId(), GetCurrentThreadId());
    std::string temporary = std::string(file) + suffix;
    std::unique_ptr<ScreenshotImage> image = CopyBackBuffer(surface);
    bool encoded = false;
    if (image) {
        HRESULT com = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
        if (SUCCEEDED(com) || com == RPC_E_CHANGED_MODE) {
            encoded = WriteJpeg(*image, temporary);
        }
        if (SUCCEEDED(com)) {
            CoUninitialize();
        }
    }
    HRESULT result = S_OK;
    if (!encoded) {
        DeleteFileA(temporary.c_str());
        result = real_SaveSurface(temporary.c_str(), format, surface, palette, rect);
    }

    // Another client can finish a screenshot under the same name first. The renderer reports this exact name, so keep the existing file.
    if (SUCCEEDED(result) && !MoveFileExA(temporary.c_str(), file, 0)) {
        result = HRESULT_FROM_WIN32(GetLastError());
    }
    if (FAILED(result)) {
        DeleteFileA(temporary.c_str());
        ModsLog("screenshot: %s could not be saved (0x%08lX)", file, (unsigned long)result);
    }
    return result;
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
