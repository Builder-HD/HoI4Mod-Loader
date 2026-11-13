// Out-of-game test of the countrystateview.gui patch (province_building_removal).
//   gui_patch_test                 hand-written GUI snippets
//   gui_patch_test <file.gui> ...  also patches real files and checks the result's shape
#include <stdio.h>
#include <string.h>

#include <fstream>
#include <sstream>
#include <string>

#include "../src/engine.h"

static int g_checks = 0, g_failed = 0;

static void Check(bool ok, const char* what) {
    ++g_checks;
    if (!ok) {
        ++g_failed;
        printf("FAIL: %s\n", what);
    }
}

static size_t Count(const std::string& s, const std::string& needle) {
    size_t n = 0;
    for (size_t at = s.find(needle); at != std::string::npos; at = s.find(needle, at + 1)) ++n;
    return n;
}

// The entry block that starts at `name = "province_building_entry"`, up to the next top-level
// containerWindowType (good enough for the snippets and vanilla's layout).
static std::string EntryBlock(const std::string& s) {
    size_t at = s.find("\"province_building_entry\"");
    if (at == std::string::npos) return {};
    size_t next = s.find("containerWindowType", at);
    return s.substr(at, next == std::string::npos ? std::string::npos : next - at);
}

static const char kShared[] =
    "guiTypes = {\n"
    "\tcontainerWindowType = {\n"
    "\t\tname = \"state_shared_slot_building_entry\"\n"
    "\t\tbuttonType = {\n"
    "\t\t\tname = \"building_picture\"\n"
    "\t\t}\n"
    "\t\tbuttonType = {\n"
    "\t\t\tname = \"remove\"\n"
    "\t\t\tspriteType = \"GFX_cancel_conversion_button\"\n"
    "\t\t}\n"
    "\t}\n"
    "\t\n";

static const char kProvince[] =
    "\tcontainerWindowType = {\n"
    "\t\tname = \"province_building_entry\"\n"
    "\t\tposition = { x=0 y=0 }\n"
    "\t\t# a comment with { braces } and name = \"remove\" in it\n"
    "\t\tbackground = {\n"
    "\t\t\tname = \"Background\"\n"
    "\t\t}\n"
    "\t\tbuttonType = {\n"
    "\t\t\tname = \"building_picture\"\n"
    "\t\t\tposition = { x = 12 y = 5 }\n"
    "\t\t}\n"
    "\t\ticonType = {\n"
    "\t\t\tname = \"damage_bar\"\n"
    "\t\t\tposition = { x=10 y=42  }\n"
    "\t\t}\n"
    "\t}\n"
    "\t\n";

static const char kEnd[] =
    "\tcontainerWindowType = {\n"
    "\t\tname = \"province_building_special_icon_entry\"\n"
    "\t}\n"
    "}\n";

static void Snippets() {
    std::string base = std::string(kShared) + kProvince + kEnd;
    std::string out = Engine::PatchCountryStateViewGui(base);
    Check(!out.empty(), "vanilla-shaped file is patched");
    Check(Count(out, "name = \"remove\"") == Count(base, "name = \"remove\"") + 1, "exactly one button added");
    std::string entry = EntryBlock(out);
    Check(entry.find("name = \"remove\"\n\t\t\tposition = { x = 42 y = 34 }") != std::string::npos,
          "the button is inside province_building_entry");
    Check(entry.find("damage_bar") < entry.find("buttonType = {\n\t\t\tname = \"remove\""),
          "the button comes last in the entry (drawn on top)");
    Check(Count(out, "{") == Count(out, "}"), "braces still balance");
    Check(EntryBlock(out).find("province_building_special_icon_entry") == std::string::npos &&
              out.find("name = \"province_building_special_icon_entry\"") != std::string::npos,
          "the next window is untouched");

    Check(Engine::PatchCountryStateViewGui(out).empty(), "a patched file is left alone");

    std::string noEntry = std::string(kShared) + kEnd;
    Check(Engine::PatchCountryStateViewGui(noEntry).empty(), "a file without the entry is left alone");

    // name = "province_building_entry" only in a comment: not an entry.
    std::string commented = std::string(kShared) + "\t# name = \"province_building_entry\"\n" + kEnd;
    Check(Engine::PatchCountryStateViewGui(commented).empty(), "a commented-out name is ignored");

    std::string crlf;
    for (char c : base) {
        if (c == '\n') crlf += '\r';
        crlf += c;
    }
    std::string outCrlf = Engine::PatchCountryStateViewGui(crlf);
    Check(!outCrlf.empty() && Count(outCrlf, "\n") == Count(outCrlf, "\r\n"), "CRLF files stay CRLF");

    // Unquoted name and other case.
    std::string bare = base;
    size_t at = bare.find("\"province_building_entry\"");
    bare.replace(at, strlen("\"province_building_entry\""), "Province_Building_Entry");
    Check(!Engine::PatchCountryStateViewGui(bare).empty(), "an unquoted name in any case is found");

    // An unterminated entry (broken file): nothing is inserted.
    std::string broken = std::string(kShared) + "\tcontainerWindowType = {\n\t\tname = \"province_building_entry\"\n";
    Check(Engine::PatchCountryStateViewGui(broken).empty(), "an unterminated entry is left alone");
}

// 00_graphics.lua: the white highlight colour (attention_area).
static void Defines() {
    const std::string base =
        "NGraphics = {\n"
        "\tSUPPLY_HOVERED_STATE_COLOR_INDEX = 0, -- Refers to the colors in BORDER_COLOR_CUSTOM_HIGHLIGHTS.\n"
        "\tBORDER_COLOR_CUSTOM_HIGHLIGHTS = {\n"
        "\t\t--[[ Groups of 4 numbers are RGBA. { braces in a comment } ]]\n"
        "\t\t0.0, 0.61, 0.75, 1.0, -- 0: mouse hover\n"
        "\t\t0.2, 0.61, 0.75, 1.0, -- 14: FACTION_THEATER_HIGHLIGHT_COLOR_INDEX\n"
        "\t},\n"
        "\tOTHER = 1,\n"
        "}\n";
    std::string out = Engine::PatchGraphicsDefines(base);
    Check(out.find("1.0, -- 14: FACTION_THEATER_HIGHLIGHT_COLOR_INDEX\n\t\t1.0, 1.0, 1.0, 1.0, -- hoi4_loader: white") !=
              std::string::npos &&
              out.find("hoi4_loader: white (attention_area hover outline)\n\t},\n\tOTHER = 1,") != std::string::npos,
          "white appended as the table's last entry, on its own line");
    Check(Engine::PatchGraphicsDefines(out).empty(), "an already patched file is left alone");
    std::string noComma = base;
    noComma.replace(noComma.find("1.0, -- 14"), 4, "1.0 ");
    std::string out2 = Engine::PatchGraphicsDefines(noComma);
    Check(out2.find("0.75, 1.0,  -- 14") != std::string::npos && out2.find("1.0, 1.0, 1.0, 1.0, -- hoi4_loader") != std::string::npos,
          "a missing separator before the new entry is added");
    Check(Engine::PatchGraphicsDefines("NGraphics = { OTHER = 1 }\n").empty(), "a file without the table is left alone");
    Check(Engine::PatchGraphicsDefines("BORDER_COLOR_CUSTOM_HIGHLIGHTS = {\n\t1.0, 1.0,\n").empty(), "an unterminated table is left alone");
    std::string crlf;
    for (char c : base) {
        if (c == '\n') crlf += '\r';
        crlf += c;
    }
    std::string outCrlf = Engine::PatchGraphicsDefines(crlf);
    Check(!outCrlf.empty() && Count(outCrlf, "\n") == Count(outCrlf, "\r\n"), "defines: CRLF files stay CRLF");
}

static void RealDefines(const char* path) {
    std::ifstream f(path, std::ios::binary);
    std::stringstream ss;
    ss << f.rdbuf();
    std::string in = ss.str(), out = Engine::PatchGraphicsDefines(in);
    printf("%s: %zu bytes, %s\n", path, in.size(), out.empty() ? "left alone" : "patched");
    if (in.find("BORDER_COLOR_CUSTOM_HIGHLIGHTS") == std::string::npos) return;
    size_t at = out.find("-- hoi4_loader: white");
    if (at != std::string::npos) printf("---\n%s\n---\n", out.substr(at > 160 ? at - 160 : 0, 240).c_str());
    Check(!out.empty() && Count(out, "1.0, 1.0, 1.0, 1.0, -- hoi4_loader: white") == 1 && Count(out, "{") == Count(in, "{") &&
              out.size() > in.size(),
          "the real 00_graphics.lua gets the white entry");
}

static void RealFile(const char* path) {
    if (strstr(path, ".lua")) return RealDefines(path);
    std::ifstream f(path, std::ios::binary);
    if (!f) {
        printf("SKIP: cannot read %s\n", path);
        return;
    }
    std::stringstream ss;
    ss << f.rdbuf();
    std::string in = ss.str();
    std::string out = Engine::PatchCountryStateViewGui(in);
    bool has = in.find("province_building_entry") != std::string::npos;
    printf("%s: %zu bytes, %s\n", path, in.size(), out.empty() ? "left alone" : "patched");
    if (!has) return;
    Check(!out.empty(), "the real file is patched");
    Check(Count(out, "{") - Count(in, "{") == 2 && Count(out, "}") - Count(in, "}") == 2, "two brace pairs added");
    Check(EntryBlock(out).find("hoi4_loader: province_building_removal") != std::string::npos,
          "the real file's entry gets the button");
}

int main(int argc, char** argv) {
    Snippets();
    Defines();
    for (int i = 1; i < argc; ++i) RealFile(argv[i]);
    printf("gui_patch_test: %d checks, %d failed\n", g_checks, g_failed);
    return g_failed ? 1 : 0;
}
