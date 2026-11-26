// globe (NOTES.md Parts S and T), the pure part. See globe.h.
#include "globe.h"

#include <math.h>
#include <stdlib.h>
#include <string.h>

#include <regex>

#include "script_tree.h"

namespace Globe {

namespace {

const float kPi = 3.14159265358979f;
const float kMinT = 0.0005f;  // below this the sphere is so large that the map counts as flat

bool NumberOf(const ScriptTree::Node& n, float lo, float hi, float* out) {
    if (n.block || n.value.empty()) return false;
    char* end = nullptr;
    double v = strtod(n.value.c_str(), &end);
    if (!end || *end || v < lo || v > hi) return false;
    *out = (float)v;
    return true;
}

float Clamp(float v, float lo, float hi) { return v < lo ? lo : (v > hi ? hi : v); }
float Ease(float p) {
    p = Clamp(p, 0.0f, 1.0f);
    return p * p * (3.0f - 2.0f * p);
}

// key = { r g b } with each 0..1.
bool ColorOf(const ScriptTree::Node& n, float out[3]) {
    std::vector<std::string> v;
    std::string why;
    if (!ScriptTree::ListOf(n, &v, &why) || v.size() != 3) return false;
    float c[3];
    for (size_t k = 0; k < 3; ++k) {
        char* end = nullptr;
        double x = strtod(v[k].c_str(), &end);
        if (!end || *end || x < 0 || x > 1) return false;
        c[k] = (float)x;
    }
    out[0] = c[0], out[1] = c[1], out[2] = c[2];
    return true;
}

bool PathOf(const ScriptTree::Node& n) {
    return !n.block && !n.value.empty() && n.value.find("..") == std::string::npos && n.value.find(':') == std::string::npos;
}

void ParsePlanet(const ScriptTree::Node& b, const std::string& shown, Settings* s, std::vector<Problem>* problems) {
    PlanetSettings pl;
    for (const ScriptTree::Node& n : b.children) {
        bool ok = true;
        if (ScriptTree::EqualsNoCase(n.key, "name")) ok = !n.block && !n.value.empty(), pl.name = n.value;
        else if (ScriptTree::EqualsNoCase(n.key, "x")) ok = NumberOf(n, 0, 1000000, &pl.x);
        else if (ScriptTree::EqualsNoCase(n.key, "y")) ok = NumberOf(n, 0, 1000000, &pl.y);
        else if (ScriptTree::EqualsNoCase(n.key, "width")) ok = NumberOf(n, 1, 1000000, &pl.width);
        else if (ScriptTree::EqualsNoCase(n.key, "height")) ok = NumberOf(n, 1, 1000000, &pl.height);
        else if (ScriptTree::EqualsNoCase(n.key, "north")) ok = NumberOf(n, -90, 90, &pl.north), pl.setNorth = ok;
        else if (ScriptTree::EqualsNoCase(n.key, "south")) ok = NumberOf(n, -90, 90, &pl.south), pl.setSouth = ok;
        else if (ScriptTree::EqualsNoCase(n.key, "polar_cap")) {
            float v = 0;
            ok = NumberOf(n, 0, 90, &v);
            if (ok) pl.polarCapNorth = v, pl.polarCapSouth = -v, pl.setCapNorth = pl.setCapSouth = true;
        } else if (ScriptTree::EqualsNoCase(n.key, "polar_cap_north")) ok = NumberOf(n, -90, 90, &pl.polarCapNorth), pl.setCapNorth = ok;
        else if (ScriptTree::EqualsNoCase(n.key, "polar_cap_south")) ok = NumberOf(n, -90, 90, &pl.polarCapSouth), pl.setCapSouth = ok;
        else if (ScriptTree::EqualsNoCase(n.key, "image")) {
            ok = PathOf(n);
            if (ok) pl.image = n.value, pl.imageFrom = shown;
        } else if (ScriptTree::EqualsNoCase(n.key, "overview")) {
            ok = n.block;
            for (const ScriptTree::Node& o : n.children) {
                if (ScriptTree::EqualsNoCase(o.key, "x")) ok = NumberOf(o, -1000, 1000, &pl.overviewX) && ok;
                else if (ScriptTree::EqualsNoCase(o.key, "y")) ok = NumberOf(o, -1000, 1000, &pl.overviewY) && ok;
                else if (ScriptTree::EqualsNoCase(o.key, "size")) ok = NumberOf(o, 0.01f, 1000, &pl.overviewSize) && ok;
                else problems->push_back({shown, o.line, "unknown key '" + ScriptTree::Label(o) + "' in overview"});
            }
            pl.hasOverview = ok;
        } else if (ScriptTree::EqualsNoCase(n.key, "orbit")) {
            ok = n.block;
            for (const ScriptTree::Node& o : n.children) {
                if (ScriptTree::EqualsNoCase(o.key, "x")) ok = NumberOf(o, 0, 1000000, &pl.orbitX) && ok;
                else if (ScriptTree::EqualsNoCase(o.key, "y")) ok = NumberOf(o, 0, 1000000, &pl.orbitY) && ok;
                else if (ScriptTree::EqualsNoCase(o.key, "radius")) ok = NumberOf(o, 8, 100000, &pl.orbitRadius) && ok;
                else problems->push_back({shown, o.line, "unknown key '" + ScriptTree::Label(o) + "' in orbit"});
            }
            pl.hasOrbit = ok && pl.orbitRadius > 0;
        } else {
            problems->push_back({shown, n.line, "unknown key '" + ScriptTree::Label(n) + "' in planet"});
            continue;
        }
        if (!ok) problems->push_back({shown, n.line, "bad value for " + n.key + " in planet"});
    }
    if (pl.name.empty()) {
        problems->push_back({shown, b.line, "a planet needs a name (its localisation key); ignored"});
        return;
    }
    if ((pl.width > 0) != (pl.height > 0)) {
        problems->push_back({shown, b.line, "planet " + pl.name + ": give both width and height (or neither: the whole map)"});
        pl.width = pl.height = 0;
    }
    for (PlanetSettings& old : s->planets)
        if (old.name == pl.name) {
            old = pl;
            return;
        }
    if ((int)s->planets.size() >= kMaxPlanets) {
        problems->push_back({shown, b.line, "more than 16 planets; " + pl.name + " ignored"});
        return;
    }
    s->planets.push_back(pl);
}

}  // namespace

void ParseFile(const std::string& text, const std::string& shown, Settings* s, std::vector<Problem>* problems) {
    std::vector<ScriptTree::Node> top;
    std::string error;
    int errorLine = 0;
    if (!ScriptTree::Parse(text, &top, &error, &errorLine)) problems->push_back({shown, errorLine, error});
    for (const ScriptTree::Node& g : top) {
        if (!ScriptTree::EqualsNoCase(g.key, "globe") || !g.block) {
            problems->push_back({shown, g.line, "unknown entry '" + ScriptTree::Label(g) + "' (expected globe = { ... })"});
            continue;
        }
        s->present = true;
        for (const ScriptTree::Node& n : g.children) {
            bool ok = true;
            if (ScriptTree::EqualsNoCase(n.key, "default")) ok = ScriptTree::BoolOf(n, &s->defaultOn);
            else if (ScriptTree::EqualsNoCase(n.key, "flat_height")) ok = NumberOf(n, 0, 100000, &s->flatHeight);
            else if (ScriptTree::EqualsNoCase(n.key, "globe_height")) ok = NumberOf(n, 0, 100000, &s->globeHeight);
            else if (ScriptTree::EqualsNoCase(n.key, "transition")) ok = NumberOf(n, 0, 10, &s->transition);
            else if (ScriptTree::EqualsNoCase(n.key, "north")) ok = NumberOf(n, -90, 90, &s->north);
            else if (ScriptTree::EqualsNoCase(n.key, "south")) ok = NumberOf(n, -90, 90, &s->south);
            else if (ScriptTree::EqualsNoCase(n.key, "polar_cap")) {
                float v = 0;
                ok = NumberOf(n, 0, 90, &v);
                if (ok) s->polarCapNorth = v, s->polarCapSouth = -v;
            } else if (ScriptTree::EqualsNoCase(n.key, "polar_cap_north")) ok = NumberOf(n, -90, 90, &s->polarCapNorth);
            else if (ScriptTree::EqualsNoCase(n.key, "polar_cap_south")) ok = NumberOf(n, -90, 90, &s->polarCapSouth);
            else if (ScriptTree::EqualsNoCase(n.key, "polar_sea_color")) ok = ColorOf(n, s->polarSeaColor);
            else if (ScriptTree::EqualsNoCase(n.key, "polar_cap_color")) ok = ColorOf(n, s->polarColor);
            else if (ScriptTree::EqualsNoCase(n.key, "overview_height")) ok = NumberOf(n, 0, 100000, &s->overviewHeight);
            else if (ScriptTree::EqualsNoCase(n.key, "overview_full_height")) ok = NumberOf(n, 1, 100000, &s->overviewFullHeight);
            else if (ScriptTree::EqualsNoCase(n.key, "name_font")) {
                ok = PathOf(n);
                if (ok) s->nameFont = n.value;
            }
            else if (ScriptTree::EqualsNoCase(n.key, "space_height")) ok = NumberOf(n, 1, 100000, &s->spaceHeight);
            else if (ScriptTree::EqualsNoCase(n.key, "space")) {
                ok = n.block;
                for (const ScriptTree::Node& o : n.children) {
                    if (ScriptTree::EqualsNoCase(o.key, "x")) ok = NumberOf(o, 0, 1000000, &s->spaceX) && ok;
                    else if (ScriptTree::EqualsNoCase(o.key, "y")) ok = NumberOf(o, 0, 1000000, &s->spaceY) && ok;
                    else if (ScriptTree::EqualsNoCase(o.key, "width")) ok = NumberOf(o, 64, 1000000, &s->spaceW) && ok;
                    else if (ScriptTree::EqualsNoCase(o.key, "height")) ok = NumberOf(o, 64, 1000000, &s->spaceH) && ok;
                    else problems->push_back({shown, o.line, "unknown key '" + ScriptTree::Label(o) + "' in space"});
                }
                s->hasSpace = ok && s->spaceW > 0 && s->spaceH > 0;
            } else if (ScriptTree::EqualsNoCase(n.key, "planet")) {
                if (n.block) ParsePlanet(n, shown, s, problems);
                else problems->push_back({shown, n.line, "planet must be a block"});
                continue;
            } else if (ScriptTree::EqualsNoCase(n.key, "background")) {
                ok = PathOf(n);
                if (ok) s->background = n.value, s->backgroundFrom = shown;
            } else {
                problems->push_back({shown, n.line, "unknown key '" + ScriptTree::Label(n) + "'"});
                continue;
            }
            if (!ok) problems->push_back({shown, n.line, "bad value for " + n.key});
        }
    }
    if (s->present && s->north <= s->south) {
        problems->push_back({shown, 0, "north must be above south; using 90 / -90"});
        s->north = 90.0f, s->south = -90.0f;
    }
    if (s->present && s->overviewHeight > 0 && s->overviewFullHeight <= s->overviewHeight) {
        problems->push_back({shown, 0, "overview_full_height must be above overview_height; using overview_height + 400"});
        s->overviewFullHeight = s->overviewHeight + 400.0f;
    }
}

bool WantSphere(const Settings& s, float height, bool edgeInView, SphereState* st) {
    bool want;
    if (height < s.flatHeight) want = false;
    else if (height >= s.globeHeight) want = true;
    else want = edgeInView || (st->sphere && height >= st->enterHeight * 0.85f);
    if (want && !st->sphere) st->enterHeight = height;
    st->sphere = want;
    return want;
}

std::vector<Planet> ResolvePlanets(const Settings& s, float mapW, float mapH, std::vector<std::string>* problems) {
    std::vector<Planet> out;
    std::vector<PlanetSettings> list = s.planets;
    if (list.empty()) list.push_back(PlanetSettings());
    float autoX = 0;
    for (size_t idx = 0; idx < list.size(); ++idx) {
        const PlanetSettings& ps = list[idx];
        Planet p;
        p.name = ps.name;
        p.source = (int)idx;
        p.x0 = ps.width > 0 ? ps.x : 0, p.z0 = ps.height > 0 ? ps.y : 0;
        p.w = ps.width > 0 ? ps.width : mapW, p.h = ps.height > 0 ? ps.height : mapH;
        if (p.x0 + p.w > mapW) p.w = mapW - p.x0;
        if (p.z0 + p.h > mapH) p.h = mapH - p.z0;
        if (!(p.w >= 64.0f) || !(p.h >= 64.0f)) {
            if (problems) problems->push_back("planet " + ps.name + " lies outside the map or is smaller than 64 pixels; dropped");
            continue;
        }
        p.north = ps.setNorth ? ps.north : s.north, p.south = ps.setSouth ? ps.south : s.south;
        if (p.north <= p.south) p.north = 90, p.south = -90;
        p.capNorth = ps.setCapNorth ? ps.polarCapNorth : s.polarCapNorth;
        p.capSouth = ps.setCapSouth ? ps.polarCapSouth : s.polarCapSouth;
        p.image = !ps.image.empty();
        p.hasOrbit = ps.hasOrbit && s.hasSpace;
        p.orbitX = ps.orbitX, p.orbitZ = ps.orbitY, p.orbitR = ps.orbitRadius;
        p.size = ps.hasOverview ? ps.overviewSize : p.w / mapW;
        if (ps.hasOverview) p.ox = ps.overviewX, p.oy = ps.overviewY;
        else p.ox = autoX + p.size * 0.5f, p.oy = 0;
        autoX = (ps.hasOverview ? p.ox : autoX + p.size * 0.5f) + p.size * 0.75f;
        out.push_back(p);
    }
    return out;
}

int PlanetAt(const std::vector<Planet>& planets, float x, float z) {
    for (size_t i = 0; i < planets.size(); ++i) {
        const Planet& p = planets[i];
        if (x >= p.x0 && x <= p.x0 + p.w && z >= p.z0 && z <= p.z0 + p.h) return (int)i;
    }
    return -1;
}

float OverviewScale(const std::vector<Planet>& planets, const View& v, float* cx, float* cy) {
    float xmin = 1e30f, xmax = -1e30f, ymin = 1e30f, ymax = -1e30f;
    for (const Planet& p : planets) {
        xmin = fminf(xmin, p.ox - p.size * 0.5f), xmax = fmaxf(xmax, p.ox + p.size * 0.5f);
        ymin = fminf(ymin, p.oy - p.size * 0.5f), ymax = fmaxf(ymax, p.oy + p.size * 0.5f);
    }
    *cx = (xmin + xmax) * 0.5f, *cy = (ymin + ymax) * 0.5f;
    if (planets.empty() || v.current < 0 || v.current >= (int)planets.size()) return 1.0f;
    const Planet& c = planets[v.current];
    float s0 = (c.w / (2.0f * kPi)) / (c.size * 0.5f);  // the current planet's globe
    float halfH = v.fullHeight * 0.41421356f * 0.85f;   // 45 degrees vertical, a margin
    float halfW = halfH * v.aspect;
    float ex = fmaxf((xmax - xmin) * 0.5f, 1e-3f), ey = fmaxf((ymax - ymin) * 0.5f, 1e-3f);
    float s1 = fminf(halfW / ex, halfH / ey);
    if (s1 > s0) s1 = s0;
    return s0 * powf(s1 / s0, Ease(v.p));
}

bool MakeParams(const Settings& s, const std::vector<Planet>& planets, float mapW, const View& v, Params* out) {
    *out = Params();
    int n = (int)planets.size();
    if (n < 1 || n > kMaxPlanets || v.current < 0 || v.current >= n || !(mapW > 1.0f)) return false;
    bool overview = v.p > 0.0f && !v.space;
    if (!overview && !v.space && !(v.t > kMinT)) return false;
    float t = overview ? 1.0f : (v.t > 1.0f ? 1.0f : v.t);
    float cx = 0, cy = 0, scale = 1;
    if (overview) scale = OverviewScale(planets, v, &cx, &cy);
    float fx = v.focusX, fy = v.focusY;
    out->a[0] = t, out->a[1] = overview ? Clamp(v.p, 0.0f, 1.0f) : 0.0f, out->a[2] = mapW, out->a[3] = 1.0f / mapW;
    out->b[0] = (float)n, out->b[1] = (float)v.current, out->b[2] = overview ? Ease(v.p) : 0.0f, out->b[3] = v.background ? 1.0f : 0.0f;
    out->e[0] = s.polarColor[0], out->e[1] = s.polarColor[1], out->e[2] = s.polarColor[2];
    out->f[0] = s.polarSeaColor[0], out->f[1] = s.polarSeaColor[1], out->f[2] = s.polarSeaColor[2];
    out->u[1] = v.lookX;
    if (v.space) {
        out->a[0] = 1.0f, out->b[2] = 1.0f, out->u[0] = 1.0f;
        out->s[0] = s.spaceX, out->s[1] = s.spaceY, out->s[2] = s.spaceW, out->s[3] = s.spaceH;
    }
    for (int i = 0; i < n; ++i) {
        const Planet& p = planets[i];
        float(*q)[4] = &out->pl[i * 8];
        bool cur = i == v.current;
        float rs = p.w / (2.0f * kPi);  // the planet's own radius: its width is the equator
        float span = (p.north - p.south) * kPi / 180.0f;
        float r, radPerX, radPerZ;
        if (cur && !overview) {
            r = rs / t;
            float stretch = 1.0f + (rs * span / p.h - 1.0f) * t;  // 1 flat, the latitude band's at t = 1
            radPerX = 1.0f / r, radPerZ = stretch / r;
        } else {
            r = overview ? p.size * 0.5f * scale : rs;
            radPerX = 2.0f * kPi / p.w, radPerZ = span / p.h;
        }
        float lx = cur ? v.lookX : (v.lastLook.size() > (size_t)(2 * i + 1) ? v.lastLook[2 * i] : p.x0 + p.w * 0.5f);
        float lz = cur ? v.lookZ : (v.lastLook.size() > (size_t)(2 * i + 1) ? v.lastLook[2 * i + 1] : p.z0 + p.h * 0.5f);
        float zEq = p.z0 + p.h * (0.0f - p.south) / (p.north - p.south);
        float latL = Clamp((Clamp(lz, p.z0, p.z0 + p.h) - zEq) * radPerZ, -kPi / 2, kPi / 2);
        float ccx = cur && !overview ? v.lookX : v.lookX + (p.ox - fx) * scale;
        float ccz = cur && !overview ? v.lookZ : v.lookZ + (p.oy - fy) * scale;
        float kind = overview ? (p.image && v.p >= 0.5f ? 2.0f : 1.0f) : (cur ? 1.0f : 0.0f);
        if (v.space) {
            // Over the space map: every planet with an orbit is its sphere in its hole there, centred on
            // the map's level (so from any angle its outline covers the whole hole: nothing is seen
            // through the hole beside it); it is turned as it was last looked at.
            r = p.orbitR;
            radPerX = 2.0f * kPi / p.w, radPerZ = span / p.h;
            ccx = p.orbitX, ccz = p.orbitZ;
            kind = p.hasOrbit ? 1.0f : 0.0f;
            lx = v.lastLook.size() > (size_t)(2 * i + 1) ? v.lastLook[2 * i] : p.x0 + p.w * 0.5f;
            lz = v.lastLook.size() > (size_t)(2 * i + 1) ? v.lastLook[2 * i + 1] : p.z0 + p.h * 0.5f;
            latL = Clamp((Clamp(lz, p.z0, p.z0 + p.h) - zEq) * radPerZ, -kPi / 2, kPi / 2);
        }
        q[0][0] = p.x0, q[0][1] = p.z0, q[0][2] = p.w, q[0][3] = p.h;
        q[1][0] = ccx, q[1][1] = v.space ? 0.0f : -r, q[1][2] = ccz, q[1][3] = r;
        q[2][0] = lx, q[2][1] = lz, q[2][2] = cosf(latL), q[2][3] = sinf(latL);
        q[3][0] = zEq, q[3][1] = radPerZ, q[3][2] = radPerX, q[3][3] = kind;
        bool polar = p.north < 90.0f || p.south > -90.0f;
        float zPerDeg = p.h / (p.north - p.south);
        float iceN = p.capNorth < 90.0f ? p.z0 + (p.capNorth - p.south) * zPerDeg : 1.0e9f;
        float iceS = p.capSouth > -90.0f ? p.z0 + (p.capSouth - p.south) * zPerDeg : -1.0e9f;
        q[4][0] = iceN < p.z0 + p.h ? p.z0 + p.h : iceN, q[4][1] = iceS > p.z0 ? p.z0 : iceS;
        q[4][2] = 2.0f * zPerDeg, q[4][3] = fminf(1.0f, r / rs);
        if (p.nameRow[1] > 0 && p.nameAspect > 0) {
            float len = fminf(1.6f, p.nameAspect * 0.3f);  // along the arc, in radii; at most 0.3 high
            q[5][0] = p.nameRow[0], q[5][1] = p.nameRow[1], q[5][2] = len, q[5][3] = len / p.nameAspect;
        }
        for (int k = 0; k < 4; ++k) q[6][k] = p.imageCell[k];
        q[7][0] = p.nameFrac, q[7][1] = polar ? 1.0f : 0.0f;
    }
    return true;
}

void Bend(const Params& p, int i, const float in[3], float out[3]) {
    if (i < 0 || i >= p.count() || !(p.t() > 0.0f)) {
        out[0] = in[0], out[1] = in[1], out[2] = in[2];
        return;
    }
    const float* rc = p.rect(i);
    const float* c = p.centre(i);
    const float* l = p.look(i);
    const float* m = p.bend(i);
    float dx = in[0] - l[0];
    dx -= rc[2] * roundf(dx / rc[2]);
    float lon = dx * m[2];
    float lat = Clamp((in[2] - m[0]) * m[1], -kPi / 2, kPi / 2);
    float d[3] = {cosf(lat) * sinf(lon), cosf(lat) * cosf(lon), sinf(lat)};
    float r[3] = {d[0], d[1] * l[2] + d[2] * l[3], d[2] * l[2] - d[1] * l[3]};
    float len = c[3] + in[1] * p.pl[i * 8 + 4][3];
    out[0] = c[0] + r[0] * len;
    out[1] = c[1] + r[1] * len;
    out[2] = c[2] + r[2] * len;
}

int RayPlanet(const Params& p, const float o[3], const float dir[3], float* dist) {
    double dl = sqrt((double)dir[0] * dir[0] + (double)dir[1] * dir[1] + (double)dir[2] * dir[2]);
    if (!(dl > 1e-9)) return -1;
    double d[3] = {dir[0] / dl, dir[1] / dl, dir[2] / dl};
    int best = -1;
    double bestS = 1e30;
    for (int i = 0; i < p.count(); ++i) {
        float kind = p.kind(i);
        const float* c = p.centre(i);
        double R = c[3];
        double s = -1;
        if (kind > 0.5f && kind < 1.5f) {
            double m[3] = {o[0] - c[0], o[1] - c[1], o[2] - c[2]};
            double b = m[0] * d[0] + m[1] * d[1] + m[2] * d[2];
            double disc = b * b - (m[0] * m[0] + m[1] * m[1] + m[2] * m[2] - R * R);
            if (disc >= 0) s = -b - sqrt(disc);
        } else if (kind > 1.5f && fabs(d[1]) > 1e-9) {
            double top = c[1] + R;  // the image's disc lies flat at the sphere's top, facing the camera
            s = (top - o[1]) / d[1];
            double hx = o[0] + s * d[0] - c[0], hz = o[2] + s * d[2] - c[2];
            if (hx * hx + hz * hz > R * R) s = -1;
        }
        if (s > 0 && s < bestS) bestS = s, best = i;
    }
    if (dist) *dist = (float)bestS;
    return best;
}

bool Unbend(const Params& p, int i, const float o[3], const float dir[3], float flat[2], bool* hit) {
    *hit = false;
    double dl = sqrt((double)dir[0] * dir[0] + (double)dir[1] * dir[1] + (double)dir[2] * dir[2]);
    if (i < 0 || i >= p.count() || !(p.t() > 0.0f) || p.kind(i) < 0.5f || p.kind(i) > 1.5f || !(dl > 1e-9)) return false;
    const float* rc = p.rect(i);
    const float* cc = p.centre(i);
    const float* l = p.look(i);
    const float* bm = p.bend(i);
    double R = cc[3];
    double d[3] = {dir[0] / dl, dir[1] / dl, dir[2] / dl};
    double m[3] = {o[0] - cc[0], o[1] - cc[1], o[2] - cc[2]};
    double b = m[0] * d[0] + m[1] * d[1] + m[2] * d[2];
    double disc = b * b - (m[0] * m[0] + m[1] * m[1] + m[2] * m[2] - R * R);
    double v[3];
    if (disc >= 0 && -b - sqrt(disc) > 0) {
        double s = -b - sqrt(disc);
        for (int k = 0; k < 3; ++k) v[k] = (m[k] + s * d[k]) / R;
        *hit = true;
    } else {
        double s = -b > 0 ? -b : 0;  // the ray's point closest to the centre, pushed onto the sphere
        double q[3], ql = 0;
        for (int k = 0; k < 3; ++k) q[k] = m[k] + s * d[k], ql += q[k] * q[k];
        ql = sqrt(ql);
        if (!(ql > 1e-9)) return false;
        for (int k = 0; k < 3; ++k) v[k] = q[k] / ql;
    }
    double cl = l[2], sl = l[3];
    double y = v[1] * cl - v[2] * sl, z = v[2] * cl + v[1] * sl;
    double lat = asin(z < -1 ? -1 : (z > 1 ? 1 : z));
    double lon = atan2(v[0], y);
    double x = l[0] + lon / bm[2];
    double fz = bm[0] + lat / bm[1];
    x = rc[0] + fmod(x - rc[0], (double)rc[2]);
    if (x < rc[0]) x += rc[2];
    flat[0] = (float)x;
    flat[1] = (float)Clamp((float)fz, rc[1], rc[1] + rc[3]);
    if (fz < rc[1] || fz > rc[1] + rc[3]) *hit = false;  // the polar seas: no map there
    return true;
}

// --- Shader text ---------------------------------------------------------------------------------

namespace {

// Put in front of each patched code block (MainCode and Code blocks are global HLSL scope, after the
// game's constant buffers, so vCamPos and vCamLookAtDir are known). The guard keeps one copy per
// compile. Not DX11: the identity.
const char kHlsl[] =
    "\n"
    "#ifndef HL_GLOBE_DEFINED\n"
    "#define HL_GLOBE_DEFINED\n"
    "#ifdef PDX_DIRECTX_11\n"
    "cbuffer HL_GlobeConstants : register( b13 )\n"
    "{\n"
    "\tfloat4 HL_GlobeA;\t// fold t of the current planet (0 flat), overview progress, map width, 1 / map width\n"
    "\tfloat4 HL_GlobeB;\t// planet count, current planet, name alpha, background on\n"
    "\tfloat4 HL_GlobeE;\t// the ice's colour\n"
    "\tfloat4 HL_GlobeF;\t// the polar seas' colour\n"
    "\tfloat4 HL_GlobeS;\t// the space map's rectangle (x0, z0, w, h)\n"
    "\tfloat4 HL_GlobeU;\t// over the space map (1), the camera's look-at x, 0, 0\n"
    "\tfloat4 HL_GlobeP[ 128 ];\t// 8 per planet (globe.h)\n"
    "};\n"
    "Texture2D HL_GlobeSpace : register( t20 );\n"
    "Texture2D HL_GlobeNames : register( t21 );\n"
    "Texture2D HL_GlobeImages : register( t22 );\n"
    "// A point to drop: pushed along its view ray past the far plane, so it is clipped where it was seen\n"
    "// (a triangle partly dropped is cut there instead of smeared across the screen).\n"
    "float3 HL_GlobeAway( float3 q )\n"
    "{\n"
    "\treturn vCamPos + normalize( q - vCamPos ) * 1000000.0f;\n"
    "}\n"
    "// The planet whose rectangle holds flat point p, or -1.\n"
    "int HL_GlobePlanetOf( float3 p )\n"
    "{\n"
    "\tint nCount = (int)HL_GlobeB.x;\n"
    "\tfor ( int i = 0; i < 16; ++i )\n"
    "\t{\n"
    "\t\tif ( i >= nCount )\n"
    "\t\t\tbreak;\n"
    "\t\tfloat4 r = HL_GlobeP[ i * 8 ];\n"
    "\t\tif ( p.x >= r.x && p.x <= r.x + r.z && p.z >= r.y && p.z <= r.y + r.w )\n"
    "\t\t\treturn i;\n"
    "\t}\n"
    "\treturn -1;\n"
    "}\n"
    "// Flat point p bent onto planet i's sphere.\n"
    "float3 HL_GlobeOnPlanet( int i, float3 p )\n"
    "{\n"
    "\tfloat4 vR = HL_GlobeP[ i * 8 ];\n"
    "\tfloat4 vC = HL_GlobeP[ i * 8 + 1 ];\n"
    "\tfloat4 vL = HL_GlobeP[ i * 8 + 2 ];\n"
    "\tfloat4 vM = HL_GlobeP[ i * 8 + 3 ];\n"
    "\tfloat dx = p.x - vL.x;\n"
    "\tdx -= vR.z * round( dx / vR.z );\n"
    "\tfloat fLon = dx * vM.z;\n"
    "\tfloat fLat = clamp( ( p.z - vM.x ) * vM.y, -1.5707963f, 1.5707963f );\n"
    "\tfloat3 d = float3( cos( fLat ) * sin( fLon ), cos( fLat ) * cos( fLon ), sin( fLat ) );\n"
    "\tfloat3 r = float3( d.x, d.y * vL.z + d.z * vL.w, d.z * vL.z - d.y * vL.w );\n"
    "\treturn vC.xyz + r * ( vC.w + p.y * HL_GlobeP[ i * 8 + 4 ].w );\n"
    "}\n"
    "// The bent position of flat point p and its planet's centre. Dropped: what lies on no planet (the\n"
    "// frame, the logo, the gaps between planets), planets not drawn (or drawn as their image), and\n"
    "// everything in a wrap copy's pass (the main camera draws every planet).\n"
    "float3 HL_GlobePoint( float3 p, out float3 vCentre )\n"
    "{\n"
    "\tfloat fPass = round( ( vCamPos.x - HL_GlobeU.y ) * HL_GlobeA.w );\n"
    "\tif ( HL_GlobeU.x > 0.5f && p.x >= HL_GlobeS.x && p.x <= HL_GlobeS.x + HL_GlobeS.z && p.z >= HL_GlobeS.y && p.z <= HL_GlobeS.y + HL_GlobeS.w )\n"
    "\t{\n"
    "\t\t// The space map stays flat; inside a planet's hole (its disc is drawn there) it is dropped.\n"
    "\t\tvCentre = p - float3( 0.0f, 100000.0f, 0.0f );\n"
    "\t\tfor ( int h = 0; h < 16; ++h )\n"
    "\t\t{\n"
    "\t\t\tif ( h >= (int)HL_GlobeB.x )\n"
    "\t\t\t\tbreak;\n"
    "\t\t\tfloat4 vC = HL_GlobeP[ h * 8 + 1 ];\n"
    "\t\t\tif ( HL_GlobeP[ h * 8 + 3 ].w > 0.5f && distance( p.xz, vC.xz ) < vC.w )\n"
    "\t\t\t\treturn HL_GlobeAway( p );\n"
    "\t\t}\n"
    "\t\treturn fPass != 0.0f ? HL_GlobeAway( p ) : p;\n"
    "\t}\n"
    "\tint i = HL_GlobePlanetOf( p );\n"
    "\tif ( i < 0 )\n"
    "\t{\n"
    "\t\t// On no planet: dropped near the closest planet's edge, where its neighbours on that planet are,\n"
    "\t\t// so a triangle across the edge is cut there instead of drawn across the screen.\n"
    "\t\tint nNear = 0;\n"
    "\t\tfloat fNear = 1.0e30f;\n"
    "\t\tfor ( int j = 0; j < 16; ++j )\n"
    "\t\t{\n"
    "\t\t\tif ( j >= (int)HL_GlobeB.x )\n"
    "\t\t\t\tbreak;\n"
    "\t\t\tfloat4 r = HL_GlobeP[ j * 8 ];\n"
    "\t\t\tfloat2 c = clamp( p.xz, r.xy, r.xy + r.zw );\n"
    "\t\t\tfloat fD = dot( c - p.xz, c - p.xz );\n"
    "\t\t\tif ( fD < fNear )\n"
    "\t\t\t{\n"
    "\t\t\t\tfNear = fD;\n"
    "\t\t\t\tnNear = j;\n"
    "\t\t\t}\n"
    "\t\t}\n"
    "\t\tfloat4 vRect = HL_GlobeP[ nNear * 8 ];\n"
    "\t\tfloat3 vEdge = float3( clamp( p.x, vRect.x, vRect.x + vRect.z ), 0.0f, clamp( p.z, vRect.y, vRect.y + vRect.w ) );\n"
    "\t\tvCentre = HL_GlobeP[ nNear * 8 + 1 ].xyz;\n"
    "\t\treturn HL_GlobeAway( HL_GlobeOnPlanet( nNear, vEdge ) );\n"
    "\t}\n"
    "\tvCentre = HL_GlobeP[ i * 8 + 1 ].xyz;\n"
    "\tfloat3 q = HL_GlobeOnPlanet( i, p );\n"
    "\tfloat fKind = HL_GlobeP[ i * 8 + 3 ].w;\n"
    "\tif ( fPass != 0.0f || fKind < 0.5f || fKind > 1.5f )\n"
    "\t\tq = HL_GlobeAway( q );\n"
    "\treturn q;\n"
    "}\n"
    "float4 HL_Globe( float4 vPos )\n"
    "{\n"
    "\tif ( HL_GlobeA.x <= 0.0f || vPos.w == 0.0f )\n"
    "\t\treturn vPos;\n"
    "\tfloat3 vCentre;\n"
    "\tfloat3 q = HL_GlobePoint( vPos.xyz / vPos.w, vCentre );\n"
    "\treturn float4( q * vPos.w, vPos.w );\n"
    "}\n"
    "// What lies on the ground (borders, rivers, trees, units): as the ground.\n"
    "float4 HL_GlobeSurface( float4 vPos )\n"
    "{\n"
    "\treturn HL_Globe( vPos );\n"
    "}\n"
    "// Also drops what lies beyond its planet's horizon: for things drawn without a depth test, such as\n"
    "// the names.\n"
    "float4 HL_GlobeCull( float4 vPos )\n"
    "{\n"
    "\tif ( HL_GlobeA.x <= 0.0f || vPos.w == 0.0f )\n"
    "\t\treturn vPos;\n"
    "\tfloat3 vCentre;\n"
    "\tfloat3 q = HL_GlobePoint( vPos.xyz / vPos.w, vCentre );\n"
    "\tif ( dot( q - vCentre, vCamPos - q ) < 0.0f )\n"
    "\t\tq = HL_GlobeAway( q );\n"
    "\treturn float4( q * vPos.w, vPos.w );\n"
    "}\n"
    "// The game's map names: as HL_GlobeCull, and gone while the overview shows (the planets' names instead).\n"
    "float4 HL_GlobeLabel( float4 vPos )\n"
    "{\n"
    "\tif ( HL_GlobeA.x <= 0.0f || vPos.w == 0.0f )\n"
    "\t\treturn vPos;\n"
    "\tif ( HL_GlobeA.y > 0.0f || ( HL_GlobeU.x > 0.5f && HL_GlobePlanetOf( vPos.xyz / vPos.w ) >= 0 ) )\n"
    "\t\treturn float4( vCamPos + float3( 0.0f, -1000000.0f, 0.0f ), 1.0f );\n"
    "\treturn HL_GlobeCull( vPos );\n"
    "}\n"
    "// Nothing of it is drawn while bent (every vertex in one point): the game's water, whose coarse grid\n"
    "// would sink under the curved seabed; the seabed shows the sea instead (HL_GlobeSea).\n"
    "float4 HL_GlobeHide( float4 vPos )\n"
    "{\n"
    "\tif ( HL_GlobeA.x <= 0.0f )\n"
    "\t\treturn vPos;\n"
    "\treturn float4( vCamPos + float3( 0.0f, -1000000.0f, 0.0f ), 1.0f );\n"
    "}\n"
    "// Distance fog: across the wrap line the short way round.\n"
    "float3 HL_GlobeFogDiff( float3 vDiff )\n"
    "{\n"
    "\tif ( HL_GlobeA.x > 0.0f )\n"
    "\t\tvDiff.x -= HL_GlobeA.z * round( vDiff.x * HL_GlobeA.w );\n"
    "\treturn vDiff;\n"
    "}\n"
    "// Water: the refraction texture would show what lies behind the planet (the sky); the game's own\n"
    "// colour for water without refractions instead.\n"
    "float3 HL_GlobeWater( float3 vColor )\n"
    "{\n"
    "\treturn HL_GlobeA.x > 0.0f ? lerp( vColor, float3( 0.0f, 0.1f, 0.2f ), saturate( HL_GlobeA.x ) ) : vColor;\n"
    "}\n"
    "// The sea: on the sphere the game's water (a coarse grid) sinks under the curved seabed, so the\n"
    "// seabed takes the water's colour instead: the colour map's sea tint, as the water shader shows it\n"
    "// (its refraction lightness, darkened), blended in at the coast.\n"
    "float3 HL_GlobeSea( float3 vDiffuse, float3 vTint, float fHeight )\n"
    "{\n"
    "\tif ( HL_GlobeA.x <= 0.0f )\n"
    "\t\treturn vDiffuse;\n"
    "\tfloat fSea = saturate( ( WATER_HEIGHT - fHeight ) * 1.5f ) * saturate( HL_GlobeA.x );\n"
    "\treturn lerp( vDiffuse, vTint * WATER_COLOR_LIGHTNESS * 0.8f, fSea );\n"
    "}\n"
    "// A texture read with bilinear filtering (Load: no sampler needed), uv clamped to [ vMin, vMax ].\n"
    "float4 HL_GlobeTexel( Texture2D vTex, float2 uv, float4 vBox )\n"
    "{\n"
    "\tuint w, h;\n"
    "\tvTex.GetDimensions( w, h );\n"
    "\tif ( w == 0 || h == 0 )\n"
    "\t\treturn float4( 0.0f, 0.0f, 0.0f, 0.0f );\n"
    "\tuv = clamp( uv, vBox.xy, vBox.zw );\n"
    "\tfloat2 p = uv * float2( w, h ) - 0.5f;\n"
    "\tint2 i = int2( floor( p ) );\n"
    "\tfloat2 f = p - floor( p );\n"
    "\tint2 vMax = int2( w, h ) - 1;\n"
    "\tfloat4 a = vTex.Load( int3( clamp( i, int2( 0, 0 ), vMax ), 0 ) );\n"
    "\tfloat4 b = vTex.Load( int3( clamp( i + int2( 1, 0 ), int2( 0, 0 ), vMax ), 0 ) );\n"
    "\tfloat4 c = vTex.Load( int3( clamp( i + int2( 0, 1 ), int2( 0, 0 ), vMax ), 0 ) );\n"
    "\tfloat4 d = vTex.Load( int3( clamp( i + int2( 1, 1 ), int2( 0, 0 ), vMax ), 0 ) );\n"
    "\treturn lerp( lerp( a, b, f.x ), lerp( c, d, f.x ), f.y );\n"
    "}\n"
    "// The background behind the planets: the mod's image, fixed to the screen (the camera looks almost\n"
    "// straight down), drifting a little as the planet turns, mirrored where it repeats.\n"
    "float3 HL_GlobeBackground( float3 vColor, float3 d )\n"
    "{\n"
    "\tif ( HL_GlobeB.w <= 0.5f )\n"
    "\t\treturn vColor;\n"
    "\tfloat2 uv = d.xz / max( -d.y, 0.05f ) * 0.68f;\n"
    "\tuv = float2( 0.5f + uv.x + HL_GlobeU.y * HL_GlobeA.w * 0.5f, 0.5f - uv.y );\n"
    "\tuv = 1.0f - abs( frac( uv * 0.5f ) * 2.0f - 1.0f );\n"
    "\treturn HL_GlobeTexel( HL_GlobeSpace, uv, float4( 0.0f, 0.0f, 1.0f, 1.0f ) ).rgb;\n"
    "}\n"
    "// The space map's ground (over space): the background image instead of land and sea, as the sky behind\n"
    "// it shows it; the map modes and borders are laid over it as over any ground.\n"
    "float3 HL_GlobeSpaceGround( float3 vDiffuse, float3 vPrepos )\n"
    "{\n"
    "\tif ( HL_GlobeU.x <= 0.5f || vPrepos.x < HL_GlobeS.x || vPrepos.x > HL_GlobeS.x + HL_GlobeS.z || vPrepos.z < HL_GlobeS.y || vPrepos.z > HL_GlobeS.y + HL_GlobeS.w )\n"
    "\t\treturn vDiffuse;\n"
    "\treturn float3( 0.01f, 0.01f, 0.02f );\t// dark: the background is added to the lit output (HL_GlobeNameOut)\n"
    "}\n"
    "// Planet i's name over the point vDisc of its disc (x east, y north, in radii from its centre): along\n"
    "// an arc, as the game writes country names; rgb and coverage.\n"
    "float4 HL_GlobeName( int i, float2 vDisc )\n"
    "{\n"
    "\tfloat4 vN = HL_GlobeP[ i * 8 + 5 ];\n"
    "\tif ( HL_GlobeB.z <= 0.0f || vN.z <= 0.0f )\n"
    "\t\treturn float4( 0.0f, 0.0f, 0.0f, 0.0f );\n"
    "\tfloat2 c = float2( vDisc.x, vDisc.y + 1.8f );\n"
    "\tfloat u = 0.5f + atan2( c.x, c.y ) * 1.8f / vN.z;\n"
    "\tfloat v = 0.5f - ( length( c ) - 1.8f ) / vN.w;\n"
    "\tif ( u < 0.0f || u > 1.0f || v < 0.0f || v > 1.0f )\n"
    "\t\treturn float4( 0.0f, 0.0f, 0.0f, 0.0f );\n"
    "\tfloat fFrac = HL_GlobeP[ i * 8 + 7 ].x;\n"
    "\tfloat2 uv = float2( 0.5f + ( u - 0.5f ) * fFrac, vN.x + v * vN.y );\n"
    "\tfloat4 t = HL_GlobeTexel( HL_GlobeNames, uv, float4( 0.0f, vN.x, 1.0f, vN.x + vN.y ) );\n"
    "\t// As the game's map names: the font's own colours, its alpha as the coverage.\n"
    "\treturn float4( t.rgb, t.a * HL_GlobeB.z );\n"
    "}\n"
    "// Planet i's disc coordinates of world point q.\n"
    "float2 HL_GlobeDisc( int i, float3 q )\n"
    "{\n"
    "\tfloat4 vC = HL_GlobeP[ i * 8 + 1 ];\n"
    "\treturn ( q.xz - vC.xz ) / vC.w;\n"
    "}\n"
    "// The names over the terrain's final output (in the overview); its alpha is the glow, cleared under them.\n"
    "float4 HL_GlobeNameOut( float4 vOut, float3 vPrepos )\n"
    "{\n"
    "\t// The space map: the background image added to its (dark) lit ground; the map modes and borders\n"
    "\t// stay over it. It glows as the sky beside it does (alpha 1), so the two match.\n"
    "\tif ( HL_GlobeU.x > 0.5f && vPrepos.x >= HL_GlobeS.x && vPrepos.x <= HL_GlobeS.x + HL_GlobeS.z && vPrepos.z >= HL_GlobeS.y && vPrepos.z <= HL_GlobeS.y + HL_GlobeS.w )\n"
    "\t\treturn float4( vOut.rgb + HL_GlobeBackground( float3( 0.0f, 0.0f, 0.0f ), normalize( vPrepos - vCamPos ) ), 0.5f );\n"
    "\tif ( HL_GlobeB.z <= 0.0f )\n"
    "\t\treturn vOut;\n"
    "\tint i = HL_GlobePlanetOf( vPrepos );\n"
    "\tif ( i < 0 )\n"
    "\t\treturn vOut;\n"
    "\tfloat4 n = HL_GlobeName( i, HL_GlobeDisc( i, HL_GlobeOnPlanet( i, vPrepos ) ) );\n"
    "\treturn float4( lerp( vOut.rgb, n.rgb, n.a ), vOut.a * ( 1.0f - n.a ) );\n"
    "}\n"
    "// Behind the planets the background. Where a planet has no ground (beyond its map's north and south\n"
    "// edges) the polar seas and ice, traced on its sphere: sea from the map's edge, ice from the ice\n"
    "// latitude to the pole, its edge wandering a little with the longitude. Planets shown as their image:\n"
    "// the image on a disc facing the camera. The names over both. The colours are scaled to about what\n"
    "// the sun makes of a terrain of that colour. The alpha is the glow (bloom): the sky's 1, none on a planet.\n"
    "float4 HL_GlobeSky( float3 vColor, float3 vPos )\n"
    "{\n"
    "\tif ( HL_GlobeA.x <= 0.0f )\n"
    "\t\treturn float4( vColor, 1.0f );\n"
    "\tfloat3 d = normalize( vPos - vCamPos );\n"
    "\tfloat fOn = saturate( HL_GlobeA.x );\n"
    "\tfloat3 vBack = lerp( vColor, HL_GlobeBackground( vColor, d ), fOn );\n"
    "\tint nCount = (int)HL_GlobeB.x;\n"
    "\tint nBest = -1;\n"
    "\tfloat fBest = 1.0e30f;\n"
    "\tfor ( int i = 0; i < 16; ++i )\n"
    "\t{\n"
    "\t\tif ( i >= nCount )\n"
    "\t\t\tbreak;\n"
    "\t\tfloat fKind = HL_GlobeP[ i * 8 + 3 ].w;\n"
    "\t\tfloat4 vC = HL_GlobeP[ i * 8 + 1 ];\n"
    "\t\tfloat s = -1.0f;\n"
    "\t\tif ( fKind > 0.5f && fKind < 1.5f )\n"
    "\t\t{\n"
    "\t\t\tfloat3 m = vCamPos - vC.xyz;\n"
    "\t\t\tfloat b = dot( m, d );\n"
    "\t\t\tfloat fDisc = b * b - ( dot( m, m ) - vC.w * vC.w );\n"
    "\t\t\tif ( fDisc >= 0.0f )\n"
    "\t\t\t\ts = -b - sqrt( fDisc );\n"
    "\t\t}\n"
    "\t\telse if ( fKind > 1.5f && abs( d.y ) > 1e-6f )\n"
    "\t\t{\n"
    "\t\t\ts = ( vC.y + vC.w - vCamPos.y ) / d.y;\n"
    "\t\t\tfloat2 h = vCamPos.xz + d.xz * s - vC.xz;\n"
    "\t\t\tif ( dot( h, h ) > vC.w * vC.w )\n"
    "\t\t\t\ts = -1.0f;\n"
    "\t\t}\n"
    "\t\tif ( s > 0.0f && s < fBest )\n"
    "\t\t{\n"
    "\t\t\tfBest = s;\n"
    "\t\t\tnBest = i;\n"
    "\t\t}\n"
    "\t}\n"
    "\tif ( nBest < 0 )\n"
    "\t\treturn float4( vBack, 1.0f );\n"
    "\tfloat3 q = vCamPos + d * fBest;\n"
    "\tfloat4 vC = HL_GlobeP[ nBest * 8 + 1 ];\n"
    "\tfloat2 vDisc = HL_GlobeDisc( nBest, q );\n"
    "\tfloat3 vSurface;\n"
    "\tfloat fCover = 1.0f;\n"
    "\tif ( HL_GlobeP[ nBest * 8 + 3 ].w > 1.5f )\n"
    "\t{\n"
    "\t\tfloat4 vCell = HL_GlobeP[ nBest * 8 + 6 ];\n"
    "\t\tfloat2 uv = vCell.xy + ( float2( vDisc.x, -vDisc.y ) * 0.5f + 0.5f ) * vCell.zw;\n"
    "\t\tfloat4 t = HL_GlobeTexel( HL_GlobeImages, uv, float4( vCell.xy, vCell.xy + vCell.zw ) );\n"
    "\t\tvSurface = t.rgb * 0.35f;\n"
    "\t\tfCover = t.a;\n"
    "\t}\n"
    "\telse\n"
    "\t{\n"
    "\t\tfloat4 vL = HL_GlobeP[ nBest * 8 + 2 ];\n"
    "\t\tfloat4 vM = HL_GlobeP[ nBest * 8 + 3 ];\n"
    "\t\tfloat4 vI = HL_GlobeP[ nBest * 8 + 4 ];\n"
    "\t\tfloat4 vR = HL_GlobeP[ nBest * 8 ];\n"
    "\t\tfloat3 v = ( q - vC.xyz ) / vC.w;\n"
    "\t\tfloat y = v.y * vL.z - v.z * vL.w;\n"
    "\t\tfloat z = v.z * vL.z + v.y * vL.w;\n"
    "\t\tfloat fZ = vM.x + asin( clamp( z, -1.0f, 1.0f ) ) / vM.y;\n"
    "\t\tfloat fTurn = ( vL.x / vR.z ) * 6.2831853f + atan2( v.x, y );\n"
    "\t\tfloat fWander = ( sin( fTurn * 5.0f ) * 0.6f + sin( fTurn * 9.0f + 1.3f ) * 0.4f ) * vI.z * 0.35f;\n"
    "\t\tfloat fIce = max( saturate( ( fZ - vI.x - fWander ) / vI.z + 0.5f ),\n"
    "\t\t                  saturate( ( vI.y - fWander - fZ ) / vI.z + 0.5f ) );\n"
    "\t\tvSurface = lerp( HL_GlobeF.rgb, HL_GlobeE.rgb, fIce ) * 0.35f;\n"
    "\t\tif ( HL_GlobeP[ nBest * 8 + 7 ].y < 0.5f )\n"
    "\t\t\tvSurface = HL_GlobeF.rgb * 0.35f;\n"
    "\t}\n"
    "\tfloat4 n = HL_GlobeName( nBest, vDisc );\n"
    "\tvSurface = lerp( vSurface, n.rgb, n.a );\n"
    "\tfCover = max( fCover, n.a );\n"
    "\tfloat fMix = fOn * fCover;\n"
    "\treturn float4( lerp( vBack, vSurface, fMix ), 1.0f - fMix );\n"
    "}\n"
    "#else\n"
    "float4 HL_Globe( float4 vPos ) { return vPos; }\n"
    "float4 HL_GlobeCull( float4 vPos ) { return vPos; }\n"
    "float4 HL_GlobeLabel( float4 vPos ) { return vPos; }\n"
    "float4 HL_GlobeSurface( float4 vPos ) { return vPos; }\n"
    "float4 HL_GlobeHide( float4 vPos ) { return vPos; }\n"
    "float3 HL_GlobeFogDiff( float3 vDiff ) { return vDiff; }\n"
    "float3 HL_GlobeWater( float3 vColor ) { return vColor; }\n"
    "float4 HL_GlobeSky( float3 vColor, float3 vPos ) { return float4( vColor, 1.0f ); }\n"
    "float3 HL_GlobeSea( float3 vDiffuse, float3 vTint, float fHeight ) { return vDiffuse; }\n"
    "float3 HL_GlobeSpaceGround( float3 vDiffuse, float3 vPrepos ) { return vDiffuse; }\n"
    "float4 HL_GlobeNameOut( float4 vOut, float3 vPrepos ) { return vOut; }\n"
    "#endif\n"
    "#endif\n";

// Index just past the ')' closing the expression that starts at `from`, or npos.
size_t ExpressionEnd(const std::string& s, size_t from) {
    int depth = 0;
    for (size_t i = from; i < s.size(); ++i) {
        char ch = s[i];
        if (ch == '(') ++depth;
        else if (ch == ')') {
            if (depth == 0) return i;
            --depth;
        } else if (ch == ';' || ch == '{' || ch == '}') {
            return std::string::npos;
        }
    }
    return std::string::npos;
}

std::string PatchCode(const std::string& code, int mode, int* count) {
    std::string out = code;
    const char* fn = mode == kHide    ? "HL_GlobeHide( "
                     : mode == kCull  ? "HL_GlobeCull( "
                     : mode == kBend  ? "HL_GlobeSurface( "
                     : mode == kLabel ? "HL_GlobeLabel( "
                                      : "HL_Globe( ";
    static const std::regex mulRx(R"(mul\s*\(\s*ViewProjectionMatrix\s*,\s*)");
    // mul( ViewProjectionMatrix, X ) -> mul( ViewProjectionMatrix, HL_Globe( X ) ), from the back so
    // earlier positions stay valid.
    std::vector<std::pair<size_t, size_t>> spans;
    for (auto it = std::sregex_iterator(out.begin(), out.end(), mulRx); it != std::sregex_iterator(); ++it) {
        size_t start = (size_t)it->position() + (size_t)it->length();
        if (out.compare(start, 8, "HL_Globe") == 0) continue;
        size_t end = ExpressionEnd(out, start);
        if (end == std::string::npos) continue;
        size_t last = end;
        while (last > start && (out[last - 1] == ' ' || out[last - 1] == '\t')) --last;
        if (last > start) spans.push_back({start, last});
    }
    for (auto k = spans.rbegin(); k != spans.rend(); ++k) {
        out.insert(k->second, " )");
        out.insert(k->first, fn);
        ++*count;
    }
    // Depth bias rows: dot( P, float4( GetMatrixData( ViewProjectionMatrix, 2, 0 ), ... ) )
    static const std::regex dotRx(R"(dot\s*\(\s*([A-Za-z_]\w*)(\s*,\s*float4\s*\(\s*GetMatrixData\s*\(\s*ViewProjectionMatrix\s*,\s*2\s*,))");
    std::string res;
    size_t pos = 0;
    for (auto it = std::sregex_iterator(out.begin(), out.end(), dotRx); it != std::sregex_iterator(); ++it) {
        const std::smatch& m = *it;
        res.append(out, pos, (size_t)m.position() - pos);
        res += std::string("dot( ") + fn + m[1].str() + " )" + m[2].str();  // the same position as the mul's
        pos = (size_t)m.position() + (size_t)m.length();
        ++*count;
    }
    res.append(out, pos, std::string::npos);
    return res;
}

// Pixel shader code: `X = tex2D( WaterRefraction, ... ).rgb;` gets `X = HL_GlobeWater( X );` after it.
std::string PixelCode(const std::string& code, int* count) {
    static const std::regex rx(R"((?:float3\s+)?([A-Za-z_]\w*)\s*=\s*tex2D\s*\(\s*WaterRefraction\s*,[^;]*;)");
    std::string res;
    size_t pos = 0;
    for (auto it = std::sregex_iterator(code.begin(), code.end(), rx); it != std::sregex_iterator(); ++it) {
        const std::smatch& m = *it;
        size_t end = (size_t)m.position() + (size_t)m.length();
        res.append(code, pos, end - pos);
        res += " " + m[1].str() + " = HL_GlobeWater( " + m[1].str() + " );";
        pos = end;
        ++*count;
    }
    res.append(code, pos, std::string::npos);
    // The terrain's colour before the map modes are laid over it: the seabed gets the sea's colour on
    // the sphere (`diffuse.rgb = HL_GlobeSea( diffuse.rgb, TerrainColor.rgb, Input.prepos.y );`).
    static const std::regex gbRx(R"(\n([ \t]*)gradient_border_apply\s*\(\s*diffuse\.rgb)");
    std::smatch g;
    if (res.find("TerrainColor") != std::string::npos && res.find("Input.prepos") != std::string::npos &&
        std::regex_search(res, g, gbRx)) {
        size_t at = (size_t)g.position() + 1;
        res.insert(at, g[1].str() + "diffuse.rgb = HL_GlobeSea( diffuse.rgb, TerrainColor.rgb, Input.prepos.y );\n" + g[1].str() +
                           "diffuse.rgb = HL_GlobeSpaceGround( diffuse.rgb, Input.prepos );\n");
        ++*count;
        // The planet names over its final output: `return float4( vOut, ... );` becomes
        // `return HL_GlobeNameOut( float4( vOut, ... ), Input.prepos );`.
        static const std::regex retRx(R"(return\s+(float4\(\s*vOut\s*,))");
        std::string named;
        size_t from = 0;
        for (auto it = std::sregex_iterator(res.begin(), res.end(), retRx); it != std::sregex_iterator(); ++it) {
            size_t open = (size_t)it->position(1);
            size_t close = ExpressionEnd(res, open + 7);  // past "float4("
            if (close == std::string::npos || open < from) continue;
            named.append(res, from, open - from);
            named += "HL_GlobeNameOut( " + res.substr(open, close + 1 - open) + ", Input.prepos )";
            from = close + 1;
            ++*count;
        }
        if (from) res = named + res.substr(from);
    }
    return res;
}

// Top-level section starts: (position, true for VertexShader).
std::vector<std::pair<size_t, bool>> Sections(const std::string& s) {
    std::vector<std::pair<size_t, bool>> out;
    static const std::regex rx(R"((^|\n)(VertexShader|PixelShader)[ \t]*=[ \t\r\n]*\{)");
    for (auto it = std::sregex_iterator(s.begin(), s.end(), rx); it != std::sregex_iterator(); ++it)
        out.push_back({(size_t)it->position(2), (*it)[2].str() == "VertexShader"});
    return out;
}

// Inserts the helpers right before `anchor` (the first match) and replaces `from` with `to` after
// it; "" if either is missing or the file has the helpers already.
std::string InsertAndReplace(const std::string& text, const std::string& anchor, const std::string& from, const std::string& to) {
    if (text.find("HL_GLOBE_DEFINED") != std::string::npos) return "";
    size_t a = text.find(anchor);
    if (a == std::string::npos) return "";
    size_t f = text.find(from, a);
    if (f == std::string::npos) return "";
    std::string out = text.substr(0, f) + to + text.substr(f + from.size());
    // The helpers go at the start of the anchor's line (global scope inside the Code block).
    size_t line = out.rfind('\n', a);
    line = line == std::string::npos ? 0 : line + 1;
    return out.substr(0, line) + kHlsl + out.substr(line);
}

}  // namespace

const char* HlslCode() { return kHlsl; }

std::string PatchVertexCode(const std::string& code, int mode, int* count) {
    *count = 0;
    return PatchCode(code, mode, count);
}

std::string PatchPixelCode(const std::string& code, int* count) {
    *count = 0;
    return PixelCode(code, count);
}

std::string PatchMapShader(const std::string& shader, int mode, int* count) {
    *count = 0;
    if (shader.find("HL_GLOBE_DEFINED") != std::string::npos) return "";
    std::vector<std::pair<size_t, bool>> sections = Sections(shader);
    std::string out;
    size_t pos = 0;
    static const std::regex mainRx(R"(MainCode[ \t]+([A-Za-z_]\w*)[ \t\r\n]*\[\[)");
    for (auto it = std::sregex_iterator(shader.begin(), shader.end(), mainRx); it != std::sregex_iterator(); ++it) {
        size_t at = (size_t)it->position();
        bool vertex = false;
        for (const auto& sec : sections)
            if (sec.first < at) vertex = sec.second;
        std::string name = (*it)[1].str();
        size_t codeStart = at + (size_t)it->length();
        size_t codeEnd = shader.find("]]", codeStart);
        if (codeEnd == std::string::npos) break;
        if (vertex && name.find("Shadow") != std::string::npos) continue;
        int n = 0;
        std::string block = shader.substr(codeStart, codeEnd - codeStart);
        std::string code = vertex ? PatchCode(block, mode, &n) : PixelCode(block, &n);
        if (n == 0) continue;
        out.append(shader, pos, codeStart - pos);
        out += kHlsl;
        out += code;
        pos = codeEnd;
        *count += n;
    }
    if (*count == 0) return "";
    out.append(shader, pos, std::string::npos);
    return out;
}

std::string PatchSharedCode(const std::string& fxh) {
    return InsertAndReplace(fxh, "float CalculateDistanceFogFactor", "float3 vDiff = vCamPos - vPos;",
                            "float3 vDiff = HL_GlobeFogDiff( vCamPos - vPos );");
}

std::string PatchSkyShader(const std::string& sky) {
    // The sky's pixel shader main returns HL_GlobeSky (its colour and glow) instead.
    return InsertAndReplace(sky, "float4 main( VS_OUTPUT_SKY Input ) : PDX_COLOR", "return float4( color, 1.0f );",
                            "return HL_GlobeSky( color, Input.pos );");
}

}  // namespace Globe
