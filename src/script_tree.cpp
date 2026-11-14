// A small reader for Paradox script (see script_tree.h).
#include "script_tree.h"

#include <cctype>
#include <cstdlib>

namespace ScriptTree {

namespace {

struct Reader {
    const std::string& s;
    size_t i = 0;
    int line = 1;
    std::string error;
    int errorLine = 0;

    explicit Reader(const std::string& text) : s(text) {
        if (s.size() >= 3 && (unsigned char)s[0] == 0xEF && (unsigned char)s[1] == 0xBB && (unsigned char)s[2] == 0xBF) i = 3;
    }
    void SkipSpace() {
        while (i < s.size()) {
            char c = s[i];
            if (c == '\n') {
                ++line;
                ++i;
            } else if (c == ' ' || c == '\t' || c == '\r') {
                ++i;
            } else if (c == '#') {
                while (i < s.size() && s[i] != '\n') ++i;
            } else {
                break;
            }
        }
    }
    bool Fail(const std::string& message) {
        if (error.empty()) {
            error = message;
            errorLine = line;
        }
        return false;
    }
    // A word or a quoted string.
    bool Word(std::string* out) {
        out->clear();
        if (i >= s.size()) return Fail("unexpected end of file");
        if (s[i] == '"') {
            ++i;
            while (i < s.size() && s[i] != '"') {
                if (s[i] == '\n') return Fail("quoted text runs past the end of the line");
                *out += s[i++];
            }
            if (i >= s.size()) return Fail("quoted text is not closed");
            ++i;
            return true;
        }
        while (i < s.size()) {
            char c = s[i];
            if (c == ' ' || c == '\t' || c == '\r' || c == '\n' || c == '=' || c == '{' || c == '}' || c == '#' || c == '"') break;
            *out += c;
            ++i;
        }
        if (out->empty()) return Fail(std::string("unexpected '") + s[i] + "'");
        return true;
    }
    // Items until '}' (inside a block) or the end of the file (top level).
    bool Items(std::vector<Node>* out, bool inBlock) {
        for (;;) {
            SkipSpace();
            if (i >= s.size()) return inBlock ? Fail("a '{' is not closed") : true;
            if (s[i] == '}') {
                if (!inBlock) return Fail("a '}' without its '{'");
                ++i;
                return true;
            }
            Node n;
            n.line = line;
            std::string word;
            if (!Word(&word)) return false;
            SkipSpace();
            if (i < s.size() && s[i] == '=') {
                ++i;
                SkipSpace();
                n.key = word;
                if (i < s.size() && s[i] == '{') {
                    ++i;
                    n.block = true;
                    if (!Items(&n.children, true)) return false;
                } else if (!Word(&n.value)) {
                    return false;
                }
            } else {
                n.value = word;  // a bare list item
            }
            out->push_back(std::move(n));
        }
    }
};

}  // namespace

bool Parse(const std::string& text, std::vector<Node>* out, std::string* error, int* errorLine) {
    Reader r(text);
    if (r.Items(out, false)) return true;
    *error = r.error;
    *errorLine = r.errorLine;
    return false;
}

bool ListOf(const Node& n, std::vector<std::string>* out, std::string* why) {
    if (!n.block) {
        out->push_back(n.value);
        return true;
    }
    for (const Node& c : n.children) {
        if (!c.key.empty() || c.block) {
            *why = "'" + n.key + "' takes a list of names, e.g. " + n.key + " = { a b }";
            return false;
        }
        out->push_back(c.value);
    }
    return true;
}

bool IntOf(const Node& n, long lo, long hi, long* out) {
    if (n.block || n.value.empty()) return false;
    char* end = nullptr;
    long v = std::strtol(n.value.c_str(), &end, 10);
    if (*end || v < lo || v > hi) return false;
    *out = v;
    return true;
}

bool BoolOf(const Node& n, bool* out) {
    if (n.block) return false;
    if (EqualsNoCase(n.value, "yes")) {
        *out = true;
        return true;
    }
    if (EqualsNoCase(n.value, "no")) {
        *out = false;
        return true;
    }
    return false;
}

bool EqualsNoCase(const std::string& a, const std::string& b) {
    if (a.size() != b.size()) return false;
    for (size_t i = 0; i < a.size(); ++i)
        if (std::tolower((unsigned char)a[i]) != std::tolower((unsigned char)b[i])) return false;
    return true;
}

}  // namespace ScriptTree
