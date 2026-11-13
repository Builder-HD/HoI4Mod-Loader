// The countrystateview.gui patch for province building removal (NOTES.md Part I): adds a `remove`
// button, as vanilla's state_shared_slot_building_entry has, to province_building_entry. Pure text
// work on whatever copy the game loads (vanilla's or a mod's), so it has no engine dependency and
// the smoke tests call it directly.
#include <ctype.h>
#include <string.h>

#include <string>
#include <vector>

#include "engine.h"

namespace Engine {

namespace {

// A tiny reader of the GUI file's syntax: words, "strings", '=', braces, # comments.
struct Tok {
    enum Kind { Word, Str, Eq, Open, Close } kind;
    std::string text;
    size_t pos;
};

std::vector<Tok> Tokenize(const std::string& s) {
    std::vector<Tok> out;
    size_t i = 0, n = s.size();
    while (i < n) {
        char c = s[i];
        if (c == '#') {
            while (i < n && s[i] != '\n') ++i;
        } else if (c == '"') {
            size_t start = i++;
            while (i < n && s[i] != '"' && s[i] != '\n') ++i;
            out.push_back({Tok::Str, s.substr(start + 1, i - start - 1), start});
            if (i < n && s[i] == '"') ++i;
        } else if (c == '=') {
            out.push_back({Tok::Eq, "=", i++});
        } else if (c == '{') {
            out.push_back({Tok::Open, "{", i++});
        } else if (c == '}') {
            out.push_back({Tok::Close, "}", i++});
        } else if ((unsigned char)c <= ' ') {
            ++i;
        } else {
            size_t start = i;
            while (i < n && (unsigned char)s[i] > ' ' && s[i] != '=' && s[i] != '{' && s[i] != '}' && s[i] != '#' &&
                   s[i] != '"')
                ++i;
            out.push_back({Tok::Word, s.substr(start, i - start), start});
        }
    }
    return out;
}

// t[i..i+2] is `name = <value>` (quoted or not, any case).
bool IsNameIs(const std::vector<Tok>& t, size_t i, const char* value) {
    return i + 2 < t.size() && t[i].kind == Tok::Word && _stricmp(t[i].text.c_str(), "name") == 0 &&
           t[i + 1].kind == Tok::Eq && (t[i + 2].kind == Tok::Word || t[i + 2].kind == Tok::Str) &&
           _stricmp(t[i + 2].text.c_str(), value) == 0;
}

}  // namespace

std::string PatchCountryStateViewGui(const std::string& original) {
    std::vector<Tok> t = Tokenize(original);
    size_t open = 0;  // unmatched '{'
    struct Entry {
        size_t depth;      // braces open at the entry's `name =`: the entry's own block is the last
        bool hasRemove;
        size_t closePos;   // offset of the entry block's '}'
    };
    std::vector<Entry> entries;
    long target = -1;  // the entry block being read
    for (size_t i = 0; i < t.size(); ++i) {
        if (t[i].kind == Tok::Open) {
            ++open;
        } else if (t[i].kind == Tok::Close) {
            if (target >= 0 && open == entries[(size_t)target].depth) {
                entries[(size_t)target].closePos = t[i].pos;
                target = -1;
            }
            if (open) --open;
        } else if (target < 0 && open && IsNameIs(t, i, "province_building_entry")) {
            entries.push_back({open, false, std::string::npos});
            target = (long)entries.size() - 1;
        } else if (target >= 0 && IsNameIs(t, i, "remove")) {
            entries[(size_t)target].hasRemove = true;
        }
    }
    const std::string nl = original.find("\r\n") != std::string::npos ? "\r\n" : "\n";
    // Where vanilla's shared slot puts its cross relative to the picture (x +30, y +29), here
    // relative to the province entry's picture at (12, 5).
    const std::string button = "\t\t# hoi4_loader: province_building_removal" + nl + "\t\tbuttonType = {" + nl +
                               "\t\t\tname = \"remove\"" + nl + "\t\t\tposition = { x = 42 y = 34 }" + nl +
                               "\t\t\tspriteType = \"GFX_cancel_conversion_button\"" + nl + "\t\t}" + nl + "\t";
    std::string out = original;
    bool changed = false;
    for (auto e = entries.rbegin(); e != entries.rend(); ++e) {  // back to front: offsets stay valid
        if (e->hasRemove || e->closePos == std::string::npos) continue;
        out.insert(e->closePos, nl + button);
        changed = true;
    }
    return changed ? out : std::string();
}

// Appends a white entry to NGraphics.BORDER_COLOR_CUSTOM_HIGHLIGHTS (the province group outline
// colours), for attention_area's hover outline. Lua comments (--, --[[ ]]) are skipped while the
// table's closing brace is searched. Empty if the file has no such table or already has the entry.
std::string PatchGraphicsDefines(const std::string& original) {
    if (original.find(kHighlightWhiteTag) != std::string::npos) return {};
    // The name also appears in comments ("Refers to the colors in BORDER_COLOR_CUSTOM_HIGHLIGHTS"):
    // the definition is the occurrence followed by `= {`.
    size_t open = std::string::npos;
    for (size_t key = original.find("BORDER_COLOR_CUSTOM_HIGHLIGHTS"); key != std::string::npos && open == std::string::npos;
         key = original.find("BORDER_COLOR_CUSTOM_HIGHLIGHTS", key + 1)) {
        size_t eq = original.find_first_not_of(" \t", key + 30);
        if (eq == std::string::npos || original[eq] != '=') continue;
        size_t brace = original.find_first_not_of(" \t\r\n", eq + 1);
        if (brace != std::string::npos && original[brace] == '{') open = brace;
    }
    if (open == std::string::npos) return {};
    size_t lastValue = std::string::npos;  // last character that is neither space nor comment
    size_t i = open + 1;
    while (i < original.size()) {
        if (original.compare(i, 4, "--[[") == 0) {
            size_t end = original.find("]]", i + 4);
            if (end == std::string::npos) return {};
            i = end + 2;
        } else if (original.compare(i, 2, "--") == 0) {
            size_t end = original.find('\n', i);
            if (end == std::string::npos) return {};
            i = end + 1;
        } else if (original[i] == '{') {
            return {};  // nested tables: not the expected shape
        } else if (original[i] == '}') {
            break;
        } else {
            if (!isspace((unsigned char)original[i])) lastValue = i;
            ++i;
        }
    }
    if (i >= original.size()) return {};
    bool crlf = original.find("\r\n") != std::string::npos;
    const char* nl = crlf ? "\r\n" : "\n";
    // The closing brace's own line: insert before its indentation so the entry gets its own line.
    size_t lineStart = original.rfind('\n', i);
    lineStart = lineStart == std::string::npos || lineStart < open ? i : lineStart + 1;
    std::string out = original;
    out.insert(lineStart, std::string("\t\t1.0, 1.0, 1.0, 1.0, ") + kHighlightWhiteTag + nl);
    // The previous last value needs a separator (Lua); it lies before lineStart, so insert it second.
    if (lastValue != std::string::npos && original[lastValue] != ',' && original[lastValue] != '{')
        out.insert(lastValue + 1, ",");
    return out;
}

}  // namespace Engine
