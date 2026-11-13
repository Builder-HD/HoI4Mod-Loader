// Engine access for attention areas (NOTES.md Part M): the camera, the province data the area
// selector reads, and the two drawing surfaces areas use, both shaded by shaders (M11):
//   - lines: the game's province group outlines (CProvinceGroupHighlight), drawn by border.shader
//     with the area's colour table entry as vSelectionColor;
//   - fills: the map's stripe texture (CMapSecondaryMask), read by secondary_color_mask.
// (Until the line/fill rework the loader also injected gradient borders for a `band` style; that
// code and its eleven signatures are gone: a band is now a fill shader, NOTES.md M12.)
// A fault inside a hook's loader part is contained (SEH), logged once; then the hook only passes through.
#include <math.h>
#include <string.h>

#include <atomic>
#include <vector>

#include "engine.h"
#include "lifecycle.h"
#include "loader.h"
#include "signatures.h"

namespace Engine {

namespace {

template <typename T>
T Rd(const void* base, size_t offset) {
    T v;
    memcpy(&v, (const uint8_t*)base + offset, sizeof v);
    return v;
}
template <typename T>
void Wr(void* base, size_t offset, T v) {
    memcpy((uint8_t*)base + offset, &v, sizeof v);
}

std::atomic<bool> g_faulted{false};
void Fault(const char* where) {
    if (g_faulted.exchange(true)) return;
    Log("ERROR: attention_area: fault in %s (contained); the loader's hooks now only pass through", where);
}

// --- Camera ------------------------------------------------------------------------------------------
using CentreOn_t = void(__fastcall*)(void* ui, int province);
using Camera_t = uint8_t*(__fastcall*)(void* ui);
using LookAt_t = void(__fastcall*)(void* camera, const float* pos);
using SetHeight_t = void(__fastcall*)(void* camera, float height);
void** g_ppUi = nullptr;
uint32_t g_centreSlot = 0, g_cameraSlot = 0;
LookAt_t g_lookAt = nullptr;
SetHeight_t g_setHeight = nullptr;
uint32_t g_heightMin = 0, g_heightMax = 0;
// Camera target fields (part of kSigCameraLookAt's pattern): eye x, height, z; look-at x, y, z.
const size_t kEyeX = 0x698, kEyeY = 0x69c, kLookX = 0x6a4, kLookZ = 0x6ac;

struct FocusIn {
    int box[4];
    int ref, ref2;
    float height, fit;
    int rect1[4], rect2[4];
};

bool FocusRaw(const FocusIn* in, float out[6]) {
    __try {
        void* ui = *g_ppUi;
        if (!ui) return false;
        uint8_t* cam = ((Camera_t)(*(void***)ui)[g_cameraSlot])(ui);
        if (!cam) return false;
        CentreOn_t centre = (CentreOn_t)(*(void***)ui)[g_centreSlot];
        float before = Rd<float>(cam, kLookX) + Rd<float>(cam, kLookZ);
        centre(ui, in->ref);
        float lx = Rd<float>(cam, kLookX), lz = Rd<float>(cam, kLookZ);
        float sign = 1.0f;
        float p1y = in->rect1[1] + in->rect1[3] * 0.5f, p1x = in->rect1[0] + in->rect1[2] * 0.5f;
        if (in->ref2 > 0) {
            float p2y = in->rect2[1] + in->rect2[3] * 0.5f;
            centre(ui, in->ref2);
            float lz2 = Rd<float>(cam, kLookZ);
            if (fabsf(p2y - p1y) > 4.0f) sign = (lz2 - lz) / (p2y - p1y) < 0 ? -1.0f : 1.0f;
        }
        float bx = in->box[0] + in->box[2] * 0.5f, by = in->box[1] + in->box[3] * 0.5f;
        float pos[3] = {lx + (bx - p1x), Rd<float>(cam, kEyeY), lz + sign * (by - p1y)};
        g_lookAt(cam, pos);
        float h = in->height;
        if (h <= 0) {
            float w = (float)in->box[2] / 1.78f, t = (float)in->box[3];
            h = (w > t ? w : t) * in->fit;
        }
        g_setHeight(cam, h);
        out[0] = lx, out[1] = lz, out[2] = sign, out[3] = pos[0], out[4] = pos[2], out[5] = Rd<float>(cam, kEyeY);
        (void)before;
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

// --- Province data ---------------------------------------------------------------------------------
using GetProvince_t = void*(__fastcall*)(void* container, int id);
uint32_t g_provContainer = 0;  // gamestate -> provinces container (vtable slot 0 = Get(id))
uint32_t g_provId = 0;         // CProvince -> id
uint32_t g_provGfx = 0, g_provRect = 0;
void** g_ppMapData = nullptr;  // static map data: per-province flags
uint32_t g_mapCount = 0, g_mapArray = 0, g_mapFlags = 0;
uint8_t g_coastalBit = 0;
uint32_t g_stateOwner = 0;

void* ProvinceObject(int id) {
    void* gs = GameState();
    if (!gs || !g_provContainer || id <= 0 || id >= ProvinceCount()) return nullptr;
    void* c = (uint8_t*)gs + g_provContainer;
    return ((GetProvince_t)(*(void***)c)[0])(c, id);
}

// --- Resolution --------------------------------------------------------------------------------------
void ResolveCamera() {
    uint8_t* go = SigFindUnique("goto_state_tail", kSigGotoStateTail);
    uint8_t* zoom = SigFindUnique("debug_zoom", kSigDebugZoom);
    uint8_t* look = SigFindUnique("camera_look_at", kSigCameraLookAt);
    if (!go || !zoom || !look) return;
    uint32_t cam = Rd<uint32_t>(go, kSigGotoCameraDisp), centre = Rd<uint32_t>(go, kSigGotoCentreDisp);
    uint32_t mn = Rd<uint32_t>(zoom, kSigZoomMinDisp), mx = Rd<uint32_t>(zoom, kSigZoomMaxDisp);
    bool agree = cam == Rd<uint32_t>(zoom, kSigZoomCameraDisp) && cam % 8 == 0 && centre % 8 == 0 && cam < 0x1000 &&
                 centre < 0x1000 && mx == mn + 4 && mn > kLookZ && mn < 0x1000;
    if (!agree) {
        Log("ERROR: attention_area: goto_state and debug_zoom disagree on the camera; camera focus off");
        return;
    }
    g_ppUi = (void**)SigRipTarget(go, kSigGotoUiRel, kSigGotoUiRel + 4);
    g_centreSlot = centre / 8;
    g_cameraSlot = cam / 8;
    g_setHeight = (SetHeight_t)SigFollowRel32(go + kSigGotoSetHeight);
    g_lookAt = (LookAt_t)look;
    g_heightMin = mn;
    g_heightMax = mx;
    g_provId = Rd<uint32_t>(go, kSigGotoProvinceIdDisp);
    Log("Attention areas: UI %s, centre slot %u, camera slot %u, height limits +0x%x/+0x%x, province id +0x%x",
        SigDescribe(g_ppUi).c_str(), g_centreSlot, g_cameraSlot, g_heightMin, g_heightMax, g_provId);
}

void ResolveProvinces() {
    uint8_t* coast = SigFindUnique("state_coastal", kSigStateCoastal);
    uint8_t* rect = SigFindUnique("province_rect", kSigProvinceRect);
    uint8_t* rail = SigFindUnique("rail_controller", kSigRailController);
    uint8_t* owner = SigFindUnique("state_owner_check", kSigStateOwnerCheck);
    if (!coast || !rect || !rail || !owner) return;
    uint8_t bit = Rd<uint8_t>(coast, kSigCoastalBit);
    uint32_t gfx = Rd<uint32_t>(rect, kSigRectGfxDisp), r = Rd<uint32_t>(rect, kSigRectDisp);
    bool agree = bit && (bit & (bit - 1)) == 0 && gfx < 0x1000 && r < 0x1000 && g_provId && g_provId < 0x1000;
    if (!agree) {
        Log("ERROR: attention_area: province data call sites look different; area selection off");
        return;
    }
    g_ppMapData = (void**)SigRipTarget(coast, kSigCoastalMapRel, kSigCoastalMapRel + 4);
    g_mapCount = Rd<uint32_t>(coast, kSigCoastalCountDisp);
    g_mapArray = Rd<uint32_t>(coast, kSigCoastalArrayDisp);
    g_mapFlags = Rd<uint32_t>(coast, kSigCoastalFlagsDisp);
    g_coastalBit = bit;
    g_provGfx = gfx;
    g_provRect = r;
    g_provContainer = Rd<uint8_t>(rail, kSigRailProvincesDisp);
    g_stateOwner = Rd<uint32_t>(owner, kSigOwnerStateOwnerDisp);
    Log("Attention areas: map data %s (provinces +0x%x/+0x%x, flags +0x%x bit 0x%x), province rect +0x%x/+0x%x, "
        "provinces +0x%x, state owner +0x%x",
        SigDescribe(g_ppMapData).c_str(), g_mapArray, g_mapCount, g_mapFlags, g_coastalBit, g_provGfx, g_provRect,
        g_provContainer, g_stateOwner);
}

// --- Province group outlines (CProvinceGroupHighlight) ---------------------------------------------
using CreateHighlight_t = void(__fastcall*)(void* owner, void** sharedOut, const void* provinces, const int* style);
CreateHighlight_t g_createHighlight = nullptr;
uint32_t g_hlOwner = 0;            // ui -> highlight owner (list of weak references at +0x430)
int g_hlSecond = 0;                // construction mode's priority (logged for comparison)
const int kOutlinePriority = 100;  // the loader's outline: above the game's own outlines
const int* g_paletteCount = nullptr;   // float count of BORDER_COLOR_CUSTOM_HIGHLIGHTS
// The renderer's colour table: palette colour kWhiteSlot is made white there (vanilla colour 14,
// FACTION_THEATER_HIGHLIGHT_COLOR_INDEX, the faction theatre map icon hover: that outline turns
// white too). A colour past the vanilla 15 is drawn as colour 14 (second in-game test).
const int kWhiteSlot = 14;
uint8_t* g_tableFn = nullptr;
void(__fastcall* g_tableOrig)(void*) = nullptr;
int g_tableFixed = 0;        // fixed colours before the palette in the table (10)
uint32_t g_tableCount = 0;   // renderer -> the table's count field
bool g_tableWhite = false;   // the slot was made white (the table was built after the hook)

struct IntArray {  // CPdxArray<int> as the highlight constructor reads it: data, count at +0xc
    const int* data;
    int cap;
    int count;
    void* allocator;
};

// std::shared_ptr release in the game's own MSVC layout: uses +8, weaks +0xc; vtable slot 0
// destroys the object, slot 1 the control block.
bool ReleaseRaw(void* ctrl) {
    __try {
        if (InterlockedDecrement((volatile LONG*)((uint8_t*)ctrl + 8)) == 0) {
            ((void(__fastcall*)(void*))(*(void***)ctrl)[0])(ctrl);
            if (InterlockedDecrement((volatile LONG*)((uint8_t*)ctrl + 0xc)) == 0)
                ((void(__fastcall*)(void*))(*(void***)ctrl)[1])(ctrl);
        }
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

bool CreateRaw(void* owner, const IntArray* provinces, const int* style, void** out) {
    __try {
        g_createHighlight(owner, out, provinces, style);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

void ResolveHighlight() {
    uint8_t* create = SigFindUnique("highlight_create", kSigHighlightCreate);
    uint8_t* site = SigFindUnique("highlight_create_site", kSigHighlightCreateSite);
    uint8_t* pal = SigFindUnique("highlight_palette_count", kSigHighlightPaletteCount);
    if (!create || !site || !pal) return;
    const int* style = (const int*)SigRipTarget(site, kSigHlStyleRel, kSigHlStyleRel + 4);
    uint32_t owner = Rd<uint32_t>(site, kSigHlOwnerDisp);
    bool agree = SigFollowRel32(site + kSigHlCreateCall) == create &&
                 (void**)SigRipTarget(site, kSigHlUiRel, kSigHlUiRel + 4) == g_ppUi && g_ppUi && owner < 0x2000 && owner % 8 == 0 &&
                 style[0] >= 0 && style[0] < 64 && style[1] >= 0 && style[1] < 64;
    if (!agree) {
        Log("ERROR: attention_area: the province outline call site disagrees; outlines off");
        return;
    }
    g_createHighlight = (CreateHighlight_t)create;
    g_hlOwner = owner;
    g_hlSecond = style[1];
    g_paletteCount = (const int*)SigRipTarget(pal, kSigPaletteCountRel, kSigPaletteCountRel + 4);
    Log("Attention areas: province outlines: owner ui+0x%x, construction style {colour id %d, %d}, palette %s", g_hlOwner, style[0],
        style[1], SigDescribe(g_paletteCount).c_str());
    uint8_t* table = SigFindUnique("highlight_colour_table", kSigHighlightColourTable);
    int fixed = table ? Rd<uint8_t>(table, kSigTableFixedImm) : 0;
    uint32_t count = table ? Rd<uint32_t>(table, kSigTableCountDisp) : 0;
    if (!table || (const int*)SigRipTarget(table, kSigTablePaletteRel, kSigTablePaletteRel + 4) != g_paletteCount || fixed <= 0 ||
        fixed > 32 || count < 0x10 || count > 0x10000) {
        Log("attention_area: the outline colour table was not found where expected; outlines keep vanilla colours");
        return;
    }
    g_tableFn = table;
    g_tableFixed = fixed;
    g_tableCount = count;
}

// The renderer's colour table (renderer + count field - 0xc: float4 data; count at the field):
// right after the game builds it, the loader's slot becomes white. The game builds it once, in
// the map graphics setup, from the built-in palette (the defines are read later), so a defines
// change never reaches it (third in-game test, NOTES.md M7).
bool WhiteRaw(void* renderer) {
    __try {
        int count = Rd<int>(renderer, g_tableCount);
        float* data = Rd<float*>(renderer, g_tableCount - 0xc);
        int slot = g_tableFixed + kWhiteSlot;
        if (!data || slot >= count) return false;
        for (int k = 0; k < 4; ++k) data[slot * 4 + k] = 1.0f;
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

// Extra colours for script outlines: kExtraSlots entries appended to the renderer's colour table at
// its build (CPdxArray<float4>: data, cap, count, allocator* at +0x10), filled as areas need them.
const int kExtraSlots = 32;
void* g_renderer = nullptr;   // the renderer whose table was extended
int g_extraBase = 0;          // colour id of the first extra slot (0: none)
float g_extraRgba[kExtraSlots][4];
int g_extraUsed = 0;          // slots handed out since the last reset

using TableAlloc_t = void*(__fastcall*)(void* allocator, size_t bytes, size_t align);
using TableFree_t = void(__fastcall*)(void* allocator, void* p);

// Appends `extra` zeroed entries (grown with the table's own allocator, as the build grows it).
bool GrowRaw(void* renderer, int extra, int* base) {
    __try {
        uint8_t* arr = (uint8_t*)renderer + g_tableCount - 0xc;
        int count = Rd<int>(arr, 0xc), cap = Rd<int>(arr, 8);
        float* data = Rd<float*>(arr, 0);
        void* alloc = Rd<void*>(arr, 0x10);
        if (!data || !alloc || count <= 0) return false;
        int need = count + extra;
        if (need > cap) {
            float* fresh = (float*)((TableAlloc_t)(*(void***)alloc)[1])(alloc, (size_t)need * 16, 16);
            if (!fresh) return false;
            memcpy(fresh, data, (size_t)count * 16);
            ((TableFree_t)(*(void***)alloc)[2])(alloc, data);
            data = fresh;
            Wr<float*>(arr, 0, fresh);
            Wr<int>(arr, 8, need);
        }
        memset(data + count * 4, 0, (size_t)extra * 16);
        Wr<int>(arr, 0xc, need);
        *base = count;
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

bool WriteSlotRaw(void* renderer, int id, const float* rgba) {
    __try {
        int count = Rd<int>(renderer, g_tableCount);
        float* data = Rd<float*>(renderer, g_tableCount - 0xc);
        if (!data || id < 0 || id >= count) return false;
        memcpy(data + id * 4, rgba, 16);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

void __fastcall TableDetour(void* renderer) {
    g_tableOrig(renderer);
    bool ok = WhiteRaw(renderer);
    g_tableWhite = ok;
    int base = 0;
    bool grown = GrowRaw(renderer, kExtraSlots, &base) && base + kExtraSlots <= 255;  // colour ids are bytes
    g_renderer = renderer;
    g_extraBase = grown ? base : 0;
    for (int i = 0; grown && i < g_extraUsed; ++i) WriteSlotRaw(renderer, base + i, g_extraRgba[i]);  // a rebuild keeps them
    Log("attention_area: outline colour %d %s; %d extra outline colours %s", kWhiteSlot,
        ok ? "made white in the renderer's colour table" : "NOT changed (table not as expected)", kExtraSlots,
        grown ? "added" : "NOT added (script outlines use palette colours)");
}

// --- Stripes: the stripe texture (CMapSecondaryMask, NOTES.md M9) -----------------------------------
// mask +0: CPdxArray<uint32> colour per province (bytes r, g, b, a; a = stripe strength), count +0xc;
// +0x44: repaint every province. The shader draws diagonal stripes where alpha > 0, in every map mode.
// Right before the game paints, the loader writes its areas' colours over the game's. A province
// the game changed is already queued for the paint; after an area change everything is repainted.
using StripePaint_t = char(__fastcall*)(void* mask, void* gamestate, void* tiles, char* out);
uint8_t* g_stripeFn = nullptr;
StripePaint_t g_stripeOrig = nullptr;
SRWLOCK g_stripeLock = SRWLOCK_INIT;
std::vector<StripeArea> g_stripeWant;     // guarded by g_stripeLock
bool g_stripeChanged = false;             // guarded: g_stripeWant changed since the last paint
std::vector<int64_t> g_stripeSaved;       // guarded: per province, the game's colour where ours went (-1: none)
std::vector<int> g_stripeMine;            // guarded: provinces holding our colour
volatile long g_stripeFaultLogged = 0, g_stripeFirstLogged = 0;

void ResolveStripes() {
    g_stripeFn = SigFindUnique("stripe_paint", kSigStripePaint);
    if (!g_stripeFn) Log("attention_area: the stripe paint function was not found: fills are not drawn");
}

// The mask's colour array, checked readable and writable at both ends.
bool StripeColoursRaw(uint8_t* mask, uint32_t** colours, int* count) {
    __try {
        uint32_t* c = Rd<uint32_t*>(mask, 0);
        int n = Rd<int>(mask, 0xc);
        if (!c || n <= 1 || n > 1000000) return false;
        volatile uint32_t* v = c;
        v[0] = v[0];
        v[n - 1] = v[n - 1];
        *colours = c;
        *count = n;
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

// Writes the wanted colours into the mask (main thread, in the paint call).
bool ApplyStripes(uint8_t* mask, const std::vector<StripeArea>& want, bool changed) {
    uint32_t* colours = nullptr;
    int count = 0;
    if (!StripeColoursRaw(mask, &colours, &count)) return false;
    {
        if ((int)g_stripeSaved.size() != count) g_stripeSaved.assign((size_t)count, -1);
        bool repaint = false;
        if (changed) {
            // Give the game back what it had in provinces no longer wanted (0 where it had nothing;
            // a later map mode refresh corrects a colour that went stale meanwhile).
            std::vector<char> wanted((size_t)count, 0);
            for (const StripeArea& a : want)
                for (int p : a.provinces)
                    if (p > 0 && p < count) wanted[p] = 1;
            for (int p : g_stripeMine) {
                if (p >= count || wanted[p]) continue;
                colours[p] = g_stripeSaved[p] >= 0 ? (uint32_t)g_stripeSaved[p] : 0;
                g_stripeSaved[p] = -1;
            }
            g_stripeMine.clear();
            for (const StripeArea& a : want)
                for (int p : a.provinces)
                    if (p > 0 && p < count) g_stripeMine.push_back(p);
            repaint = true;
        }
        for (const StripeArea& a : want)
            for (int p : a.provinces) {
                if (p <= 0 || p >= count || colours[p] == a.rgba) continue;
                // The game's own colour (or a later change of it) is kept for the restore.
                bool ours = false;
                for (const StripeArea& b : want) ours = ours || colours[p] == b.rgba;
                if (!ours) g_stripeSaved[p] = colours[p];
                colours[p] = a.rgba;  // a province the game changed is queued already
            }
        if (repaint) mask[0x44] = 1;
        return true;
    }
}

char __fastcall StripePaintDetour(void* mask, void* gamestate, void* tiles, char* out) {
    AcquireSRWLockExclusive(&g_stripeLock);
    bool changed = g_stripeChanged;
    g_stripeChanged = false;
    if ((changed || !g_stripeWant.empty()) && !ApplyStripes((uint8_t*)mask, g_stripeWant, changed) &&
        InterlockedExchange(&g_stripeFaultLogged, 1) == 0)
        Log("ERROR: attention_area: writing stripe colours faulted (contained; logged once)");
    if (changed && InterlockedExchange(&g_stripeFirstLogged, 1) == 0)
        Log("attention_area: stripe colours applied (%zu stripe area(s); first paint, logged once)", g_stripeWant.size());
    ReleaseSRWLockExclusive(&g_stripeLock);
    return g_stripeOrig(mask, gamestate, tiles, out);
}

}  // namespace

bool g_stripeHooked = false;
bool HaveStripes() { return g_stripeHooked; }

bool HookStripes() {
    g_stripeHooked = g_stripeFn && HookQueue(g_stripeFn, (void*)&StripePaintDetour, (void**)&g_stripeOrig, "stripe_paint");
    return g_stripeHooked;
}

void SetStripes(const std::vector<StripeArea>& areas) {
    AcquireSRWLockExclusive(&g_stripeLock);
    g_stripeWant = areas;
    g_stripeChanged = true;
    ReleaseSRWLockExclusive(&g_stripeLock);
}

void ForgetStripes() {
    // A new game: the old provinces' saved colours mean nothing there.
    AcquireSRWLockExclusive(&g_stripeLock);
    g_stripeWant.clear();
    g_stripeSaved.assign(g_stripeSaved.size(), -1);
    g_stripeMine.clear();
    g_stripeChanged = false;
    ReleaseSRWLockExclusive(&g_stripeLock);
}

void ResolveAttention() {
    ResolveCamera();  // before ResolveProvinces: the province id offset comes from goto_state
    ResolveProvinces();
    ResolveHighlight();  // after ResolveCamera: cross-checks the UI global
    ResolveStripes();
}

bool HaveHighlight() { return g_createHighlight && g_hlOwner && g_paletteCount && g_ppUi && g_tableFixed; }

int HighlightPaletteSize() {
    int n = g_paletteCount ? *g_paletteCount : 0;
    return n > 0 && n < 4096 ? n / 4 : 0;
}

int HighlightWhiteIndex() { return g_tableWhite ? kWhiteSlot : -1; }

// --- The owner's hovered item (drawn before every outline, colour id 5) ----------------------------
namespace {
using SetHovered_t = void(__fastcall*)(void* owner, void* province);
uint8_t* g_setHovered = nullptr;
SetHovered_t g_setHoveredOrig = nullptr;
uint32_t g_hoverType = 0, g_hoverItem = 0;  // owner -> type (0 province, 1 state), item
HoverFilter_t g_hoverFilter = nullptr;

bool HoverFilterRaw(void* province, bool* drop) {
    __try {
        *drop = g_hoverFilter(ProvinceIdOf(province));
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

void __fastcall SetHoveredDetour(void* owner, void* province) {
    if (g_hoverFilter && province && !g_faulted.load()) {
        bool drop = false;
        if (!HoverFilterRaw(province, &drop)) Fault("the hovered-province hook");
        else if (drop) province = nullptr;  // nothing hovered: the loader's outline shows instead
    }
    g_setHoveredOrig(owner, province);
}

void* HoverOwner() {
    void* ui = g_ppUi ? *g_ppUi : nullptr;
    return ui && g_hlOwner ? Rd<void*>(ui, g_hlOwner) : nullptr;
}

bool ClearHoveredRaw(void* owner, int (*idOf)(const void*), HoverFilter_t filter, bool* cleared) {
    __try {
        *cleared = false;
        if (Rd<int>(owner, g_hoverType) != 0) return true;  // a state, not a province
        void* p = Rd<void*>(owner, g_hoverItem);
        if (p && filter(idOf(p))) {
            g_setHoveredOrig(owner, nullptr);
            *cleared = true;
        }
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}
}  // namespace

bool HookHoveredProvince(HoverFilter_t filter) {
    uint8_t* p = SigFindUnique("hover_setters", kSigHoverSetters);
    if (!p || !g_provId) return false;
    uint32_t type = Rd<uint32_t>(p, kSigHoverTypeDisp), item = Rd<uint32_t>(p, kSigHoverItemDisp);
    if (type != Rd<uint32_t>(p, kSigHoverStateTypeDisp) || item != Rd<uint32_t>(p, kSigHoverStateItemDisp) || item != type + 8 ||
        type > 0x2000) {
        Log("attention_area: the hovered-item setters disagree; the game's start-province mark stays");
        return false;
    }
    g_setHovered = p;
    g_hoverType = type;
    g_hoverItem = item;
    g_hoverFilter = filter;
    Log("Attention areas: hovered item at owner +0x%x/+0x%x", type, item);
    return HookQueue(p, (void*)&SetHoveredDetour, (void**)&g_setHoveredOrig, "set_hovered_province");
}

bool ClearHoveredProvinceIf(HoverFilter_t filter) {
    void* owner = g_setHoveredOrig ? HoverOwner() : nullptr;
    bool cleared = false;
    if (owner && !ClearHoveredRaw(owner, &ProvinceIdOf, filter, &cleared)) Fault("clearing the hovered province");
    return cleared;
}

bool HookHighlightWhite() {
    return g_tableFn && HookQueue(g_tableFn, (void*)&TableDetour, (void**)&g_tableOrig, "highlight_colour_table");
}

void ReleaseOutline(OutlineRef* ref) {
    void* ctrl = ref ? ref->ctrl : nullptr;
    if (ref) ref->obj = ref->ctrl = nullptr;
    if (ctrl && !ReleaseRaw(ctrl)) Log("ERROR: attention_area: releasing a province outline faulted (contained)");
}

bool CreateOutline(const std::vector<int>& provinces, int colourId, int priority, OutlineRef* out) {
    ReleaseOutline(out);
    if (!HaveHighlight() || provinces.empty() || colourId < 0 || colourId > 255) return false;
    void* ui = *g_ppUi;
    void* owner = ui ? Rd<void*>(ui, g_hlOwner) : nullptr;
    if (!owner) return false;
    IntArray arr = {provinces.data(), (int)provinces.size(), (int)provinces.size(), nullptr};
    // The style's first byte is the colour id in the renderer's table (FUN_14130f0e0 reads it): ids
    // 0-9 are the fixed border colours, 10 + i palette colour i, then the loader's extra colours.
    // Construction mode's {10, 2} is palette 0 (teal); the earlier tests drew {15, 2} as palette 5
    // (green) and {10, x} as teal. The second number is the priority: the owner's list is sorted by
    // it, highest first, and an edge shared by several outlines gets the first one's colour
    // (FUN_140bb2cd0 / FUN_14130f0e0).
    int style[2] = {colourId, priority};
    void* res[2] = {nullptr, nullptr};
    if (!CreateRaw(owner, &arr, style, res)) {
        Log("ERROR: attention_area: creating a province outline faulted (contained)");
        return false;
    }
    out->obj = res[0];
    out->ctrl = res[1];
    return res[1] != nullptr;
}

int OutlineWhiteId() { return g_tableWhite ? g_tableFixed + kWhiteSlot : -1; }

int OutlineColourId(float r, float g, float b, float a) {
    if (!g_extraBase || !g_renderer) return -1;
    float rgba[4] = {r, g, b, a};
    for (int i = 0; i < g_extraUsed; ++i)
        if (memcmp(g_extraRgba[i], rgba, 16) == 0) return g_extraBase + i;
    if (g_extraUsed >= kExtraSlots) return -1;
    memcpy(g_extraRgba[g_extraUsed], rgba, 16);
    if (!WriteSlotRaw(g_renderer, g_extraBase + g_extraUsed, rgba)) return -1;
    return g_extraBase + g_extraUsed++;
}

void ResetOutlineColours() { g_extraUsed = 0; }

// The railway hover mark: one outline above everything (priority 100: the game's own start-province
// outline of a hovered railway entry ranks above construction mode's 2, sixth test).
namespace {
OutlineRef g_hover;
}
void HideHighlight() { ReleaseOutline(&g_hover); }
bool ShowHighlight(const std::vector<int>& provinces, int paletteIndex) {
    if (paletteIndex < 0 || paletteIndex >= HighlightPaletteSize()) return false;
    return CreateOutline(provinces, g_tableFixed + paletteIndex, kOutlinePriority, &g_hover);
}

bool HaveAttentionCamera() { return g_ppUi && g_centreSlot && g_cameraSlot && g_setHeight && g_lookAt && g_heightMin; }
bool HaveProvinceData() {
    return g_ppMapData && g_mapCount && g_mapArray && g_mapFlags && g_coastalBit && g_provGfx && g_provRect && g_provContainer &&
           g_provId && g_stateOwner && ProvinceControllerOffset();
}

int ProvinceCount() {
    void* map = g_ppMapData ? *g_ppMapData : nullptr;
    int n = map ? Rd<int>(map, g_mapCount) : 0;
    return n > 0 && n < 1000000 ? n : 0;
}

int ProvinceIdOf(const void* province) { return province && g_provId ? Rd<int>(province, g_provId) : 0; }

bool ProvinceIsCoastal(int id) {
    void* map = g_ppMapData ? *g_ppMapData : nullptr;
    if (!map || id <= 0 || id >= ProvinceCount()) return false;
    void* const* arr = Rd<void* const*>(map, g_mapArray);
    const uint8_t* p = arr ? (const uint8_t*)arr[id] : nullptr;
    return p && (p[g_mapFlags] & g_coastalBit) != 0;
}

int ProvinceController(int id) {
    void* p = ProvinceObject(id);
    return p && ProvinceControllerOffset() ? Rd<int>(p, ProvinceControllerOffset()) : 0;
}

int StateOwner(int stateId) {
    void* s = StateById(stateId);
    return s && g_stateOwner ? Rd<int>(s, g_stateOwner) : 0;
}

bool ProvinceRect(int id, int rect[4]) {
    void* p = ProvinceObject(id);
    const uint8_t* gfx = p ? Rd<const uint8_t*>(p, g_provGfx) : nullptr;
    if (!gfx) return false;
    memcpy(rect, gfx + g_provRect, 4 * sizeof(int));
    return rect[2] >= 0 && rect[3] >= 0;
}

// --- The UI's centre-on-province (right-click on a queue entry, goto_state, ...) -------------------
namespace {
using CentreObj_t = void(__fastcall*)(void* ui, void* province);
CentreObj_t g_centreObj = nullptr, g_centreObjOrig = nullptr;
CentreFilter_t g_centreFilter = nullptr;
bool g_inOwnFocus = false;  // FocusCamera calibrates with the same function: no filter then

bool FilterRaw(void* province, bool* take) {
    __try {
        *take = g_centreFilter(ProvinceIdOf(province));
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

void __fastcall CentreObjDetour(void* ui, void* province) {
    if (g_centreFilter && province && !g_inOwnFocus && !g_faulted.load()) {
        bool take = false;
        if (!FilterRaw(province, &take)) Fault("the centre-on-province hook");
        else if (take) return;  // the filter moves the camera itself
    }
    g_centreObjOrig(ui, province);
}
}  // namespace

bool HookCentreOnProvince(CentreFilter_t filter) {
    if (!HaveAttentionCamera() || !g_provId) return false;
    size_t n = 0;
    void** vt = FindVtableByRtti(".?AVCInGameIdler@@", &n);
    uint8_t* tail = vt && n > g_centreSlot ? SigFindIn((uint8_t*)vt[g_centreSlot], 0x200, kCodeIdlerCentreTail) : nullptr;
    uint32_t disp = tail ? Rd<uint32_t>(tail, kCodeCentreTailDisp) : 0;
    if (!tail || disp % 8 || disp / 8 >= n) {
        Log("attention_area: the UI's centre-on-province has another shape; right-click focus off");
        return false;
    }
    g_centreObj = (CentreObj_t)vt[disp / 8];
    g_centreFilter = filter;
    Log("Attention areas: centre on province object = idler slot %u (%s)", disp / 8, SigDescribe((void*)g_centreObj).c_str());
    return HookQueue((void*)g_centreObj, (void*)&CentreObjDetour, (void**)&g_centreObjOrig, "idler_centre_province");
}

bool FocusCamera(const int box[4], int ref, int ref2, float height, float fitFactor) {
    if (!HaveAttentionCamera() || !GameState() || ref <= 0) return false;
    FocusIn in{};
    memcpy(in.box, box, sizeof in.box);
    in.ref = ref;
    in.ref2 = ref2;
    in.height = height;
    in.fit = fitFactor > 0 ? fitFactor : 1.0f;
    if (!ProvinceRect(ref, in.rect1)) return false;
    if (ref2 > 0 && !ProvinceRect(ref2, in.rect2)) in.ref2 = 0;
    float out[6] = {};
    g_inOwnFocus = true;
    bool ok = FocusRaw(&in, out);
    g_inOwnFocus = false;
    if (!ok) {
        Log("ERROR: attention_area: the camera focus faulted (contained)");
        return false;
    }
    Log("attention_area: focus box %d,%d %dx%d: province %d centred at look-at %.0f/%.0f, z sign %.0f; look-at %.0f/%.0f, "
        "height %.0f",
        box[0], box[1], box[2], box[3], ref, out[0], out[1], out[2], out[3], out[4], out[5]);
    return true;
}

}  // namespace Engine
