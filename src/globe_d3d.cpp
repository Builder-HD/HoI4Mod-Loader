// globe (NOTES.md Parts S and T): the shaders' channel. The loader's own constant buffer (register
// b13; vanilla uses b0..b4) and its textures (vanilla's samplers use t0..t15): the background image
// (t20), the planet names (t21, made by the plugin with GDI) and the planets' overview images (t22,
// one texture of 512 x 512 cells) on the game's Direct3D 11 device, bound to the vertex and pixel stages whenever the game
// sets a vertex shader, the constants refreshed there when they changed. API hooks only (d3d11.dll's
// D3D11CreateDevice and the context's VSSetShader), no game code. Without a DX11 device (DX9 or
// OpenGL renderer) Ready() stays false and the globe stays off.
#include <windows.h>
#include <d3d11.h>
#include <string.h>
#include <wincodec.h>

#include <atomic>
#include <string>
#include <vector>

#include "MinHook.h"
#include "engine.h"
#include "globe.h"
#include "globe_d3d.h"
#include "loader.h"

namespace GlobeD3D {

namespace {

using Create_t = HRESULT(WINAPI*)(IDXGIAdapter*, D3D_DRIVER_TYPE, HMODULE, UINT, const D3D_FEATURE_LEVEL*, UINT, UINT,
                                  ID3D11Device**, D3D_FEATURE_LEVEL*, ID3D11DeviceContext**);
using VSSetShader_t = void(STDMETHODCALLTYPE*)(ID3D11DeviceContext*, ID3D11VertexShader*, ID3D11ClassInstance* const*, UINT);

const UINT kSlot = 13;          // constant buffer
const UINT kTextureSlot = 20;   // background image
const UINT kNamesSlot = 21;     // planet names
const UINT kImagesSlot = 22;    // planet images
const UINT kCell = 512;         // an image's cell
const UINT kCellsPerRow = 4;
const size_t kVSSetShaderSlot = 11;  // ID3D11DeviceContext vtable: IUnknown (3), DeviceChild (4), VSSetConstantBuffers, PSSetShaderResources, PSSetShader, PSSetSamplers, VSSetShader

Create_t g_create = nullptr;
VSSetShader_t g_vsSetShader = nullptr;
void* g_vsSetShaderTarget = nullptr;
std::atomic<ID3D11DeviceContext*> g_ctx{nullptr};
std::atomic<ID3D11Buffer*> g_buf{nullptr};
std::atomic<ID3D11ShaderResourceView*> g_background{nullptr};
std::atomic<ID3D11ShaderResourceView*> g_names{nullptr};
std::atomic<ID3D11ShaderResourceView*> g_images{nullptr};
std::atomic<ID3D11Device*> g_dev{nullptr};
std::atomic<bool> g_ready{false};
std::wstring g_backgroundPath;  // set at init, read when the device is made
std::vector<std::wstring> g_imagePaths;
std::vector<float> g_imageCells;  // 4 per image (u0, v0, du, dv); du = 0: not loaded

SRWLOCK g_lock = SRWLOCK_INIT;
float g_data[sizeof(Globe::Params) / sizeof(float)] = {};
std::atomic<long> g_version{0};
long g_uploaded = -1;  // render thread only
std::atomic<bool> g_bound{false};

void Upload(ID3D11DeviceContext* ctx, ID3D11Buffer* buf) {
    long v = g_version.load();
    if (v != g_uploaded) {
        D3D11_MAPPED_SUBRESOURCE m;
        if (SUCCEEDED(ctx->Map(buf, 0, D3D11_MAP_WRITE_DISCARD, 0, &m))) {
            AcquireSRWLockShared(&g_lock);
            memcpy(m.pData, g_data, sizeof g_data);
            ReleaseSRWLockShared(&g_lock);
            ctx->Unmap(buf, 0);
            g_uploaded = v;
        }
    }
    ctx->VSSetConstantBuffers(kSlot, 1, &buf);
    ctx->PSSetConstantBuffers(kSlot, 1, &buf);
    ID3D11ShaderResourceView* srv = g_background.load();
    if (srv) ctx->PSSetShaderResources(kTextureSlot, 1, &srv);
    if ((srv = g_names.load()) != nullptr) ctx->PSSetShaderResources(kNamesSlot, 1, &srv);
    if ((srv = g_images.load()) != nullptr) ctx->PSSetShaderResources(kImagesSlot, 1, &srv);
}

ID3D11ShaderResourceView* MakeTexture(ID3D11Device* dev, const uint8_t* rgba, UINT w, UINT h, HRESULT* hrOut) {
    ID3D11Texture2D* tex = nullptr;
    ID3D11ShaderResourceView* srv = nullptr;
    D3D11_TEXTURE2D_DESC d = {};
    d.Width = w, d.Height = h, d.MipLevels = 1, d.ArraySize = 1;
    d.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    d.SampleDesc.Count = 1;
    d.Usage = D3D11_USAGE_IMMUTABLE;
    d.BindFlags = D3D11_BIND_SHADER_RESOURCE;
    D3D11_SUBRESOURCE_DATA init = {rgba, w * 4, 0};
    HRESULT hr = dev->CreateTexture2D(&d, &init, &tex);
    if (SUCCEEDED(hr)) hr = dev->CreateShaderResourceView(tex, nullptr, &srv);
    if (tex) tex->Release();
    if (hrOut) *hrOut = hr;
    return SUCCEEDED(hr) ? srv : nullptr;
}

// An image file as RGBA pixels (WIC: PNG, JPEG, BMP, ...), scaled to size x size if size > 0.
HRESULT ReadImage(const std::wstring& path, UINT size, UINT* w, UINT* h, std::vector<uint8_t>* px) {
    HRESULT co = CoInitializeEx(nullptr, COINIT_MULTITHREADED);  // the game's thread may have COM already
    IWICImagingFactory* f = nullptr;
    IWICBitmapDecoder* dec = nullptr;
    IWICBitmapFrameDecode* frame = nullptr;
    IWICBitmapScaler* scaler = nullptr;
    IWICFormatConverter* conv = nullptr;
    HRESULT hr = CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&f));
    if (SUCCEEDED(hr)) hr = f->CreateDecoderFromFilename(path.c_str(), nullptr, GENERIC_READ, WICDecodeMetadataCacheOnLoad, &dec);
    if (SUCCEEDED(hr)) hr = dec->GetFrame(0, &frame);
    IWICBitmapSource* src = frame;
    if (SUCCEEDED(hr) && size > 0) {
        hr = f->CreateBitmapScaler(&scaler);
        if (SUCCEEDED(hr)) hr = scaler->Initialize(frame, size, size, WICBitmapInterpolationModeFant);
        src = scaler;
    }
    if (SUCCEEDED(hr)) hr = f->CreateFormatConverter(&conv);
    if (SUCCEEDED(hr))
        hr = conv->Initialize(src, GUID_WICPixelFormat32bppRGBA, WICBitmapDitherTypeNone, nullptr, 0.0, WICBitmapPaletteTypeCustom);
    if (SUCCEEDED(hr)) hr = conv->GetSize(w, h);
    if (SUCCEEDED(hr) && (*w == 0 || *h == 0 || *w > 8192 || *h > 8192)) hr = E_INVALIDARG;
    if (SUCCEEDED(hr)) {
        px->resize((size_t)*w * *h * 4);
        hr = conv->CopyPixels(nullptr, *w * 4, (UINT)px->size(), px->data());
    }
    if (conv) conv->Release();
    if (scaler) scaler->Release();
    if (frame) frame->Release();
    if (dec) dec->Release();
    if (f) f->Release();
    if (SUCCEEDED(co)) CoUninitialize();
    return hr;
}

// The planets' images in one texture, kCellsPerRow cells of kCell x kCell per row.
ID3D11ShaderResourceView* LoadImages(ID3D11Device* dev) {
    size_t n = g_imagePaths.size();
    g_imageCells.assign(n * 4, 0.0f);
    if (!n) return nullptr;
    UINT cols = (UINT)(n < kCellsPerRow ? n : kCellsPerRow), rows = (UINT)((n + kCellsPerRow - 1) / kCellsPerRow);
    UINT tw = cols * kCell, th = rows * kCell;
    std::vector<uint8_t> atlas((size_t)tw * th * 4, 0);
    int loaded = 0;
    for (size_t i = 0; i < n; ++i) {
        if (g_imagePaths[i].empty()) continue;
        UINT w = 0, h = 0;
        std::vector<uint8_t> px;
        HRESULT hr = ReadImage(g_imagePaths[i], kCell, &w, &h, &px);
        if (FAILED(hr) || w != kCell || h != kCell) {
            Log("globe: the planet image %ls could not be loaded (0x%08lX); that planet shows its map", g_imagePaths[i].c_str(), hr);
            continue;
        }
        UINT cx = (UINT)(i % kCellsPerRow) * kCell, cy = (UINT)(i / kCellsPerRow) * kCell;
        for (UINT y = 0; y < kCell; ++y) memcpy(&atlas[((size_t)(cy + y) * tw + cx) * 4], &px[(size_t)y * kCell * 4], kCell * 4);
        // Half a texel inside the cell, so the filter never reads the neighbour.
        g_imageCells[i * 4 + 0] = (cx + 0.5f) / tw, g_imageCells[i * 4 + 1] = (cy + 0.5f) / th;
        g_imageCells[i * 4 + 2] = (kCell - 1.0f) / tw, g_imageCells[i * 4 + 3] = (kCell - 1.0f) / th;
        ++loaded;
    }
    if (!loaded) return nullptr;
    HRESULT hr;
    ID3D11ShaderResourceView* srv = MakeTexture(dev, atlas.data(), tw, th, &hr);
    if (!srv) {
        Log("globe: the planet images could not go on the device (0x%08lX)", hr);
        g_imageCells.assign(n * 4, 0.0f);
        return nullptr;
    }
    Log("globe: %d planet image(s) in slot t%u (%ux%u)", loaded, kImagesSlot, tw, th);
    return srv;
}

void STDMETHODCALLTYPE HookVSSetShader(ID3D11DeviceContext* ctx, ID3D11VertexShader* vs, ID3D11ClassInstance* const* ci, UINT n) {
    g_vsSetShader(ctx, vs, ci, n);
    ID3D11Buffer* buf = g_buf.load();
    // Until the first values are published nothing is bound: an unbound buffer reads as zeros, which
    // the shaders take as "flat".
    if (ctx != g_ctx.load() || !buf || !g_bound.load()) return;
    Upload(ctx, buf);
}

// The background image as an RGBA texture. Null (logged) on any failure.
ID3D11ShaderResourceView* LoadBackground(ID3D11Device* dev) {
    if (g_backgroundPath.empty()) return nullptr;
    UINT w = 0, h = 0;
    std::vector<uint8_t> px;
    HRESULT hr = ReadImage(g_backgroundPath, 0, &w, &h, &px);
    ID3D11ShaderResourceView* srv = SUCCEEDED(hr) ? MakeTexture(dev, px.data(), w, h, &hr) : nullptr;
    if (FAILED(hr) || !srv) {
        Log("globe: the background %ls could not be loaded (0x%08lX); the game's sky stays", g_backgroundPath.c_str(), hr);
        return nullptr;
    }
    Log("globe: background %ls (%ux%u) in slot t%u", g_backgroundPath.c_str(), w, h, kTextureSlot);
    return srv;
}

void Capture(ID3D11Device* dev) {
    ID3D11DeviceContext* ctx = nullptr;
    dev->GetImmediateContext(&ctx);  // the reference is kept: the device lives as long as the game
    if (!ctx) return;
    D3D11_BUFFER_DESC d = {};
    d.ByteWidth = sizeof g_data;
    d.Usage = D3D11_USAGE_DYNAMIC;
    d.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
    d.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
    ID3D11Buffer* buf = nullptr;
    HRESULT hr = dev->CreateBuffer(&d, nullptr, &buf);
    if (FAILED(hr) || !buf) {
        Log("globe: cannot create the constant buffer (0x%08lX); the globe stays off", hr);
        return;
    }
    if (!g_vsSetShaderTarget) {
        void* target = (*(void***)ctx)[kVSSetShaderSlot];
        MH_STATUS s = MH_CreateHook(target, (void*)&HookVSSetShader, (void**)&g_vsSetShader);
        if (s == MH_OK) s = MH_EnableHook(target);
        if (s != MH_OK) {
            Log("globe: cannot hook the device context (%s); the globe stays off", MH_StatusToString(s));
            buf->Release();
            return;
        }
        g_vsSetShaderTarget = target;
        Log("globe: device context hooked (%s)", DescribeAddress(target).c_str());
    }
    ID3D11ShaderResourceView* srv = LoadBackground(dev);
    ID3D11ShaderResourceView* images = LoadImages(dev);
    g_uploaded = -1;
    ID3D11Buffer* old = g_buf.exchange(buf);
    ID3D11ShaderResourceView* oldSrv = g_background.exchange(srv);
    ID3D11ShaderResourceView* oldImages = g_images.exchange(images);
    g_dev.store(dev);
    g_ctx.store(ctx);
    if (old) old->Release();
    if (oldSrv) oldSrv->Release();
    if (oldImages) oldImages->Release();
    g_ready.store(true);
    Log("globe: Direct3D 11 device %p, constant buffer in slot b%u", (void*)dev, kSlot);
}

HRESULT WINAPI HookCreate(IDXGIAdapter* a, D3D_DRIVER_TYPE t, HMODULE sw, UINT flags, const D3D_FEATURE_LEVEL* fl, UINT nfl,
                          UINT sdk, ID3D11Device** dev, D3D_FEATURE_LEVEL* got, ID3D11DeviceContext** ctx) {
    HRESULT hr = g_create(a, t, sw, flags, fl, nfl, sdk, dev, got, ctx);
    if (SUCCEEDED(hr) && dev && *dev) Capture(*dev);
    return hr;
}

}  // namespace

bool Install(const std::wstring& backgroundPath, const std::vector<std::wstring>& imagePaths) {
    g_backgroundPath = backgroundPath;
    g_imagePaths = imagePaths;
    HMODULE m = GetModuleHandleW(L"d3d11.dll");
    if (!m) m = LoadLibraryW(L"d3d11.dll");
    void* create = m ? (void*)GetProcAddress(m, "D3D11CreateDevice") : nullptr;
    if (!create) {
        Log("globe: d3d11.dll has no D3D11CreateDevice; the globe stays off");
        return false;
    }
    return HookQueue(create, (void*)&HookCreate, (void**)&g_create, "D3D11CreateDevice (globe)");
}

bool Ready() { return g_ready.load(); }

bool HaveBackground() { return g_background.load() != nullptr; }

bool ImageCell(size_t i, float cell[4]) {
    if (!g_ready.load() || i * 4 + 3 >= g_imageCells.size() || !(g_imageCells[i * 4 + 2] > 0.0f)) return false;
    for (int k = 0; k < 4; ++k) cell[k] = g_imageCells[i * 4 + k];
    return true;
}

bool SetNames(const uint8_t* rgba, int w, int h) {
    ID3D11Device* dev = g_dev.load();
    if (!dev || w <= 0 || h <= 0) return false;
    HRESULT hr;
    ID3D11ShaderResourceView* srv = MakeTexture(dev, rgba, (UINT)w, (UINT)h, &hr);
    if (!srv) {
        Log("globe: the planet names could not go on the device (0x%08lX)", hr);
        return false;
    }
    // The old one may still be bound on the render thread: kept (a few kilobytes, once per game).
    g_names.exchange(srv);
    return true;
}

void Publish(const float* data) {
    AcquireSRWLockExclusive(&g_lock);
    bool same = memcmp(g_data, data, sizeof g_data) == 0;
    if (!same) memcpy(g_data, data, sizeof g_data);
    ReleaseSRWLockExclusive(&g_lock);
    if (!same) {
        g_version.fetch_add(1);
        g_bound.store(true);
    }
}

}  // namespace GlobeD3D
