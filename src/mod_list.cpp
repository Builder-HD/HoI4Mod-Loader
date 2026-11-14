// The launcher's enabled mods, see mod_list.h.
#include "mod_list.h"

#include <windows.h>

#include <algorithm>
#include <cctype>

namespace ModList {

namespace {

bool EqualsNoCase(const std::string& a, const char* b) {
    size_t n = strlen(b);
    if (a.size() != n) return false;
    for (size_t i = 0; i < n; ++i)
        if (std::tolower((unsigned char)a[i]) != std::tolower((unsigned char)b[i])) return false;
    return true;
}

// A descriptor's tokens: quoted strings, bare words, '=', '{', '}'; '#' starts a comment.
std::vector<std::string> Tokens(const std::string& t, std::vector<bool>* quoted) {
    std::vector<std::string> out;
    for (size_t i = 0; i < t.size();) {
        char c = t[i];
        if (std::isspace((unsigned char)c)) {
            ++i;
        } else if (c == '#') {
            while (i < t.size() && t[i] != '\n') ++i;
        } else if (c == '=' || c == '{' || c == '}') {
            out.push_back(std::string(1, c));
            quoted->push_back(false);
            ++i;
        } else if (c == '"') {
            std::string v;
            for (++i; i < t.size() && t[i] != '"'; ++i) v += t[i];
            ++i;
            out.push_back(v);
            quoted->push_back(true);
        } else {
            std::string v;
            while (i < t.size() && !std::isspace((unsigned char)t[i]) && t[i] != '=' && t[i] != '{' && t[i] != '}' && t[i] != '#')
                v += t[i++];
            out.push_back(v);
            quoted->push_back(false);
        }
    }
    return out;
}

bool IsAbsolute(const std::wstring& p) { return p.size() > 2 && (p[1] == L':' || (p[0] == L'\\' && p[1] == L'\\')); }

std::wstring Slashes(std::wstring p) {
    for (wchar_t& c : p)
        if (c == L'/') c = L'\\';
    while (!p.empty() && p.back() == L'\\') p.pop_back();
    return p;
}

}  // namespace

bool ParseDlcLoad(const std::string& json, std::vector<std::string>* modFiles) {
    modFiles->clear();
    size_t k = json.find("\"enabled_mods\"");
    if (k == std::string::npos) return false;
    size_t open = json.find('[', k), close = open == std::string::npos ? open : json.find(']', open);
    if (open == std::string::npos || close == std::string::npos) return false;
    for (size_t i = open + 1; i < close; ++i) {
        if (json[i] != '"') continue;
        std::string v;
        for (++i; i < close && json[i] != '"'; ++i) {
            if (json[i] == '\\' && i + 1 < close) ++i;  // \" \\ \/
            v += json[i];
        }
        if (!v.empty()) modFiles->push_back(v);
    }
    return true;
}

bool ParseDescriptor(const std::string& text, ModDescriptor* out) {
    *out = ModDescriptor{};
    std::vector<bool> quoted;
    std::vector<std::string> t = Tokens(text, &quoted);
    int depth = 0;
    for (size_t i = 0; i < t.size(); ++i) {
        if (!quoted[i] && t[i] == "{") ++depth;
        else if (!quoted[i] && t[i] == "}") depth = std::max(0, depth - 1);
        else if (depth == 0 && i + 2 < t.size() && !quoted[i + 1] && t[i + 1] == "=" && !(t[i + 2] == "{" && !quoted[i + 2])) {
            const std::string& key = t[i];
            const std::string& value = t[i + 2];
            if (EqualsNoCase(key, "name")) out->name = value;
            else if (EqualsNoCase(key, "path")) out->path = value;
            else if (EqualsNoCase(key, "archive")) out->archive = value;
            else if (EqualsNoCase(key, "replace_path")) out->replacePaths.push_back(value);
            i += 2;
        }
    }
    return !out->path.empty() || !out->archive.empty();
}

std::wstring Wide(const std::string& s) {
    int n = MultiByteToWideChar(CP_UTF8, 0, s.data(), (int)s.size(), nullptr, 0);
    std::wstring w((size_t)std::max(n, 0), L'\0');
    if (n > 0) MultiByteToWideChar(CP_UTF8, 0, s.data(), (int)s.size(), &w[0], n);
    return w;
}

std::string Narrow(const std::wstring& w) {
    int n = WideCharToMultiByte(CP_UTF8, 0, w.data(), (int)w.size(), nullptr, 0, nullptr, nullptr);
    std::string s((size_t)std::max(n, 0), '\0');
    if (n > 0) WideCharToMultiByte(CP_UTF8, 0, w.data(), (int)w.size(), &s[0], n, nullptr, nullptr);
    return s;
}

bool ReadWholeFile(const std::wstring& path, std::string* out) {
    HANDLE h = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, 0, nullptr);
    if (h == INVALID_HANDLE_VALUE) return false;
    LARGE_INTEGER size;
    bool ok = GetFileSizeEx(h, &size) && size.QuadPart < (64 << 20);
    if (ok) {
        out->resize((size_t)size.QuadPart);
        DWORD got = 0;
        ok = size.QuadPart == 0 || (ReadFile(h, &(*out)[0], (DWORD)size.QuadPart, &got, nullptr) && got == size.QuadPart);
    }
    CloseHandle(h);
    if (ok && out->size() >= 3 && (unsigned char)(*out)[0] == 0xEF && (unsigned char)(*out)[1] == 0xBB &&
        (unsigned char)(*out)[2] == 0xBF)
        out->erase(0, 3);
    return ok;
}

bool Enabled(const std::wstring& userDir, std::vector<EnabledMod>* out) {
    out->clear();
    std::string json;
    std::vector<std::string> files;
    if (userDir.empty() || !ReadWholeFile(userDir + L"\\dlc_load.json", &json) || !ParseDlcLoad(json, &files)) return false;
    for (const std::string& f : files) {
        EnabledMod m;
        m.file = f;
        std::wstring desc = Slashes(Wide(f));
        if (!IsAbsolute(desc)) desc = userDir + L"\\" + desc;
        std::string text;
        if (ReadWholeFile(desc, &text) && ParseDescriptor(text, &m.descriptor)) {
            if (m.descriptor.path.empty()) {
                m.status = EnabledMod::Status::Archive;
            } else {
                m.status = EnabledMod::Status::Ok;
                m.root = Slashes(Wide(m.descriptor.path));
                if (!IsAbsolute(m.root)) m.root = userDir + L"\\" + m.root;
            }
        }
        out->push_back(std::move(m));
    }
    return true;
}

}  // namespace ModList
