// The countrystateview.gui patch for province building removal (NOTES.md Part I): adds a `remove`
// button, as vanilla's state_shared_slot_building_entry has, to province_building_entry. Pure text
// work on whatever copy the game loads (vanilla's or a mod's), so it has no engine dependency and
// the smoke tests call it directly.
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

// tank_designer_view.gui (equipment_designers, NOTES.md N9): the engine makes one slot window per
// positionType pos_custom_module_slot_window_N / pos_fixed_module_slot_window_N, N = 0, 1, ... up to
// the first missing one. Missing ones are added after the last existing one, at window 0's position.
std::string PatchTankDesignerViewGui(const std::string& original, int maxWindows, int* customBefore, int* fixedBefore) {
    *customBefore = *fixedBefore = 0;
    std::vector<Tok> t = Tokenize(original);
    std::vector<long> partner(t.size(), -1);  // '{' -> its '}'
    std::vector<size_t> stack;
    for (size_t i = 0; i < t.size(); ++i) {
        if (t[i].kind == Tok::Open) {
            stack.push_back(i);
        } else if (t[i].kind == Tok::Close && !stack.empty()) {
            partner[stack.back()] = (long)i;
            stack.pop_back();
        }
    }
    struct Block {
        std::string name;
        size_t start;     // offset of `positionType`
        size_t end;       // just after its '}'
        std::string position;  // `position = { ... }` as written
    };
    std::vector<Block> blocks;
    for (size_t i = 0; i + 2 < t.size(); ++i) {
        if (t[i].kind != Tok::Word || _stricmp(t[i].text.c_str(), "positionType") != 0 || t[i + 1].kind != Tok::Eq ||
            t[i + 2].kind != Tok::Open || partner[i + 2] < 0)
            continue;
        size_t close = (size_t)partner[i + 2];
        Block b{std::string(), t[i].pos, t[close].pos + 1, std::string()};
        for (size_t k = i + 3; k < close; ++k) {
            if (b.name.empty() && k + 2 < close && t[k].kind == Tok::Word && _stricmp(t[k].text.c_str(), "name") == 0 &&
                t[k + 1].kind == Tok::Eq && (t[k + 2].kind == Tok::Word || t[k + 2].kind == Tok::Str))
                b.name = t[k + 2].text;
            if (b.position.empty() && k + 2 < close && t[k].kind == Tok::Word && _stricmp(t[k].text.c_str(), "position") == 0 &&
                t[k + 1].kind == Tok::Eq && t[k + 2].kind == Tok::Open && partner[k + 2] > 0)
                b.position = original.substr(t[k].pos, t[(size_t)partner[k + 2]].pos + 1 - t[k].pos);
        }
        blocks.push_back(b);
        i = close;
    }
    auto find = [&](const std::string& name) -> const Block* {
        for (const Block& b : blocks)
            if (_stricmp(b.name.c_str(), name.c_str()) == 0) return &b;
        return nullptr;
    };
    const char* const kinds[2] = {"pos_custom_module_slot_window_", "pos_fixed_module_slot_window_"};
    const Block* first = find(std::string(kinds[0]) + "0");
    if (!first || first->position.empty()) return std::string();
    size_t after = 0;  // just after the last existing slot position
    for (const Block& b : blocks)
        if (_strnicmp(b.name.c_str(), kinds[0], strlen(kinds[0])) == 0 || _strnicmp(b.name.c_str(), kinds[1], strlen(kinds[1])) == 0)
            after = b.end > after ? b.end : after;
    int* before[2] = {customBefore, fixedBefore};
    for (int k = 0; k < 2; ++k)
        while (*before[k] < maxWindows && find(kinds[k] + std::to_string(*before[k]))) ++*before[k];
    size_t lineStart = original.rfind('\n', first->start);
    lineStart = lineStart == std::string::npos ? 0 : lineStart + 1;
    std::string indent = original.substr(lineStart, first->start - lineStart);
    if (indent.find_first_not_of(" \t") != std::string::npos) indent = "\t\t\t";
    const std::string nl = original.find("\r\n") != std::string::npos ? "\r\n" : "\n";
    std::string add;
    for (int k = 0; k < 2; ++k)
        for (int n = 0; n < maxWindows; ++n) {
            std::string name = kinds[k] + std::to_string(n);
            if (find(name)) continue;
            add += nl + indent + "positionType = { name = \"" + name + "\" " + first->position + " }";
        }
    if (add.empty()) return std::string();
    std::string out = original;
    out.insert(after, nl + nl + indent + "# hoi4_loader: equipment_designers (more slot windows, placed by the loader)" + add);
    return out;
}

}  // namespace Engine
