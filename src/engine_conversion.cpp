// Engine access for equipment_conversion (NOTES.md Part J): an equipment type's convert-from list
// (the list every conversion test reads), the generation chain, the naval test and the hook
// targets. Every offset is read from the instructions that use it, and each one the engine uses in
// two places is read from both and compared.
#include <string.h>

#include "engine.h"
#include "loader.h"
#include "signatures.h"

namespace Engine {

void* pConvertListBuild = nullptr;
void* pCanConvert = nullptr;
void* pConversionCost = nullptr;
void* pLineRebuild = nullptr;
void* pConvertClick = nullptr;
void* pConvertTooltip = nullptr;

namespace {

template <typename T>
T Rd(const void* base, size_t offset) {
    T v;
    memcpy(&v, (const uint8_t*)base + offset, sizeof v);
    return v;
}

using IsNaval_t = bool(__fastcall*)(const void* type);
// The list's allocator (CPdxArray's): vtable slot alloc(this, size, align) / free(this, p).
using Alloc_t = void*(__fastcall*)(void* allocator, size_t size, size_t align);
using Free_t = void(__fastcall*)(void* allocator, void* p);

IsNaval_t g_isNaval = nullptr;
uint32_t g_list = 0, g_cap = 0, g_count = 0, g_allocator = 0;  // type -> convert-from list
uint32_t g_allocSlot = 0, g_freeSlot = 0;                       // byte offsets in the allocator vtable
uint32_t g_parent = 0, g_depth = 0, g_archetype = 0, g_name = 0;  // type fields
uint32_t g_designType = 0, g_designParent = 0;                  // design (equipment variant) fields
uint32_t g_designRole = 0;                                      // design unit-type bits (its role)
uint32_t g_lineTarget = 0, g_itemLine = 0;                      // line -> target design, line UI item -> line
using Append_t = void*(__fastcall*)(EngineString* s, const char* p, size_t n);
Append_t g_append = nullptr;
using Friendly_t = bool(__fastcall*)(const int* tag, int other);
using DesignName_t = EngineString*(__fastcall*)(const void* design, EngineString* out);
uint32_t g_designObsolete = 0, g_designOwner = 0;  // design: "use in conversion" byte, owner tag
Friendly_t g_friendly = nullptr;
DesignName_t g_designName = nullptr;
bool g_ok = false;

// [p, p + len) lies inside hoi4.exe's .text: nothing is read outside the image (a wildcard byte that
// happens to be E8 can point anywhere on another build).
bool InText(const uint8_t* p, size_t len) {
    static uint8_t* text = nullptr;
    static size_t size = 0;
    if (!text && !ImageSection(".text", &text, &size)) return false;
    return p >= text && len <= size && p <= text + size - len;
}

bool CallsTarget(const uint8_t* fn, size_t len, const void* target) {
    if (!InText(fn, len)) return false;
    for (size_t i = 0; i + 5 <= len; ++i)
        if (fn[i] == 0xE8 && fn + i + 5 + Rd<int32_t>(fn, i + 1) == target) return true;
    return false;
}

bool Fail(const char* why) {
    Log("ERROR: equipment conversion: %s", why);
    return false;
}

// The tooltip's list of designs the role mode leaves out: the stockpile walk's skip rule and the
// design name. Optional on top of the role mode.
void ResolveSkippedList(uint8_t* walk) {
    uint8_t* skip = SigFindIn(walk, 0x100, kCodeStockpileSkip);
    uint8_t* lea = AnchorFindLea("convert_order_entry", kAnchorConvertOrderEntry);
    uint8_t* before = lea ? SigFindIn(lea - kConvOrderEntryNameBefore, kConvOrderEntryNameBefore, kCodeConvVariantName) : nullptr;
    uint8_t* after = lea ? SigFindIn(lea, kConvOrderEntryNameAfter, kCodeConvVariantName) : nullptr;
    if (!skip || !before || !after) {
        Log("Equipment conversion: the list of left-out designs is unavailable (a code shape is missing)");
        return;
    }
    uint8_t* name = SigFollowRel32(before + kCodeConvVariantNameCall);
    if (name != SigFollowRel32(after + kCodeConvVariantNameCall)) {
        Log("Equipment conversion: the order-of-conversion entry calls two name functions; list of left-out designs unavailable");
        return;
    }
    g_designObsolete = Rd<uint32_t>(skip, kCodeStockpileObsoleteDisp);
    g_designOwner = Rd<uint8_t>(skip, kCodeStockpileOwnerDisp);
    g_friendly = (Friendly_t)SigFollowRel32(skip + kCodeStockpileFriendlyCall);
    g_designName = (DesignName_t)name;
}

// The role mode (Shift+click on a line's convert button): optional, the base feature works
// without it. Needs g_designType / g_count / g_designParent from ResolveImpl.
void ResolveRoleMode() {
    uint8_t* rebuild = SigFindUnique("line_rebuild_sources", kSigLineRebuildSources);
    uint8_t* click = SigFindUnique("convert_click", kSigConvertClick);
    uint8_t* tip = SigFindUnique("convert_tooltip", kSigConvertTooltip);
    uint8_t* header = AnchorFindLea("convert_header", kAnchorConvertHeader);
    if (!rebuild || !click || !tip || !header) {
        Log("Equipment conversion: role mode unavailable (a signature is missing)");
        return;
    }
    uint32_t target = Rd<uint32_t>(rebuild, kSigRebuildTargetDisp);
    if (Rd<uint32_t>(rebuild, kSigRebuildTypeDisp) != g_designType || Rd<uint32_t>(rebuild, kSigRebuildParentDisp) != g_designParent ||
        Rd<uint32_t>(rebuild, kSigRebuildCountDisp) != g_count || Rd<uint32_t>(tip, kSigTooltipTargetDisp) != target) {
        Log("ERROR: equipment conversion: the line rebuild / tooltip offsets disagree; role mode unavailable");
        return;
    }
    // The rebuild must reach CanConvert: one of its calls goes to the stockpile walk that calls it.
    bool reaches = false;
    uint8_t* walk = nullptr;
    // Only E8 bytes whose target lies inside .text are followed (CallsTarget checks the range).
    for (size_t i = 0; i + 5 <= 0x100 && !reaches && InText(rebuild, 0x100); ++i)
        if (rebuild[i] == 0xE8) {
            walk = rebuild + i + 5 + Rd<int32_t>(rebuild, i + 1);
            reaches = CallsTarget(walk, 0x100, pCanConvert);
        }
    if (reaches) ResolveSkippedList(walk);
    uint32_t line = Rd<uint32_t>(click, kSigClickLineDisp);
    uint8_t* append = SigFindIn(tip, kConvTooltipAppendWithin, kCodeConvTipAppend);
    if (!reaches || line != Rd<uint32_t>(click, kSigClickLineDisp2) || header < tip || header >= tip + kConvTooltipAnchorWithin ||
        !append || g_designRole == 0 || g_designRole > 0x2000) {
        Log("ERROR: equipment conversion: the line rebuild / click / tooltip have an unexpected shape; role mode unavailable");
        return;
    }
    g_lineTarget = target;
    g_itemLine = line;
    g_append = (Append_t)SigFollowRel32(append + kCodeConvTipAppendCall);
    pLineRebuild = rebuild;
    pConvertClick = click;
    pConvertTooltip = tip;
    Log("Equipment conversion: role mode: line target +0x%x, item line +0x%x, design role +0x%x", g_lineTarget, g_itemLine, g_designRole);
}

bool ResolveImpl() {
    uint8_t* build = SigFindUnique("convert_list_build", kSigConvertListBuild);
    uint8_t* post = SigFindUnique("equipment_post_load", kSigEquipPostLoad);
    uint8_t* can = SigFindUnique("can_convert", kSigCanConvert);
    uint8_t* naval = SigFindUnique("equipment_is_naval", kSigEquipIsNaval);
    uint8_t* cost = SigFindUnique("conversion_cost", kSigConversionCost);
    uint8_t* anchor = AnchorFindLea("convert_from_list", kAnchorConvertFromList);
    if (!build || !post || !can || !naval || !cost || !anchor) return Fail("a signature is missing");

    // The list: from its builder (the anchor's lea must be inside it) and the grow code there.
    if (anchor < build || anchor >= build + kConvListAnchorWithin) return Fail("the can_convert_from message is not in the list builder");
    uint8_t* grow = SigFindIn(build, 0x100, kCodeConvListGrow);
    if (!grow) return Fail("the list builder has an unexpected shape (grow)");
    uint32_t count = Rd<uint32_t>(build, kSigConvListCountDisp);
    g_allocator = Rd<uint32_t>(grow, kCodeConvGrowAlloc);
    g_list = Rd<uint32_t>(grow, kCodeConvGrowList);
    g_cap = Rd<uint32_t>(grow, kCodeConvGrowCap);
    g_allocSlot = Rd<uint8_t>(grow, kCodeConvGrowAllocSlot);
    g_freeSlot = Rd<uint8_t>(grow, kCodeConvGrowFreeSlot);
    if (Rd<uint32_t>(grow, kCodeConvGrowCount) != count || Rd<uint32_t>(grow, kCodeConvGrowAlloc2) != g_allocator ||
        Rd<uint32_t>(grow, kCodeConvGrowList2) != g_list || Rd<uint32_t>(grow, kCodeConvGrowList3) != g_list)
        return Fail("the list builder's field offsets disagree");
    if (g_allocSlot == g_freeSlot || g_allocSlot % 8 || g_freeSlot % 8) return Fail("the allocator slots are implausible");
    g_count = count;
    if (!SigMatchAt(anchor, kCodeConvListTypeName)) return Fail("the list builder's message has an unexpected shape");
    g_name = Rd<uint8_t>(anchor, kCodeConvListTypeNameDisp);

    // CanConvert reads the same list.
    uint8_t* test = SigFindIn(can, 0x100, kCodeCanConvertList);
    if (!test) return Fail("CanConvert has an unexpected shape");
    g_designType = Rd<uint32_t>(can, kSigCanConvertTypeDisp);
    g_designParent = Rd<uint32_t>(can, kSigCanConvertParentDisp);
    if (Rd<uint32_t>(test, kCodeCanConvListCount) != g_count || Rd<uint32_t>(test, kCodeCanConvListList) != g_list ||
        Rd<uint32_t>(test, kCodeCanConvListType) != g_designType)
        return Fail("CanConvert's list offsets differ from the list builder's");
    if (EquipmentTypeOffset() && EquipmentTypeOffset() != g_designType) return Fail("the design -> type offset differs from production_data's");

    // The generation chain: the post-load counts the parent hops and ends with the list builder.
    g_parent = Rd<uint32_t>(post, kSigPostLoadParentDisp);
    uint8_t* depth = SigFindIn(post, kPostLoadCallsBuildWithin, kCodePostLoadDepth);
    if (Rd<uint32_t>(post, kSigPostLoadParentLoopDisp) != g_parent || !depth) return Fail("the post-load has an unexpected shape");
    g_depth = Rd<uint32_t>(depth, kCodePostLoadDepthDisp);
    if (!CallsTarget(post, kPostLoadCallsBuildWithin, build)) return Fail("the post-load does not call the list builder");

    // IsNaval: the archetype field, read twice and checked against production_data's.
    g_archetype = Rd<uint32_t>(naval, kSigIsNavalArchetypeDisp);
    if (Rd<uint32_t>(naval, kSigIsNavalArchetypeDisp2) != g_archetype ||
        naval + kSigIsNavalSelfCall + 5 + Rd<int32_t>(naval, kSigIsNavalSelfCall + 1) != naval)
        return Fail("IsNaval has an unexpected shape");
    if (TypeArchetypeOffset() && TypeArchetypeOffset() != g_archetype) return Fail("the archetype offset differs from production_data's");

    // ConversionCost reads the source design's type with the same offset, and its role bits.
    uint8_t* chassis = SigFindIn(cost, 0x400, kCodeConvCostChassis);
    if (!chassis || Rd<uint32_t>(chassis, kCodeConvCostTypeDisp) != g_designType) return Fail("ConversionCost has an unexpected shape");
    g_designRole = Rd<uint32_t>(chassis, kCodeConvCostRoleDisp);

    uint32_t fields[] = {g_list, g_cap, g_count, g_allocator, g_parent, g_depth, g_archetype, g_designParent};
    for (uint32_t f : fields)
        if (f == 0 || f > 0x2000) return Fail("an offset is out of range");
    g_isNaval = (IsNaval_t)naval;
    pConvertListBuild = build;
    pCanConvert = can;
    pConversionCost = cost;
    ResolveRoleMode();
    Log("Equipment conversion: list +0x%x (capacity +0x%x, count +0x%x, allocator +0x%x slots +0x%x/+0x%x), parent +0x%x, "
        "depth +0x%x, archetype +0x%x, design type +0x%x, design parent +0x%x",
        g_list, g_cap, g_count, g_allocator, g_allocSlot, g_freeSlot, g_parent, g_depth, g_archetype, g_designType, g_designParent);
    return true;
}

}  // namespace

void ResolveConversion() { g_ok = ResolveImpl(); }
bool HaveConversion() { return g_ok; }

bool HaveRoleMode() { return g_ok && pLineRebuild && pConvertClick && pConvertTooltip && g_append; }
uint64_t DesignRole(const void* design) { return Rd<uint64_t>(design, g_designRole); }
const void* LineTarget(const void* line) { return Rd<const void*>(line, g_lineTarget); }
void* ItemLine(const void* item) { return Rd<void*>(item, g_itemLine); }
void AppendEngineString(EngineString* s, const char* p, size_t n) { g_append(s, p, n); }

bool HaveSkippedList() { return HaveRoleMode() && g_designName && g_friendly && g_designObsolete && g_designOwner && HaveScopeBlocks(); }

bool StockpileUses(const void* design, int tag) {
    if (Rd<uint8_t>(design, g_designObsolete)) return true;
    const int* owner = (const int*)((const uint8_t*)design + g_designOwner);
    if (*owner == tag) return false;
    return !(*owner && tag && g_friendly(owner, tag));
}

std::string DesignName(const void* design) {
    EngineString s{};
    if (!g_designName(design, &s)) return "";
    std::string out(s.c_str(), (size_t)s.size);
    FreeEngineString(&s);
    return out;
}

const void* DesignType(const void* design) { return design ? Rd<const void*>(design, g_designType) : nullptr; }

const void* DesignRoot(const void* design) {
    for (int i = 0; design && i < 1000; ++i) {
        const void* parent = Rd<const void*>(design, g_designParent);
        if (!parent) return design;
        design = parent;
    }
    return nullptr;  // a cycle: treat as unknown
}

const void* TypeParent(const void* type) {
    const void* p = Rd<const void*>(type, g_parent);
    return p == type ? nullptr : p;
}
const void* TypeArchetype(const void* type) { return Rd<const void*>(type, g_archetype); }
bool TypeIsArchetype(const void* type) { return TypeArchetype(type) == type; }
int TypeDepth(const void* type) { return Rd<int32_t>(type, g_depth); }
bool TypeIsNaval(const void* type) { return g_isNaval(type); }
const char* TypeName(const void* type) { return ((const EngineString*)((const uint8_t*)type + g_name))->c_str(); }

int GenerationsBack(const void* target, const void* source) {
    if (!target || !source || target == source) return 0;
    const void* t = target;
    for (int g = 1; g <= 64; ++g) {
        t = TypeParent(t);
        if (!t) return 0;
        if (t == source) return g;
    }
    return 0;
}

int ConvertListCount(const void* type) { return Rd<int32_t>(type, g_count); }
const void* ConvertListAt(const void* type, int i) { return Rd<const void* const*>(type, g_list)[i]; }

bool ConvertListTruncate(void* type, int count) {
    uint8_t* t = (uint8_t*)type;
    int now = Rd<int32_t>(t, g_count);
    if (count < 0 || count > now) return false;
    memcpy(t + g_count, &count, sizeof count);  // the storage stays; the type's destructor frees it
    return true;
}

bool ConvertListAppend(void* type, const void* const* add, int n) {
    if (n <= 0) return true;
    uint8_t* t = (uint8_t*)type;
    int count = Rd<int32_t>(t, g_count), cap = Rd<int32_t>(t, g_cap);
    if (count < 0 || cap < count) return false;
    const void** data = Rd<const void**>(t, g_list);
    if (count + n > cap) {
        void* allocator = Rd<void*>(t, g_allocator);
        if (!allocator) return false;
        void** vt = *(void***)allocator;
        int newCap = count + n;
        const void** fresh = (const void**)((Alloc_t)vt[g_allocSlot / 8])(allocator, (size_t)newCap * 8, 8);
        if (!fresh) return false;
        if (count) memcpy(fresh, data, (size_t)count * 8);
        if (data) ((Free_t)vt[g_freeSlot / 8])(allocator, (void*)data);
        memcpy(t + g_list, &fresh, sizeof fresh);
        memcpy(t + g_cap, &newCap, sizeof newCap);
        data = fresh;
    }
    for (int i = 0; i < n; ++i) data[count + i] = add[i];
    count += n;
    memcpy(t + g_count, &count, sizeof count);
    return true;
}

}  // namespace Engine
