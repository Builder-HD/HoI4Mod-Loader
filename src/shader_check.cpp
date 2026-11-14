// Shader patterns, the compile check's compiler (NOTES.md M15): the system's d3dcompiler_47.dll,
// loaded on first use (never from DllMain), so the DLL does not link against it.
#include <windows.h>

#include <d3dcompiler.h>

#include "shader_patterns.h"

namespace ShaderPatterns {

namespace {

pD3DCompile g_compile = nullptr;
bool g_tried = false;

}  // namespace

bool CompileCheck(const std::string& hlsl, std::string* errors, bool* available) {
    if (!g_tried) {
        g_tried = true;
        HMODULE m = LoadLibraryW(L"d3dcompiler_47.dll");
        g_compile = m ? (pD3DCompile)GetProcAddress(m, "D3DCompile") : nullptr;
    }
    *available = g_compile != nullptr;
    errors->clear();
    if (!g_compile) return false;
    ID3DBlob* code = nullptr;
    ID3DBlob* messages = nullptr;
    HRESULT hr = g_compile(hlsl.data(), hlsl.size(), "hoi4_loader_pattern_check", nullptr, nullptr, "main", "ps_5_0",
                           D3DCOMPILE_SKIP_OPTIMIZATION, 0, &code, &messages);
    if (messages) {
        std::string all((const char*)messages->GetBufferPointer(), messages->GetBufferSize());
        // The first error lines are enough for the log; warnings are dropped.
        size_t pos = 0;
        int kept = 0;
        while (pos < all.size() && kept < 3) {
            size_t end = all.find('\n', pos);
            std::string line = all.substr(pos, end == std::string::npos ? std::string::npos : end - pos);
            pos = end == std::string::npos ? all.size() : end + 1;
            if (line.find("error") == std::string::npos) continue;
            if (!errors->empty()) *errors += " | ";
            *errors += line;
            ++kept;
        }
        messages->Release();
    }
    if (code) code->Release();
    if (FAILED(hr) && errors->empty()) *errors = "compile failed (no message)";
    return SUCCEEDED(hr);
}

}  // namespace ShaderPatterns
