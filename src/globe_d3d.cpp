// globe (NOTES.md Part S): the shader constants' channel. The loader's own constant buffer (register
// b13; vanilla uses b0..b4) on the game's Direct3D 11 device, bound to the vertex stage whenever the
// game sets a vertex shader and refreshed there when the values changed. API hooks only
// (d3d11.dll's D3D11CreateDevice and the context's VSSetShader), no game code. Without a DX11 device
// (DX9 or OpenGL renderer) Ready() stays false and the globe stays off.
#include <windows.h>
#include <d3d11.h>
#include <string.h>

#include <atomic>

#include "MinHook.h"
#include "engine.h"
#include "loader.h"

namespace GlobeD3D {

namespace {

using Create_t = HRESULT(WINAPI*)(IDXGIAdapter*, D3D_DRIVER_TYPE, HMODULE, UINT, const D3D_FEATURE_LEVEL*, UINT, UINT,
                                  ID3D11Device**, D3D_FEATURE_LEVEL*, ID3D11DeviceContext**);
using VSSetShader_t = void(STDMETHODCALLTYPE*)(ID3D11DeviceContext*, ID3D11VertexShader*, ID3D11ClassInstance* const*, UINT);

const UINT kSlot = 13;
const size_t kVSSetShaderSlot = 11;  // ID3D11DeviceContext vtable: IUnknown (3), DeviceChild (4), VSSetConstantBuffers, PSSetShaderResources, PSSetShader, PSSetSamplers, VSSetShader

Create_t g_create = nullptr;
VSSetShader_t g_vsSetShader = nullptr;
void* g_vsSetShaderTarget = nullptr;
std::atomic<ID3D11DeviceContext*> g_ctx{nullptr};
std::atomic<ID3D11Buffer*> g_buf{nullptr};
std::atomic<bool> g_ready{false};

SRWLOCK g_lock = SRWLOCK_INIT;
float g_data[12] = {};
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
}

void STDMETHODCALLTYPE HookVSSetShader(ID3D11DeviceContext* ctx, ID3D11VertexShader* vs, ID3D11ClassInstance* const* ci, UINT n) {
    g_vsSetShader(ctx, vs, ci, n);
    ID3D11Buffer* buf = g_buf.load();
    // Until the first values are published nothing is bound: an unbound buffer reads as zeros, which
    // the shader takes as "flat".
    if (ctx != g_ctx.load() || !buf || !g_bound.load()) return;
    Upload(ctx, buf);
}

void Capture(ID3D11Device* dev) {
    ID3D11DeviceContext* ctx = nullptr;
    dev->GetImmediateContext(&ctx);  // the reference is kept: the device lives as long as the game
    if (!ctx) return;
    D3D11_BUFFER_DESC d = {};
    d.ByteWidth = 48;
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
    g_uploaded = -1;
    ID3D11Buffer* old = g_buf.exchange(buf);
    g_ctx.store(ctx);
    if (old) old->Release();
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

bool Install() {
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

void Publish(const float a[4], const float b[4], const float c[4]) {
    AcquireSRWLockExclusive(&g_lock);
    bool same = memcmp(g_data, a, 16) == 0 && memcmp(g_data + 4, b, 16) == 0 && memcmp(g_data + 8, c, 16) == 0;
    if (!same) {
        memcpy(g_data, a, 16);
        memcpy(g_data + 4, b, 16);
        memcpy(g_data + 8, c, 16);
    }
    ReleaseSRWLockExclusive(&g_lock);
    if (!same) {
        g_version.fetch_add(1);
        g_bound.store(true);
    }
}

}  // namespace GlobeD3D
