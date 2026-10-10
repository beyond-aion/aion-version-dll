#include "mods.h"
#include "ping.h"
#include <d3d9.h>
#include <stdio.h>
#include <vector>
#include "detours.h"
#include "dxvk_hud_font.h"
#include <math.h>

// Text size of the DXVK HUD, and the chat tab text color for the label.
static constexpr int FONT_SIZE = 16;
static constexpr D3DCOLOR LABEL_COLOR = D3DCOLOR_XRGB(218, 214, 174);
static const wchar_t LABEL[] = L"Ping: ";

typedef IDirect3D9*(WINAPI* Direct3DCreate9_t)(UINT);
typedef HRESULT(STDMETHODCALLTYPE* CreateDevice_t)(IDirect3D9*, UINT, D3DDEVTYPE, HWND, DWORD, D3DPRESENT_PARAMETERS*, IDirect3DDevice9**);
typedef HRESULT(STDMETHODCALLTYPE* Present_t)(IDirect3DDevice9*, const RECT*, const RECT*, HWND, const RGNDATA*);
typedef HRESULT(STDMETHODCALLTYPE* Reset_t)(IDirect3DDevice9*, D3DPRESENT_PARAMETERS*);

static Direct3DCreate9_t real_Direct3DCreate9 = nullptr;
static CreateDevice_t real_CreateDevice = nullptr;
static Present_t real_Present = nullptr;
static Reset_t real_Reset = nullptr;

struct TextTexture {
    IDirect3DDevice9* device = nullptr;
    IDirect3DTexture9* texture = nullptr;
    int width = 0;
    int height = 0;
    int fontSize = 0;
    int originX = 0;
    int originY = 0;
    wchar_t text[64] = {};
    D3DCOLOR color = 0;

    void Release() {
        if (texture) {
            texture->Release();
            texture = nullptr;
        }
        device = nullptr;
        text[0] = 0;
    }
};

static TextTexture s_pingText;

// Where the text was drawn last, in back buffer pixels, and the size of the back buffer, for dragging it with the mouse.
static volatile LONG s_textLeft, s_textTop, s_textWidth, s_textHeight;
static volatile LONG s_backBufferWidth, s_backBufferHeight;
static volatile bool s_textVisible = false;

static HWND s_window = nullptr;
static WNDPROC real_WindowProc = nullptr;
static bool s_dragging = false;
static POINT s_dragOffset;

// The text is placed like the DXVK HUD places it: its position is the start of the base line, and the texture reaches
// PADDING pixels around the glyphs of the font's full height.
static constexpr int PADDING = 3;

static float SrgbToLinear(int c) {
    float v = c / 255.0f;
    return v <= 0.04045f ? v / 12.92f : powf((v + 0.055f) / 1.055f, 2.4f);
}

static int LinearToSrgb(float v) {
    v = min(1.0f, max(0.0f, v));
    float s = v <= 0.0031308f ? v * 12.92f : 1.055f * powf(v, 1 / 2.4f) - 0.055f;
    return (int)(s * 255 + 0.5f);
}

/// Bilinear sample of the font's distance field at texel coordinates, as the GPU does with a linear sampler.
static float SampleFont(float u, float v) {
    const auto& font = dxvk_hud::g_hudFont;
    u -= 0.5f;
    v -= 0.5f;
    int x0 = (int)floorf(u), y0 = (int)floorf(v);
    float fx = u - x0, fy = v - y0;
    auto texel = [&](int x, int y) {
        x = min((int)font.width - 1, max(0, x));
        y = min((int)font.height - 1, max(0, y));
        return font.texture[y * font.width + x] / 255.0f;
    };
    float top = texel(x0, y0) * (1 - fx) + texel(x0 + 1, y0) * fx;
    float bottom = texel(x0, y0 + 1) * (1 - fx) + texel(x0 + 1, y0 + 1) * fx;
    return top * (1 - fy) + bottom * fy;
}

/// Coverage of the distance field like the DXVK text shader: a positive bias grows the shape (used for the shadow).
static float Coverage(float distance, float bias, float sizeFactor) {
    const float range = (float)dxvk_hud::g_hudFont.falloff;
    // the shader divides by fwidth of pixel texture coordinates, which is 1 / sizeFactor on each axis
    float d = (distance + bias - 0.5f) * (2 * range * sizeFactor);
    return min(1.0f, max(0.0f, d + 0.5f));
}

static const dxvk_hud::HudGlyph* FindGlyph(wchar_t c) {
    const auto& font = dxvk_hud::g_hudFont;
    for (uint32_t i = 0; i < font.charCount; i++) {
        if (font.glyphs[i].codePoint == (uint32_t)c) {
            return &font.glyphs[i];
        }
    }
    return nullptr;
}

/// Renders the label and the value the way the DXVK HUD renders its text: its signed distance field font, a black shadow
/// around the glyphs, mixed in linear color and written as sRGB.
static bool RenderText(IDirect3DDevice9* device, TextTexture& target, const wchar_t* value, D3DCOLOR valueColor, int fontSize) {
    wchar_t text[64];
    swprintf_s(text, L"%s%s", LABEL, value);
    if (target.device == device && target.color == valueColor && target.fontSize == fontSize && wcscmp(target.text, text) == 0) {
        return target.texture != nullptr;
    }
    target.Release();

    const auto& font = dxvk_hud::g_hudFont;
    float sizeFactor = (float)fontSize / font.size;
    int length = (int)wcslen(text);
    int labelLength = (int)wcslen(LABEL);
    int ascent = (int)ceilf(font.size * sizeFactor);
    int width = (int)ceilf(font.advance * sizeFactor * length) + 2 * PADDING;
    int height = (int)ceilf(font.size * 1.4f * sizeFactor) + 2 * PADDING;
    float originX = (float)PADDING, originY = (float)(PADDING + ascent);

    std::vector<float> red(width * height), green(width * height), blue(width * height), alpha(width * height);
    for (int i = 0; i < length; i++) {
        const dxvk_hud::HudGlyph* glyph = FindGlyph(text[i]);
        if (!glyph) {
            continue;
        }
        D3DCOLOR color = i < labelLength ? LABEL_COLOR : valueColor;
        float r = SrgbToLinear((color >> 16) & 0xFF), g = SrgbToLinear((color >> 8) & 0xFF), b = SrgbToLinear(color & 0xFF);
        float left = originX + sizeFactor * ((int)font.advance * i - glyph->originX);
        float top = originY - sizeFactor * glyph->originY;
        int x0 = max(0, (int)floorf(left)), x1 = min(width, (int)ceilf(left + sizeFactor * glyph->w));
        int y0 = max(0, (int)floorf(top)), y1 = min(height, (int)ceilf(top + sizeFactor * glyph->h));
        for (int y = y0; y < y1; y++) {
            for (int x = x0; x < x1; x++) {
                float u = (x + 0.5f - left) / sizeFactor, v = (y + 0.5f - top) / sizeFactor;
                if (u < 0 || v < 0 || u > glyph->w || v > glyph->h) {
                    continue;
                }
                float distance = SampleFont(glyph->x + u, glyph->y + v);
                float center = Coverage(distance, 0, sizeFactor);
                float shadow = Coverage(distance, 0.25f, sizeFactor);
                int k = y * width + x;
                // glyph quads of neighbours overlap at their borders; keep the stronger one
                red[k] = max(red[k], r * center);
                green[k] = max(green[k], g * center);
                blue[k] = max(blue[k], b * center);
                alpha[k] = max(alpha[k], shadow);
            }
        }
    }

    IDirect3DTexture9* texture = nullptr;
    if (FAILED(device->CreateTexture(width, height, 1, 0, D3DFMT_A8R8G8B8, D3DPOOL_MANAGED, &texture, nullptr))) {
        return false;
    }
    D3DLOCKED_RECT locked;
    if (FAILED(texture->LockRect(0, &locked, nullptr, 0))) {
        texture->Release();
        return false;
    }
    for (int y = 0; y < height; y++) {
        DWORD* row = (DWORD*)((BYTE*)locked.pBits + y * locked.Pitch);
        for (int x = 0; x < width; x++) {
            int k = y * width + x;
            row[x] = D3DCOLOR_ARGB((int)(alpha[k] * 255 + 0.5f), LinearToSrgb(red[k]), LinearToSrgb(green[k]), LinearToSrgb(blue[k]));
        }
    }
    texture->UnlockRect(0);

    target.device = device;
    target.texture = texture;
    target.width = width;
    target.height = height;
    target.originX = (int)originX;
    target.originY = (int)originY;
    target.color = valueColor;
    target.fontSize = fontSize;
    wcsncpy_s(target.text, text, _TRUNCATE);
    return true;
}

struct Vertex {
    float x, y, z, rhw;
    float u, v;
};

// The game's values of every state the text changes, saved before drawing and put back after it.
static IDirect3DDevice9* s_savedStateDevice = nullptr;
static IDirect3DStateBlock9* s_savedState = nullptr;

static void ReleaseSavedState() {
    if (s_savedState) {
        s_savedState->Release();
        s_savedState = nullptr;
    }
    s_savedStateDevice = nullptr;
}

static void SetTextState(IDirect3DDevice9* device, IDirect3DTexture9* texture) {
    device->SetRenderState(D3DRS_ZENABLE, FALSE);
    device->SetRenderState(D3DRS_ZWRITEENABLE, FALSE);
    device->SetRenderState(D3DRS_STENCILENABLE, FALSE);
    device->SetRenderState(D3DRS_SCISSORTESTENABLE, FALSE);
    device->SetRenderState(D3DRS_ALPHATESTENABLE, FALSE);
    device->SetRenderState(D3DRS_ALPHABLENDENABLE, TRUE);
    device->SetRenderState(D3DRS_SEPARATEALPHABLENDENABLE, FALSE);
    device->SetRenderState(D3DRS_BLENDOP, D3DBLENDOP_ADD);
    device->SetRenderState(D3DRS_SRCBLEND, D3DBLEND_SRCALPHA);
    device->SetRenderState(D3DRS_DESTBLEND, D3DBLEND_INVSRCALPHA);
    device->SetRenderState(D3DRS_CULLMODE, D3DCULL_NONE);
    device->SetRenderState(D3DRS_LIGHTING, FALSE);
    device->SetRenderState(D3DRS_FOGENABLE, FALSE);
    device->SetRenderState(D3DRS_FILLMODE, D3DFILL_SOLID);
    device->SetRenderState(D3DRS_COLORWRITEENABLE, 0xF);
    device->SetRenderState(D3DRS_SRGBWRITEENABLE, FALSE);
    device->SetVertexShader(nullptr);
    device->SetPixelShader(nullptr);
    device->SetTexture(0, texture);
    device->SetTextureStageState(0, D3DTSS_COLOROP, D3DTOP_SELECTARG1);
    device->SetTextureStageState(0, D3DTSS_COLORARG1, D3DTA_TEXTURE);
    device->SetTextureStageState(0, D3DTSS_ALPHAOP, D3DTOP_SELECTARG1);
    device->SetTextureStageState(0, D3DTSS_ALPHAARG1, D3DTA_TEXTURE);
    device->SetTextureStageState(0, D3DTSS_TEXCOORDINDEX, 0);
    device->SetTextureStageState(0, D3DTSS_TEXTURETRANSFORMFLAGS, D3DTTFF_DISABLE);
    device->SetTextureStageState(1, D3DTSS_COLOROP, D3DTOP_DISABLE);
    device->SetTextureStageState(1, D3DTSS_ALPHAOP, D3DTOP_DISABLE);
    device->SetSamplerState(0, D3DSAMP_MINFILTER, D3DTEXF_POINT);
    device->SetSamplerState(0, D3DSAMP_MAGFILTER, D3DTEXF_POINT);
    device->SetSamplerState(0, D3DSAMP_MIPFILTER, D3DTEXF_NONE);
    device->SetSamplerState(0, D3DSAMP_ADDRESSU, D3DTADDRESS_CLAMP);
    device->SetSamplerState(0, D3DSAMP_ADDRESSV, D3DTADDRESS_CLAMP);
    device->SetSamplerState(0, D3DSAMP_SRGBTEXTURE, FALSE);
    device->SetFVF(D3DFVF_XYZRHW | D3DFVF_TEX1);
}

/// Records a state block holding just the states the text changes: the render target switch resets the viewport, and drawing from
/// user memory clears stream 0. Capturing and applying it costs far less than a block of the whole device state.
static bool CreateSavedState(IDirect3DDevice9* device) {
    ReleaseSavedState();
    if (FAILED(device->BeginStateBlock())) {
        return false;
    }
    SetTextState(device, nullptr);
    D3DVIEWPORT9 viewport = { 0, 0, 1, 1, 0, 1 };
    device->SetViewport(&viewport);
    device->SetStreamSource(0, nullptr, 0, 0);
    if (FAILED(device->EndStateBlock(&s_savedState))) {
        s_savedState = nullptr;
        return false;
    }
    s_savedStateDevice = device;
    return true;
}

static void DrawTexture(IDirect3DDevice9* device, const TextTexture& t, float x, float y) {
    SetTextState(device, t.texture);
    float left = x - 0.5f, top = y - 0.5f, right = left + t.width, bottom = top + t.height;
    Vertex quad[4] = {
        { left, top, 0, 1, 0, 0 },
        { right, top, 0, 1, 1, 0 },
        { left, bottom, 0, 1, 0, 1 },
        { right, bottom, 0, 1, 1, 1 },
    };
    device->DrawPrimitiveUP(D3DPT_TRIANGLESTRIP, 2, quad, sizeof(Vertex));
}

struct PingStop {
    int ms;
    int r, g, b;
};

// Green up to 60 ms, yellow at 100 ms, red from 200 ms; orange comes in between.
static constexpr PingStop PING_STOPS[] = {
    { 60, 110, 230, 110 },
    { 100, 240, 210, 80 },
    { 200, 240, 90, 80 },
};

static D3DCOLOR PingColor(int ms) {
    if (ms < 0) {
        return D3DCOLOR_XRGB(200, 200, 200);
    }
    const PingStop* stop = PING_STOPS;
    if (ms <= stop->ms) {
        return D3DCOLOR_XRGB(stop->r, stop->g, stop->b);
    }
    for (; stop + 1 < std::end(PING_STOPS); stop++) {
        const PingStop& next = stop[1];
        if (ms < next.ms) {
            float t = (float)(ms - stop->ms) / (next.ms - stop->ms);
            return D3DCOLOR_XRGB((int)(stop->r + (next.r - stop->r) * t + 0.5f), (int)(stop->g + (next.g - stop->g) * t + 0.5f),
                (int)(stop->b + (next.b - stop->b) * t + 0.5f));
        }
    }
    return D3DCOLOR_XRGB(stop->r, stop->g, stop->b);
}

static void DrawOverlay(IDirect3DDevice9* device) {
    if (!IsInWorld()) {
        s_textVisible = false;
        return;
    }
    int ping = GetDisplayedPing();
    wchar_t value[32];
    if (ping < 0) {
        wcscpy_s(value, L"--");
    } else {
        swprintf_s(value, L"%d ms", min(ping, 99999));
    }

    IDirect3DSurface9* backBuffer = nullptr;
    if (FAILED(device->GetBackBuffer(0, 0, D3DBACKBUFFER_TYPE_MONO, &backBuffer))) {
        return;
    }
    D3DSURFACE_DESC desc;
    backBuffer->GetDesc(&desc);
    if (!RenderText(device, s_pingText, value, PingColor(ping), (int)(FONT_SIZE * g_modsConfig.pingScale + 0.5f))) {
        backBuffer->Release();
        return;
    }
    if ((s_savedStateDevice != device || !s_savedState) && !CreateSavedState(device)) {
        backBuffer->Release();
        return;
    }
    s_savedState->Capture();
    IDirect3DSurface9* oldTarget = nullptr;
    device->GetRenderTarget(0, &oldTarget);
    device->SetRenderTarget(0, backBuffer);

    // the configured position is where the base line of the text starts, like positions in the DXVK HUD
    int x = min(g_modsConfig.pingX - s_pingText.originX, max(0, (int)desc.Width - s_pingText.width));
    int y = min(g_modsConfig.pingY - s_pingText.originY, max(0, (int)desc.Height - s_pingText.height));
    if (SUCCEEDED(device->BeginScene())) {
        DrawTexture(device, s_pingText, (float)x, (float)y);
        device->EndScene();
    }
    s_textLeft = x;
    s_textTop = y;
    s_textWidth = s_pingText.width;
    s_textHeight = s_pingText.height;
    s_backBufferWidth = desc.Width;
    s_backBufferHeight = desc.Height;
    s_textVisible = true;

    device->SetRenderTarget(0, oldTarget);
    if (oldTarget) {
        oldTarget->Release();
    }
    s_savedState->Apply();
    backBuffer->Release();
}

/// Mouse position in back buffer pixels, which differ from window pixels when the window is scaled.
static POINT ToBackBuffer(HWND window, LPARAM lParam) {
    POINT p = { (short)LOWORD(lParam), (short)HIWORD(lParam) };
    RECT client;
    if (GetClientRect(window, &client) && client.right > 0 && client.bottom > 0 && s_backBufferWidth > 0) {
        p.x = p.x * s_backBufferWidth / client.right;
        p.y = p.y * s_backBufferHeight / client.bottom;
    }
    return p;
}

static void SavePosition() {
    wchar_t number[16];
    swprintf_s(number, L"%d", g_modsConfig.pingX);
    WritePrivateProfileStringW(L"Ping", L"X", number, g_modsIniPath);
    swprintf_s(number, L"%d", g_modsConfig.pingY);
    WritePrivateProfileStringW(L"Ping", L"Y", number, g_modsIniPath);
}

/// Lets the player drag the ping text with the left mouse button; the new place is kept in mods.ini.
static LRESULT CALLBACK OverlayWindowProc(HWND window, UINT message, WPARAM wParam, LPARAM lParam) {
    switch (message) {
        case WM_LBUTTONDOWN: {
            POINT p = ToBackBuffer(window, lParam);
            if (s_textVisible && p.x >= s_textLeft && p.x < s_textLeft + s_textWidth && p.y >= s_textTop && p.y < s_textTop + s_textHeight) {
                s_dragging = true;
                s_dragOffset = { p.x - s_textLeft, p.y - s_textTop };
                SetCapture(window);
                return 0;
            }
            break;
        }
        case WM_MOUSEMOVE:
            if (s_dragging) {
                POINT p = ToBackBuffer(window, lParam);
                g_modsConfig.pingX = max(s_pingText.originX, (int)(p.x - s_dragOffset.x) + s_pingText.originX);
                g_modsConfig.pingY = max(s_pingText.originY, (int)(p.y - s_dragOffset.y) + s_pingText.originY);
                return 0;
            }
            break;
        case WM_LBUTTONUP:
        case WM_CAPTURECHANGED:
            if (s_dragging) {
                s_dragging = false;
                if (message == WM_LBUTTONUP) {
                    ReleaseCapture();
                }
                SavePosition();
                return 0;
            }
            break;
    }
    return CallWindowProcW(real_WindowProc, window, message, wParam, lParam);
}

static void HookWindow(HWND window) {
    if (!window || s_window) {
        return;
    }
    s_window = window;
    real_WindowProc = (WNDPROC)SetWindowLongPtrW(window, GWLP_WNDPROC, (LONG_PTR)OverlayWindowProc);
    ModsLog("overlay: window %p", window);
}

static HRESULT STDMETHODCALLTYPE zzPresent(IDirect3DDevice9* device, const RECT* src, const RECT* dst, HWND window, const RGNDATA* dirty) {
    DrawOverlay(device);
    return real_Present(device, src, dst, window, dirty);
}

static HRESULT STDMETHODCALLTYPE zzReset(IDirect3DDevice9* device, D3DPRESENT_PARAMETERS* params) {
    s_pingText.Release();
    ReleaseSavedState();
    return real_Reset(device, params);
}

static HRESULT STDMETHODCALLTYPE zzCreateDevice(IDirect3D9* d3d, UINT adapter, D3DDEVTYPE type, HWND window, DWORD flags,
    D3DPRESENT_PARAMETERS* params, IDirect3DDevice9** device) {
    HRESULT result = real_CreateDevice(d3d, adapter, type, window, flags, params, device);
    if (SUCCEEDED(result) && *device && !real_Present) {
        void** vtable = *(void***)*device;
        real_Reset = (Reset_t)vtable[16];
        real_Present = (Present_t)vtable[17];
        DetourTransactionBegin();
        DetourUpdateThread(GetCurrentThread());
        DetourAttach(&(PVOID&)real_Reset, zzReset);
        DetourAttach(&(PVOID&)real_Present, zzPresent);
        LONG error = DetourTransactionCommit();
        ModsLog("overlay: device hooks committed: %ld", error);
        HookWindow(params && params->hDeviceWindow ? params->hDeviceWindow : window);
    }
    return result;
}

static IDirect3D9* WINAPI zzDirect3DCreate9(UINT sdkVersion) {
    IDirect3D9* d3d = real_Direct3DCreate9(sdkVersion);
    if (d3d && !real_CreateDevice) {
        real_CreateDevice = (CreateDevice_t)(*(void***)d3d)[16];
        DetourTransactionBegin();
        DetourUpdateThread(GetCurrentThread());
        DetourAttach(&(PVOID&)real_CreateDevice, zzCreateDevice);
        LONG error = DetourTransactionCommit();
        ModsLog("overlay: CreateDevice hook committed: %ld", error);
    }
    return d3d;
}

/// Must be called inside an open Detours transaction, after d3d9.dll was loaded.
void InstallOverlay() {
    HMODULE d3d9 = GetModuleHandleW(L"d3d9.dll");
    if (!d3d9) {
        d3d9 = LoadLibraryW(L"d3d9.dll");
    }
    real_Direct3DCreate9 = d3d9 ? (Direct3DCreate9_t)GetProcAddress(d3d9, "Direct3DCreate9") : nullptr;
    ModsLog("overlay: Direct3DCreate9=%p", real_Direct3DCreate9);
    if (real_Direct3DCreate9) {
        DetourAttach(&(PVOID&)real_Direct3DCreate9, zzDirect3DCreate9);
    }
}
