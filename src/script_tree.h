// A small reader for Paradox script (key = value, key = { ... }, bare list items, # comments),
// shared by the features that read definition files from mods (air_scenarios,
// equipment_designers). No engine access, so the parsers built on it can be unit tested.
#pragma once

#include <string>
#include <vector>

namespace ScriptTree {

struct Node {
    std::string key;     // empty for a bare list item
    std::string value;   // for key = value and bare items
    bool block = false;  // key = { ... }
    std::vector<Node> children;
    int line = 0;
};

// Parses a whole file. On a syntax error it returns false with the message and line; the items
// read before the error are kept in *out. A UTF-8 BOM is skipped.
bool Parse(const std::string& text, std::vector<Node>* out, std::string* error, int* errorLine);

// key = { a b c } or key = a: the words. False (with *why) if the block holds anything else.
bool ListOf(const Node& n, std::vector<std::string>* out, std::string* why);

// A whole number in [lo, hi] (optionally signed). False if the value is not exactly that.
bool IntOf(const Node& n, long lo, long hi, long* out);

// yes / no. False if the value is anything else.
bool BoolOf(const Node& n, bool* out);

bool EqualsNoCase(const std::string& a, const std::string& b);

// The node's key, or its value for a bare item (for messages).
inline const std::string& Label(const Node& n) { return n.key.empty() ? n.value : n.key; }

}  // namespace ScriptTree
