#!/usr/bin/env python3
"""Build a test map mod with several copies of the vanilla map ("planets"), stacked in z.

    python -I tools/make_planets_map.py --game "<HoI4 folder>" --out "<Documents>/mod/hoi4_loader_planets_test"
        [--tags PLA,PLB] [--gap 256] [--background <image>] [--space-height 2048]

Reads the owner's own vanilla install and writes a total-conversion map mod (NOTES Part T): copy k of
the map lies at z = k * (map height + gap), its provinces, states and strategic regions numbered after
the copies before it; the gaps between the copies are lake provinces (neither land nor sea for
movement, so nothing walks or sails from one copy to another). Above the copies a space band (NOTES.md
Part U): sea provinces in space zones, a hole per planet holding 16 hidden port provinces (each its
own state, given to a country's spaceport by script) around a lake; spaceship hulls and units. Every copy is owned entirely by one new
country (one tag per copy). The generated files are copies of game data: never commit them.
"""
import argparse
import glob
import os
import re
import struct
import sys

CRLF = "\r\n"


def fail(msg):
    sys.exit("make_planets_map: " + msg)


def write_text(path, text, bom=False):
    os.makedirs(os.path.dirname(path), exist_ok=True)
    with open(path, "w", encoding="utf-8-sig" if bom else "utf-8", newline="") as f:
        f.write(text.replace("\r\n", "\n").replace("\n", CRLF))


def write_bytes(path, data):
    os.makedirs(os.path.dirname(path), exist_ok=True)
    with open(path, "wb") as f:
        f.write(data)


def read_text(path):
    with open(path, "rb") as f:
        raw = f.read()
    for enc in ("utf-8-sig", "cp1252"):
        try:
            return raw.decode(enc)
        except UnicodeDecodeError:
            pass
    return raw.decode("latin-1")


# --- Paradox script (enough for state files) ------------------------------------------------------

TOKEN = re.compile(r'\s+|#[^\n]*|"(?:[^"\\]|\\.)*"|[{}]|[<>!]?=|[<>]|[^\s{}=<>#"]+')


def tokens(text):
    for m in TOKEN.finditer(text):
        t = m.group(0)
        if t[0].isspace() or t[0] == "#":
            continue
        yield t


def parse(text):
    """A list of (key, op, value) / bare values; a value is a string or a list (a block)."""
    toks = list(tokens(text))
    pos = 0

    def block():
        nonlocal pos
        out = []
        while pos < len(toks):
            t = toks[pos]
            if t == "}":
                pos += 1
                return out
            pos += 1
            if pos < len(toks) and toks[pos] in ("=", "<", ">", "<=", ">=", "!="):
                op = toks[pos]
                pos += 1
                if toks[pos] == "{":
                    pos += 1
                    out.append((t, op, block()))
                else:
                    out.append((t, op, toks[pos]))
                    pos += 1
            elif t == "{":
                out.append((None, None, block()))
            else:
                out.append((None, None, t))
        return out

    return block()


def dump(items, indent=0):
    pad = "\t" * indent
    lines = []
    for key, op, value in items:
        if isinstance(value, list):
            head = pad + (key + " " + op + " " if key is not None else "")
            if all(k is None and not isinstance(v, list) for k, _, v in value):
                lines.append(head + "{ " + " ".join(v for _, _, v in value) + " }")
            else:
                lines.append(head + "{")
                lines.extend(dump(value, indent + 1))
                lines.append(pad + "}")
        elif key is None:
            lines.append(pad + value)
        else:
            lines.append(pad + key + " " + op + " " + value)
    return lines


def get(items, key):
    for k, _, v in items:
        if k == key:
            return v
    return None


# --- Bitmaps and textures --------------------------------------------------------------------------

def read_bmp(path):
    with open(path, "rb") as f:
        data = f.read()
    off = struct.unpack_from("<I", data, 10)[0]
    w, h = struct.unpack_from("<ii", data, 18)
    bpp = struct.unpack_from("<H", data, 28)[0]
    if h <= 0 or bpp not in (8, 24):
        fail("%s: unexpected BMP (height %d, %d bits)" % (path, h, bpp))
    stride = (w * bpp // 8 + 3) & ~3
    rows = [data[off + r * stride: off + (r + 1) * stride] for r in range(h)]  # bottom-up: rows[0] is z = 0
    return {"head": bytearray(data[:off]), "w": w, "h": h, "bpp": bpp, "stride": stride, "rows": rows}


def write_bmp(path, bmp, rows):
    head = bmp["head"]
    size = len(head) + len(rows) * bmp["stride"]
    struct.pack_into("<I", head, 2, size)
    struct.pack_into("<i", head, 22, len(rows))
    struct.pack_into("<I", head, 34, len(rows) * bmp["stride"])
    write_bytes(path, bytes(head) + b"".join(rows))


def most_common(rows):
    counts = {}
    for row in rows:
        for b in row:
            counts[b] = counts.get(b, 0) + 1
    return max(counts, key=counts.get)


def stack_rows(rows, copies, gap_rows, gap_row, transforms=None, extra_rows=0):
    out = []
    for k in range(copies):
        if k:
            out.extend([gap_row] * gap_rows)
        t = transforms[k] if transforms else None
        out.extend(rows if t is None else [r.translate(t) for r in rows])
    out.extend([gap_row] * extra_rows)  # the space band (drawn by the loader, not from these)
    return out


def stack_plain_bmp(src, dst, copies, gap_map_rows, map_h, extra_map_rows=0):
    """A map-sized 8- or 24-bit bitmap (any resolution): the copies with a gap of the edges' most
    common value (8-bit) or pixel (24-bit)."""
    bmp = read_bmp(src)
    scale = bmp["h"] / map_h
    gap = round(gap_map_rows * scale)
    if abs(gap - gap_map_rows * scale) > 1e-6:
        fail("%s: the gap (%d map rows) is not a whole number of rows at %d high" % (src, gap_map_rows, bmp["h"]))
    edges = [bmp["rows"][0], bmp["rows"][-1]]
    if bmp["bpp"] == 8:
        gap_row = bytes([most_common(edges)]) * bmp["stride"]
    else:
        px = {}
        for row in edges:
            for i in range(0, bmp["w"] * 3, 3):
                p = row[i:i + 3]
                px[p] = px.get(p, 0) + 1
        best = max(px, key=px.get)
        gap_row = (best * bmp["w"]).ljust(bmp["stride"], b"\0")
    extra = round(extra_map_rows * scale)
    if abs(extra - extra_map_rows * scale) > 1e-6:
        fail("%s: the space band is not a whole number of rows at %d high" % (src, bmp["h"]))
    write_bmp(dst, bmp, stack_rows(bmp["rows"], copies, gap, gap_row, extra_rows=extra))
    return bmp["w"], bmp["h"]


def stack_dds(src, dst, copies, gap_map_rows, map_h, extra_map_rows=0):
    """A map-sized DDS: uncompressed 32-bit or DXT1/DXT5, its mip levels stacked as long as every part
    is a whole number of block rows (the rest of the chain is dropped). Rows are top-down; the copies
    are identical, so their order does not matter."""
    with open(src, "rb") as f:
        data = f.read()
    if data[:4] != b"DDS ":
        fail(src + ": not a DDS file")
    h, w = struct.unpack_from("<II", data, 12)
    mips = max(1, struct.unpack_from("<I", data, 28)[0])
    pf_flags = struct.unpack_from("<I", data, 80)[0]
    fourcc = data[84:88]
    bits = struct.unpack_from("<I", data, 88)[0]
    if pf_flags & 0x4 and fourcc in (b"DXT1", b"DXT5"):
        block, bsize = 4, (8 if fourcc == b"DXT1" else 16)
    elif pf_flags & 0x40 and bits == 32:
        block, bsize = 1, 4
    else:
        fail("%s: unsupported DDS format" % src)
    head = bytearray(data[:128])
    pos = 128
    levels = []
    for lvl in range(mips):
        lw, lh = max(1, w >> lvl), max(1, h >> lvl)
        bw, bh = (lw + block - 1) // block, (lh + block - 1) // block
        size = bw * bh * bsize
        levels.append((lw, lh, bw, bh, data[pos:pos + size]))
        pos += size
    gap0 = gap_map_rows * h / map_h
    extra0 = extra_map_rows * h / map_h
    out_levels = []
    for lvl, (lw, lh, bw, bh, pix) in enumerate(levels):
        gap = gap0 / (1 << lvl)
        extra = extra0 / (1 << lvl)
        if gap != int(gap) or int(gap) % block or lh % block or extra != int(extra) or int(extra) % block:
            break
        gap_blocks = int(gap) // block
        row_bytes = bw * bsize
        rows = [pix[r * row_bytes:(r + 1) * row_bytes] for r in range(bh)]
        out_levels.append(b"".join(stack_rows(rows, copies, gap_blocks, rows[0], extra_rows=int(extra) // block)))
    if not out_levels:
        fail("%s: the gap does not fit its blocks" % src)
    new_h = h * copies + int(gap0) * (copies - 1) + int(extra0)
    struct.pack_into("<I", head, 12, new_h)
    if block == 4:
        struct.pack_into("<I", head, 20, len(out_levels[0]))  # linear size of the top level
    if mips > 1:
        struct.pack_into("<I", head, 28, len(out_levels))
    write_bytes(dst, bytes(head) + b"".join(out_levels))
    return w, h


# --- Space (NOTES.md Part U) ---------------------------------------------------------------------------

def plan_space(map_w, z0, hs, copies, next_id, free_colour, cell=96):
    """The space band at z0..z0+hs: a grid of sea cells (about cell x cell), and per planet a hole of
    5 x 5 cells: its 16 border cells hidden ports (land, each its own state), its inner 3 x 3 cells a
    lake. A column of lake tiles at the map's east edge keeps the band from wrapping."""
    ws = map_w - 256
    nx, ny = max(7, round(ws / cell)), max(7, round(hs / cell))
    xb = [round(i * ws / nx) for i in range(nx + 1)]
    yb = [round(j * hs / ny) for j in range(ny + 1)]
    info, colour = {}, {}
    ports, inner, sea, edge = [], [], [], []
    holes = []
    for k in range(copies):
        ix0 = min(max(round(nx * (k + 1) / (copies + 1)) - 2, 1), nx - 6)
        iy0 = ny // 2 - 2
        holes.append((ix0, iy0))
        lake = next_id
        next_id += 1
        colour[lake] = free_colour(lake * 7919)
        inner.append((lake, k))
        n = 0
        for dy in range(5):
            for dx in range(5):
                c = (ix0 + dx, iy0 + dy)
                if 1 <= dx <= 3 and 1 <= dy <= 3:
                    info[c] = lake
                    continue
                out = (ix0 - 1, c[1]) if dx == 0 else (ix0 + 5, c[1]) if dx == 4 else (c[0], iy0 - 1) if dy == 0 else (c[0], iy0 + 5)
                ports.append({"id": next_id, "k": k, "n": n, "cell": c, "out": out})
                info[c] = next_id
                colour[next_id] = free_colour(next_id * 7919)
                next_id += 1
                n += 1
    for iy in range(ny):
        for ix in range(nx):
            if (ix, iy) in info:
                continue
            sea.append({"id": next_id, "cell": (ix, iy)})
            info[(ix, iy)] = next_id
            colour[next_id] = free_colour(next_id * 7919)
            next_id += 1
    for t in range(hs // 256):
        edge.append(next_id)
        colour[next_id] = free_colour(next_id * 7919)
        next_id += 1
    for pt in ports:
        pt["sea"] = info[pt["out"]]

    def centre(c):
        return (xb[c[0]] + xb[c[0] + 1]) / 2.0, z0 + (yb[c[1]] + yb[c[1] + 1]) / 2.0

    return {"next_id": next_id, "nx": nx, "ny": ny, "xb": xb, "yb": yb, "ws": ws, "hs": hs, "z0": z0, "info": info,
            "colour": colour, "ports": ports, "inner": inner, "sea": sea, "edge": edge, "holes": holes, "centre": centre}


def space_rows(sp, map_w):
    import bisect
    cache = {}
    rows = []
    for zl in range(sp["hs"]):
        iy = bisect.bisect_right(sp["yb"], zl) - 1
        t = zl // 256
        key = (iy, t)
        if key not in cache:
            row = bytearray()
            for ix in range(sp["nx"]):
                r, g, b = sp["colour"][sp["info"][(ix, iy)]]
                row += bytes((b, g, r)) * (sp["xb"][ix + 1] - sp["xb"][ix])
            r, g, b = sp["colour"][sp["edge"][t]]
            row += bytes((b, g, r)) * (map_w - sp["ws"])
            cache[key] = bytes(row)
        rows.append(cache[key])
    return rows


def space_definitions(sp, continent):
    lines = []
    for pid, k in sp["inner"]:
        r, g, b = sp["colour"][pid]
        lines.append("%d;%d;%d;%d;lake;false;lakes;0" % (pid, r, g, b))
    for pt in sp["ports"]:
        r, g, b = sp["colour"][pt["id"]]
        lines.append("%d;%d;%d;%d;land;true;plains;%d" % (pt["id"], r, g, b, continent))
    for c in sp["sea"]:
        r, g, b = sp["colour"][c["id"]]
        lines.append("%d;%d;%d;%d;sea;false;ocean;0" % (c["id"], r, g, b))
    for pid in sp["edge"]:
        r, g, b = sp["colour"][pid]
        lines.append("%d;%d;%d;%d;lake;false;lakes;0" % (pid, r, g, b))
    lines.sort(key=lambda l: int(l.split(";")[0]))
    return lines


SPACE_WEATHER = """	weather={
		period={
			between={ 0.0 30.11 }
			temperature={ -20.0 -20.0 }
			no_phenomenon=1.000
			rain_light=0.000
			rain_heavy=0.000
			snow=0.000
			blizzard=0.000
			arctic_water=0.000
			mud=0.000
			sandstorm=0.000
			min_snow_level=0.000
		}
	}
"""


def region_text(rid, name, provinces):
    return "strategic_region={\n\tid=%d\n\tname=\"%s\"\n\tprovinces={\n\t\t%s\n\t}\n%s}\n" % (
        rid, name, " ".join(str(p) for p in provinces), SPACE_WEATHER)


def find_blocks(items, key, out):
    """Every block named key anywhere in a parsed tree."""
    for k, _, v in items:
        if isinstance(v, list):
            if k == key:
                out.append(v)
            find_blocks(v, key, out)
    return out


def write_space_content(out, game, sp, tags, loc):
    """Spaceship hulls and units (copies of vanilla's light, cruiser and heavy hulls and their ships under
    space_ names, available from the start), the spaceport (a very expensive state building) and the
    weekly script that gives each spaceport a hidden port of its planet (owner and naval base in step)."""
    hulls = (("light", "Light"), ("cruiser", "Cruiser"), ("heavy", "Heavy"))
    for hull, label in hulls:
        src = os.path.join(game, "common", "units", "equipment", "ship_hull_%s.txt" % hull)
        tree = parse(read_text(src))
        eq = get(tree, "equipments") or []
        keep = []
        base = "ship_hull_" + hull
        for k, o, v in eq:
            if k == base or (k and re.fullmatch(re.escape(base) + r"_\d+", k)):
                v = [x for x in v if x[0] != "alias"]  # vanilla's hull keeps its alias (e.g. destroyer)
                if k != base:
                    v = [x for x in v if x[0] != "active"] + [("active", "=", "yes")]
                keep.append((k, o, v))
        text = "\n".join(dump([("equipments", "=", keep)])) + "\n"
        text = re.sub(r"\bship_hull_" + hull + r"\b", "space_hull_" + hull, text)
        text = re.sub(r"\bship_hull_" + hull + r"_(\d+)\b", r"space_hull_" + hull + r"_\1", text)
        write_text(os.path.join(out, "common", "units", "equipment", "space_hull_%s.txt" % hull), text)
        n = len([k for k, _, _ in keep if k != base])
        loc.append(' space_hull_%s:0 "%s Space Hull"' % (hull, label))
        loc.append(' space_hull_%s_desc:0 "A %s hull for ships in space."' % (hull, label.lower()))
        for i in range(1, n + 1):
            for suffix, text_ in (("", "%s Space Hull %d"), ("_short", "%s Space Hull %d")):
                loc.append(' space_hull_%s_%d%s:0 "%s"' % (hull, i, suffix, text_ % (label, i)))
            loc.append(' space_hull_%s_%d_desc:0 "A %s hull for ships in space."' % (hull, i, label.lower()))
    for unit, label in (("destroyer", "Space Destroyer"), ("light_cruiser", "Space Cruiser"), ("battleship", "Space Battleship")):
        tree = parse(read_text(os.path.join(game, "common", "units", unit + ".txt")))
        subs = get(tree, "sub_units") or []
        keep = [("space_" + k, o, v) for k, o, v in subs if k == unit]
        text = "\n".join(dump([("sub_units", "=", keep)])) + "\n"
        text = re.sub(r"\bship_hull_(light|cruiser|heavy)\b", r"space_hull_\1", text)
        write_text(os.path.join(out, "common", "units", "space_%s.txt" % unit), text)
        loc.append(' space_%s:0 "%s"' % (unit, label))
        loc.append(' space_%s_desc:0 "A warship built for space."' % unit)
        for what in ("training", "mission", "combat"):
            loc.append(' modifier_experience_gain_space_%s_%s_factor:0 "%s experience gain from %s"' % (unit, what, label, what))
    write_text(os.path.join(out, "common", "buildings", "hoi4_loader_space_buildings.txt"),
               "buildings = {\n\tspace_port = {\n\t\tbase_cost = 40000\n\t\ticon_frame = 6\n\t\tvalue = 1\n\t\ttags = { }\n"
               "\t\tinfrastructure_construction_effect = yes\n\t\tlevel_cap = {\n\t\t\tstate_max = 1\n\t\t\tshares_slots = yes\n\t\t}\n\t}\n}\n")
    loc.append(' space_port:0 "Spaceport"')
    loc.append(' space_port_plural:0 "Spaceports"')
    loc.append(' space_port_desc:0 "Launches spaceships from this state: its country can deploy, base and repair spaceships at the planet. Very expensive."')
    # The script: per planet, pair every spaceport with a free hidden port of that planet; free the
    # ports of spaceports that are gone; keep each port's owner and naval base in step.
    base_10, base_0 = [], []
    for pt in sp["ports"]:
        for lst, lvl in ((base_10, 10), (base_0, 0)):
            lst.append("\t%s = {\n\t\tlimit = { state = %d }\n\t\tset_building_level = { type = naval_base level = %d province = %d instant_build = yes }\n\t}\n"
                       % ("if" if not lst else "else_if", pt["state"], lvl, pt["id"]))
    eff = ["hl_space_port_base_on = {\n" + "".join(base_10) + "}\n", "hl_space_port_base_off = {\n" + "".join(base_0) + "}\n"]
    sync = []
    for k in range(len(tags)):
        sync.append("""	# Planet %(k)d: ports whose spaceport is gone go free.
	every_state = {
		limit = { has_state_flag = hl_port_%(k)d has_variable = hl_port_of var:hl_port_of = { space_port < 1 } }
		var:hl_port_of = { clear_variable = hl_port }
		clear_variable = hl_port_of
		hl_space_port_base_off = yes
	}
	# Spaceports without a port get a free one of their planet.
	every_state = {
		limit = { has_state_flag = hl_planet_%(k)d space_port > 0 NOT = { has_variable = hl_port } }
		random_state = {
			limit = { has_state_flag = hl_port_%(k)d NOT = { has_variable = hl_port_of } }
			set_variable = { hl_port_of = PREV }
			PREV = { set_variable = { hl_port = PREV } }
		}
	}
""" % {"k": k})
    sync.append("""	# Every paired port: its spaceport's owner, a naval base.
	every_state = {
		limit = { has_state_flag = hl_port has_variable = hl_port_of }
		owner = { save_event_target_as = hl_port_owner }
		if = {
			limit = { var:hl_port_of = { NOT = { is_owned_by = event_target:hl_port_owner } } }
			var:hl_port_of = { owner = { transfer_state = PREV.PREV } }
		}
		hl_space_port_base_on = yes
	}
""")
    eff.append("hl_space_port_sync = {\n" + "".join(sync) + "}\n")
    write_text(os.path.join(out, "common", "scripted_effects", "hoi4_loader_space_effects.txt"), "\n".join(eff))
    write_text(os.path.join(out, "common", "on_actions", "hoi4_loader_space_on_actions.txt"),
               "on_actions = {\n\ton_weekly = {\n\t\teffect = {\n\t\t\trandom_country = { hl_space_port_sync = yes }\n\t\t}\n\t}\n}\n")
    for n, (rid, _) in enumerate(sp["zones"]):
        loc.append(' SPACE_ZONE_%d:0 "Space Zone %d"' % (n + 1, n + 1))
    for k, tag in enumerate(tags):
        loc.append(' SPACE_ORBIT_%s:0 "Orbit of Earth %s"' % (tag, chr(ord("A") + k)))
    for pt in sp["ports"]:
        loc.append(' STATE_%d:0 "Spaceport Dock %d (Earth %s)"' % (pt["state"], pt["n"] + 1, chr(ord("A") + pt["k"])))
    loc.append(' space:0 "Space"')


def write_space_start(out, sp, tags, capitals, variants):
    """The test scenario's start: each country's capital state has a spaceport, paired with its planet's
    first hidden port, and a small space fleet based there (frigates and a cruiser of the starting
    designs)."""
    lines = ["on_actions = {", "\ton_startup = {", "\t\teffect = {",
             "\t\t\tif = {",  # once, whatever scope on_startup runs in
             "\t\t\tlimit = { NOT = { has_global_flag = hl_space_started } }",
             "\t\t\tset_global_flag = hl_space_started"]
    for k, tag in enumerate(tags):
        port = [pt for pt in sp["ports"] if pt["k"] == k and pt["n"] == 0][0]
        cap_state = capitals[tag][0]
        lines += ["\t\t\t%d = {" % cap_state,
                  "\t\t\t\tset_building_level = { type = space_port level = 1 instant_build = yes }",
                  "\t\t\t\tset_variable = { hl_port = %d }" % port["state"],
                  "\t\t\t}",
                  "\t\t\t%d = { set_variable = { hl_port_of = %d } }" % (port["state"], cap_state)]
    lines += ["\t\t\trandom_country = { hl_space_port_sync = yes }"]  # a scripted effect needs a scope
    for tag in tags:
        lines += ["\t\t\t%s = { load_oob = \"%s_space\" }" % (tag, tag)]
    lines += ["\t\t\t}", "\t\t}", "\t}", "}"]
    write_text(os.path.join(out, "common", "on_actions", "hoi4_loader_space_start.txt"), "\n".join(lines) + "\n")
    names = {"ship_hull_light_1": ("space_destroyer", "space_hull_light_1", "Orbital Frigate", 3),
             "ship_hull_cruiser_1": ("space_light_cruiser", "space_hull_cruiser_1", "Orbital Cruiser", 1)}
    for k, tag in enumerate(tags):
        port = [pt for pt in sp["ports"] if pt["k"] == k and pt["n"] == 0][0]
        ships = []
        for hull, (unit, eq, version, count) in names.items():
            if hull not in variants:
                continue
            for i in range(count):
                ships.append("\t\t\tship = { name = \"%s %d\" definition = %s equipment = { %s = { amount = 1 owner = %s version_name = \"%s\" } } }"
                             % (version, i + 1, unit, eq, tag, version))
        write_text(os.path.join(out, "history", "units", "%s_space.txt" % tag),
                   "units = {\n\tfleet = {\n\t\tname = \"First Space Fleet\"\n\t\tnaval_base = %d\n\t\ttask_force = {\n"
                   "\t\t\tname = \"First Space Fleet\"\n\t\t\tlocation = %d\n%s\n\t\t}\n\t}\n}\n"
                   % (port["id"], port["id"], "\n".join(ships)))


# --- The mod ---------------------------------------------------------------------------------------

def main():
    ap = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    ap.add_argument("--game", required=True)
    ap.add_argument("--out", required=True)
    ap.add_argument("--tags", default="PLA,PLB")
    ap.add_argument("--gap", type=int, default=256, help="map rows of lake between the copies (a multiple of 256)")
    ap.add_argument("--background", default="", help="an image (PNG, JPEG) shown behind the planets")
    ap.add_argument("--space-height", type=int, default=2048,
                    help="map rows of the space band (a multiple of 256; 0: no space)")
    args = ap.parse_args()
    game, out = args.game, args.out
    tags = [t.strip().upper() for t in args.tags.split(",") if t.strip()]
    copies = len(tags)
    if copies < 1 or any(not re.fullmatch(r"[A-Z][A-Z0-9]{2}", t) for t in tags):
        fail("--tags: three-letter tags, e.g. PLA,PLB")
    if args.gap <= 0 or args.gap % 256:
        fail("--gap: a positive multiple of 256")
    gap = args.gap
    space_h = args.space_height
    if space_h < 0 or space_h % 256:
        fail("--space-height: 0 or a multiple of 256")
    if os.path.abspath(out).lower().startswith(os.path.abspath(game).lower()):
        fail("--out must not be inside the game folder")
    vmap = os.path.join(game, "map")

    # Provinces.
    defs = []
    for line in read_text(os.path.join(vmap, "definition.csv")).splitlines():
        f = line.split(";")
        if len(f) >= 8 and f[0].isdigit():
            defs.append(f)
    ids = [int(f[0]) for f in defs]
    if ids != list(range(len(ids))):
        fail("definition.csv: ids not 0..n in order")
    poff = ids[-1]  # copy k's province = vanilla's + k * poff
    colours = {(int(f[1]), int(f[2]), int(f[3])) for f in defs}
    transforms = [None]
    for k in range(1, copies):
        used = set(colours) | {c for t in transforms[1:] for c in t[1]}
        for key in range(1, 256):
            table = bytes((x + key * 37) % 256 ^ key for x in range(256))
            if len(set(table)) != 256:
                continue
            mapped = {(table[r], table[g], table[b]) for r, g, b in colours}
            if not mapped & used:
                transforms.append((table, mapped))
                break
        else:
            fail("no free colour mapping for copy %d" % k)
    tables = [None] + [t[0] for t in transforms[1:]]

    prov = read_bmp(os.path.join(vmap, "provinces.bmp"))
    if prov["bpp"] != 24:
        fail("provinces.bmp is not 24-bit")
    map_w, map_h = prov["w"], prov["h"]
    if map_w % 256 or map_h % 256:
        fail("the vanilla map is not a multiple of 256")
    used = set(colours)
    for t in transforms[1:]:
        used |= t[1]

    def free_colour(seed):
        c = seed
        while True:
            c = (c * 1103515245 + 12345) & 0xFFFFFF
            rgb = (c >> 16 & 255, c >> 8 & 255, c & 255)
            if rgb not in used and rgb != (0, 0, 0):
                used.add(rgb)
                return rgb

    # The gap's lakes: tiles of 256 x 256 (copy k's gap below it, for k >= 1).
    lake_tiles = map_w // 256
    lakes = []  # (id, rgb, copy above it, column)
    next_id = poff * copies + 1
    for k in range(1, copies + (1 if space_h else 0)):  # copies' gaps, and the space band's (k = copies)
        for col in range(lake_tiles):
            lakes.append((next_id, free_colour(next_id * 7919), k, col))
            next_id += 1
    space = None
    if space_h:
        space = plan_space(map_w, copies * (map_h + gap), space_h, copies, next_id, free_colour)
        next_id = space["next_id"]
    rows = []
    for k in range(copies):
        if k:
            mine = [l for l in lakes if l[2] == k]
            for r in range(gap):
                row = bytearray()
                for l in mine:
                    rr, gg, bb = l[1]
                    row += bytes((bb, gg, rr)) * 256
                rows.append(bytes(row))
        rows.extend(prov["rows"] if tables[k] is None else [r.translate(tables[k]) for r in prov["rows"]])
    if space:
        mine = [l for l in lakes if l[2] == copies]
        for r in range(gap):
            row = bytearray()
            for l in mine:
                rr, gg, bb = l[1]
                row += bytes((bb, gg, rr)) * 256
            rows.append(bytes(row))
        rows.extend(space_rows(space, map_w))
    omap = os.path.join(out, "map")
    write_bmp(os.path.join(omap, "provinces.bmp"), prov, rows)
    total_h = len(rows)
    del rows, prov
    print("provinces.bmp: %d x %d, %d copies, gap %d, space %d" % (map_w, total_h, copies, gap, space_h))

    lines = ["0;0;0;0;land;false;unknown;0"]
    for k in range(copies):
        for f in defs[1:]:
            g = list(f)
            g[0] = str(int(f[0]) + k * poff)
            if k:
                t = tables[k]
                g[1], g[2], g[3] = (str(t[int(f[1])]), str(t[int(f[2])]), str(t[int(f[3])]))
            lines.append(";".join(g))
    for lid, (r, g, b), _, _ in lakes:
        lines.append("%d;%d;%d;%d;lake;false;lakes;0" % (lid, r, g, b))
    continents = read_text(os.path.join(vmap, "continent.txt"))
    space_continent = len(re.findall(r"^\s*\w+\s*$", continents.split("{", 1)[1].split("}", 1)[0], re.M)) + 1
    if space:
        lines.extend(space_definitions(space, space_continent))
    write_text(os.path.join(omap, "definition.csv"), "\n".join(lines) + "\n")
    extra = (gap + space_h) if space_h else 0  # rows above the copies (the space band's gap and the band)

    for name in ("heightmap", "terrain", "rivers", "trees", "cities", "world_normal"):
        stack_plain_bmp(os.path.join(vmap, name + ".bmp"), os.path.join(omap, name + ".bmp"), copies, gap, map_h, extra)
    for path in glob.glob(os.path.join(vmap, "terrain", "*.dds")):
        with open(path, "rb") as f:
            hdr = f.read(20)
        h, w = struct.unpack_from("<II", hdr, 12)
        if w * map_h == h * map_w and w >= map_w // 8:  # map-sized (same proportions as the map)
            stack_dds(path, os.path.join(omap, "terrain", os.path.basename(path)), copies, gap, map_h, extra)
            print("terrain/%s: %d x %d stacked" % (os.path.basename(path), w, h))

    zstep = map_h + gap

    def zoff(k):
        return k * zstep

    def fnum(v):
        return ("%.2f" % v)

    # States.
    state_files = sorted(glob.glob(os.path.join(game, "history", "states", "*.txt")))
    states = []
    for path in state_files:
        tree = parse(read_text(path))
        st = get(tree, "state")
        if st is None:
            continue
        states.append((os.path.basename(path), st))
    soff = max(int(get(st, "id")) for _, st in states)

    def remap_state(st, k, tag):
        out_items = []
        for key, op, value in st:
            if key == "id":
                out_items.append((key, op, str(int(value) + k * soff)))
            elif key == "provinces":
                out_items.append((key, op, [(None, None, str(int(v) + k * poff)) for _, _, v in value]))
            elif key == "history":
                hist = [("owner", "=", tag), ("add_core_of", "=", tag), ("set_state_flag", "=", "hl_planet_%d" % k)]
                for hk, hop, hv in value:
                    if hk == "victory_points" and isinstance(hv, list) and len(hv) == 2:
                        hist.append((hk, hop, [(None, None, str(int(hv[0][2]) + k * poff)), hv[1]]))
                    elif hk == "buildings" and isinstance(hv, list):
                        b = []
                        for bk, bop, bv in hv:
                            if bk is not None and bk.isdigit():
                                b.append((str(int(bk) + k * poff), bop, bv))
                            else:
                                b.append((bk, bop, bv))
                        hist.append((hk, hop, b))
                out_items.append((key, op, hist))
            else:
                out_items.append((key, op, value))
        return out_items

    capitals = {}
    for k, tag in enumerate(tags):
        for fname, st in states:
            sid = int(get(st, "id")) + k * soff
            items = remap_state(st, k, tag)
            name = fname if k == 0 else re.sub(r"^\d+", str(sid), fname)[:-4] + " " + tag + ".txt"
            write_text(os.path.join(out, "history", "states", name), "\n".join(dump([("state", "=", items)])) + "\n")
            if int(get(st, "id")) == 64:
                vps = [v for hk, _, v in get(items, "history") if hk == "victory_points"]
                capitals[tag] = (sid, vps[0][0][2] if vps else get(items, "provinces")[0][2])
    if len(capitals) != copies:
        fail("state 64 (the capitals) not found")
    print("states: %d x %d (ids + %d per copy)" % (len(states), copies, soff))
    # The hidden ports: one state each, owned by their planet's country, naval base 0 (the spaceport
    # script sets it), flagged for the script.
    if space:
        sid = soff * copies
        for pt in space["ports"]:
            sid += 1
            pt["state"] = sid
            tag = tags[pt["k"]]
            write_text(os.path.join(out, "history", "states", "%d-Spaceport dock %d %s.txt" % (sid, pt["n"] + 1, tag)),
                       "state = {\n\tid = %d\n\tname = \"STATE_%d\"\n\tmanpower = 1\n\tstate_category = wasteland\n"
                       "\thistory = {\n\t\towner = %s\n\t\tadd_core_of = %s\n\t\tset_state_flag = hl_port\n"
                       "\t\tset_state_flag = hl_port_%d\n\t}\n\tprovinces = { %d }\n"
                       # No railway or sea route reaches space: the port supplies itself (as from its spaceport).
                       "\tlocal_supplies = 20.0\n}\n" % (sid, sid, tag, tag, pt["k"], pt["id"]))
        print("space: %d x %d cells, %d sea provinces, %d hidden ports" % (space["nx"], space["ny"], len(space["sea"]), len(space["ports"])))

    # Strategic regions.
    reg_files = sorted(glob.glob(os.path.join(vmap, "strategicregions", "*.txt")))
    regs = []
    for path in reg_files:
        text = read_text(path)
        rid = int(re.search(r"\bid\s*=\s*(\d+)", text).group(1))
        regs.append((os.path.basename(path), rid, text))
    roff = max(r[1] for r in regs)

    def remap_provinces_block(text, add):
        return re.sub(r"(provinces\s*=\s*\{)([^}]*)(\})",
                      lambda m: m.group(1) + " " + " ".join(str(int(x) + add) for x in re.sub(r"#[^\n]*", "", m.group(2)).split())
                      + " " + m.group(3),
                      text, count=1)

    for k in range(copies):
        for fname, rid, text in regs:
            t = re.sub(r"\bid\s*=\s*\d+", "id=%d" % (rid + k * roff), text, count=1)
            t = remap_provinces_block(t, k * poff)
            name = fname if k == 0 else re.sub(r"^\d+", str(rid + k * roff), fname)[:-4] + " " + tags[k] + ".txt"
            write_text(os.path.join(omap, "strategicregions", name), t)
    gap_region = roff * copies + 1
    gap_ids = [l[0] for l in lakes] + (space["edge"] if space else [])
    next_region = gap_region + 1
    if space:
        zones = {}
        for c in space["sea"]:
            zones.setdefault((c["cell"][0] // 6, c["cell"][1] // 5), []).append(c)
        space["zones"] = []
        for zk in sorted(zones, key=lambda z: (z[1], z[0])):
            rid = next_region
            next_region += 1
            for c in zones[zk]:
                c["zone"] = rid
            space["zones"].append((rid, zones[zk]))
            write_text(os.path.join(omap, "strategicregions", "%d-Space zone %d.txt" % (rid, len(space["zones"]))),
                       region_text(rid, "SPACE_ZONE_%d" % len(space["zones"]), [c["id"] for c in zones[zk]]))
        space["orbits"] = []
        for k in range(copies):
            rid = next_region
            next_region += 1
            provs = [pt["id"] for pt in space["ports"] if pt["k"] == k] + [pid for pid, kk in space["inner"] if kk == k]
            space["orbits"].append(rid)
            write_text(os.path.join(omap, "strategicregions", "%d-Orbit %s.txt" % (rid, tags[k])),
                       region_text(rid, "SPACE_ORBIT_%s" % tags[k], provs))
    if gap_ids:
        base = regs[0][2]
        t = re.sub(r"\bid\s*=\s*\d+", "id=%d" % gap_region, base, count=1)
        t = re.sub(r'name\s*=\s*"[^"]*"', 'name="STRATEGICREGION_PLANET_GAP"', t, count=1)
        t = re.sub(r"(provinces\s*=\s*\{)([^}]*)(\})", lambda m: m.group(1) + " " + " ".join(str(i) for i in gap_ids) + " " + m.group(3), t, count=1)
        t = re.sub(r"\bnaval_terrain\s*=\s*\w+\s*", "", t)
        write_text(os.path.join(omap, "strategicregions", "%d-Planet gap.txt" % gap_region), t)

    # Coordinate lists.
    def copy_lines(name, fn):
        src = read_text(os.path.join(vmap, name)).splitlines()
        res = []
        for k in range(copies):
            for line in src:
                if line.strip():
                    res.append(fn(line, k))
        return res

    def buildings_line(line, k):
        f = line.split(";")
        f[0] = str(int(f[0]) + k * soff)
        f[4] = fnum(float(f[4]) + zoff(k))
        if int(f[6]) > 0:
            f[6] = str(int(f[6]) + k * poff)
        return ";".join(f)

    def unitstacks_line(line, k):
        f = line.split(";")
        f[0] = str(int(f[0]) + k * poff)
        f[4] = fnum(float(f[4]) + zoff(k))
        return ";".join(f)

    def weather_line(line, k):
        f = line.split(";")
        f[0] = str(int(f[0]) + k * roff)
        f[3] = fnum(float(f[3]) + zoff(k))
        return ";".join(f)

    def chain_line(line, k):
        f = line.split()
        return " ".join(f[:2] + [str(int(p) + k * poff) for p in f[2:]])

    def node_line(line, k):
        f = line.split()
        return f[0] + " " + str(int(f[1]) + k * poff)

    buildings = copy_lines("buildings.txt", buildings_line)
    stacks = copy_lines("unitstacks.txt", unitstacks_line)
    if space:
        # Vanilla's stack kinds for a sea and a land province, all at the cell's centre.
        sea_kinds = [l.split(";")[1] for l in read_text(os.path.join(vmap, "unitstacks.txt")).splitlines() if l.startswith("2753;")]
        land_kinds = [l.split(";")[1] for l in read_text(os.path.join(vmap, "unitstacks.txt")).splitlines() if l.startswith("6521;")]
        for c in space["sea"]:
            x, z = space["centre"](c["cell"])
            stacks.extend("%d;%s;%s;9.50;%s;0.00;0.86" % (c["id"], kd, fnum(x), fnum(z)) for kd in sea_kinds)
        for pt in space["ports"]:
            x, z = space["centre"](pt["cell"])
            ox, oz = space["centre"](pt["out"])
            stacks.extend("%d;%s;%s;10.00;%s;0.00;0.86" % (pt["id"], kd, fnum(x), fnum(z)) for kd in land_kinds)
            # A vanilla coastal one-province state's building positions: per province, per coastal province
            # (naval ones: the sea next to it; the floating harbour: the province itself) and per state.
            bx, bz = fnum(x + (ox - x) * 0.3), fnum(z + (oz - z) * 0.3)
            st = pt["state"]
            for kind in ("supply_node", "bunker", "special_project_facility_spawn", "coastal_bunker", "naval_supply_hub",
                         "naval_headquarters", "air_base", "synthetic_refinery", "nuclear_reactor_spawn", "rocket_site_spawn",
                         "radar_station", "fuel_silo", "stronghold_network", "dockyard") + ("industrial_complex",) * 6 + \
                        ("arms_factory",) * 6 + ("anti_air_building",) * 3:
                buildings.append("%d;%s;%s;10.00;%s;0.00;0" % (st, kind, bx, bz))
            buildings.append("%d;naval_base_spawn;%s;10.00;%s;0.00;%d" % (st, bx, bz, pt["sea"]))
            buildings.append("%d;floating_harbor;%s;10.00;%s;0.00;%d" % (st, bx, bz, pt["id"]))
    write_text(os.path.join(omap, "buildings.txt"), "\n".join(buildings) + "\n")
    write_text(os.path.join(omap, "unitstacks.txt"), "\n".join(stacks) + "\n")
    weather = copy_lines("weatherpositions.txt", weather_line)
    if space:
        for rid, cells in space["zones"]:
            xs = [space["centre"](c["cell"]) for c in cells]
            weather.append("%d;%s;10.00;%s;small" % (rid, fnum(sum(a for a, _ in xs) / len(xs)), fnum(sum(b for _, b in xs) / len(xs))))
        for k, rid in enumerate(space["orbits"]):
            ix0, iy0 = space["holes"][k]
            x, z = space["centre"]((ix0 + 2, iy0 + 2))
            weather.append("%d;%s;10.00;%s;small" % (rid, fnum(x), fnum(z)))
        weather.append("%d;%s;10.00;%s;small" % (gap_region, fnum(map_w / 2), fnum(space["z0"] - gap / 2)))
    for k in range(1, copies):  # one in the middle of each gap
        weather.append("%d;%s;10.00;%s;small" % (gap_region, fnum(map_w / 2), fnum(zoff(k) - gap / 2)))
    write_text(os.path.join(omap, "weatherpositions.txt"), "\n".join(weather) + "\n")
    write_text(os.path.join(omap, "railways.txt"), "\n".join(copy_lines("railways.txt", chain_line)) + "\n")
    write_text(os.path.join(omap, "supply_nodes.txt"), "\n".join(copy_lines("supply_nodes.txt", node_line)) + "\n")

    # Adjacencies and their rules.
    rules_text = read_text(os.path.join(vmap, "adjacency_rules.txt"))
    rule_blocks = parse(rules_text)
    rules_out = []
    for k in range(copies):
        for key, op, value in rule_blocks:
            if key != "adjacency_rule":
                continue
            items = []
            for rk, rop, rv in value:
                if rk == "name" and k:
                    rv = '"%s_%s"' % (rv.strip('"'), tags[k])
                elif rk == "required_provinces":
                    rv = [(None, None, str(int(v) + k * poff)) for _, _, v in rv]
                elif rk == "icon":
                    rv = str(int(rv) + k * poff)
                items.append((rk, rop, rv))
            rules_out.append((key, op, items))
    write_text(os.path.join(omap, "adjacency_rules.txt"), "\n".join(dump(rules_out)) + "\n")
    adj = read_text(os.path.join(vmap, "adjacencies.csv")).splitlines()
    adj_out = [adj[0]]
    tail = []
    for k in range(copies):
        for line in adj[1:]:
            f = line.split(";")
            if len(f) < 9 or not re.fullmatch(r"\d+", f[0].strip()):  # the end marker, comments
                if k == 0 and line.strip():
                    tail.append(line)
                continue
            for i in (0, 1, 3):
                if f[i].strip() and int(f[i]) >= 0:
                    f[i] = str(int(f[i]) + k * poff)
            for i in (5, 7):
                if f[i].strip() and int(f[i]) >= 0:
                    f[i] = str(int(f[i]) + zoff(k))
            if k and f[8].strip():
                f[8] = f[8] + "_" + tags[k]
            adj_out.append(";".join(f))
    write_text(os.path.join(omap, "adjacencies.csv"), "\n".join(adj_out + tail) + "\n")

    # The rest of map/ as it is.
    if space:
        write_text(os.path.join(omap, "continent.txt"), re.sub(r"\}", "\tspace\n}", continents, count=1))
    for name in ("default.map", "seasons.txt", "ambient_object.txt", "cities.txt", "colors.txt", "positions.txt") + (() if space else ("continent.txt",)):
        src = os.path.join(vmap, name)
        if os.path.exists(src):
            with open(src, "rb") as f:
                write_bytes(os.path.join(omap, name), f.read())
    for path in glob.glob(os.path.join(vmap, "supplyareas", "*.txt")):
        with open(path, "rb") as f:
            write_bytes(os.path.join(omap, "supplyareas", os.path.basename(path)), f.read())

    # Countries.
    techs = set()
    for path in glob.glob(os.path.join(game, "common", "technologies", "*.txt")):
        techs |= set(re.findall(r"^\t(\w+)\s*=\s*\{", read_text(path), re.M))
    wanted = ["infantry_weapons", "infantry_weapons1", "tech_support", "tech_engineers", "tech_recon",
              "gw_artillery", "early_fighter", "early_bomber", "basic_train", "early_destroyer"]
    # Germany's 1936 technologies (the ship modules for the spaceship designs among them).
    ger_path = glob.glob(os.path.join(game, "history", "countries", "GER - *.txt"))
    ger = parse(read_text(ger_path[0])) if ger_path else []
    for blk in find_blocks(ger, "set_technology", []):
        for tk, _, tv in blk:
            if tk and tv == "1" and tk not in wanted:
                wanted.append(tk)
    have = [t for t in wanted if t in techs]
    # Spaceship designs: Germany's first light / cruiser / heavy hull designs on the space hulls.
    variants = {}
    if space:
        for blk in find_blocks(ger, "create_equipment_variant", []):
            ty = get(blk, "type")
            for hull, label in (("ship_hull_light_1", "Orbital Frigate"), ("ship_hull_cruiser_1", "Orbital Cruiser"),
                                ("ship_hull_heavy_1", "Orbital Battleship")):
                if ty == hull and hull not in variants:
                    items = [("name", "=", '"%s"' % label), ("type", "=", hull.replace("ship_hull", "space_hull"))]
                    items += [(k2, o2, v2) for k2, o2, v2 in blk if k2 == "modules"]
                    variants[hull] = items
    palette = [(176, 62, 62), (62, 98, 176), (70, 150, 80), (190, 150, 50), (130, 70, 160), (60, 160, 160)]
    tag_lines = []
    loc = ["l_english:"]
    bookmark_countries = []
    for k, tag in enumerate(tags):
        name = "Earth " + chr(ord("A") + k)
        r, g, b = palette[k % len(palette)]
        tag_lines.append('%s = "countries/%s.txt"' % (tag, name))
        write_text(os.path.join(out, "common", "countries", name + ".txt"),
                   "graphical_culture = western_european_gfx\ngraphical_culture_2d = western_european_2d\n"
                   "color = { %d %d %d }\n" % (r, g, b))
        cap_state, cap_prov = capitals[tag]
        write_text(os.path.join(out, "history", "countries", "%s - %s.txt" % (tag, name)),
                   "capital = %d\noob = \"%s_1936\"\nset_research_slots = 3\nset_convoys = 50\n"
                   "set_technology = {\n%s}\n"
                   "set_politics = {\n\truling_party = neutrality\n\tlast_election = \"1932.1.1\"\n"
                   "\telection_frequency = 48\n\telections_allowed = no\n}\n"
                   "set_popularities = {\n\tdemocratic = 0\n\tfascism = 0\n\tcommunism = 0\n\tneutrality = 100\n}\n"
                   % (cap_state, tag, "".join("\t%s = 1\n" % t for t in have))
                   + "".join("\n".join(dump([("create_equipment_variant", "=", v)])) + "\n" for v in variants.values()))
        regs_txt = "".join("\t\tinfantry = { x = 0 y = %d }\n\t\tinfantry = { x = 1 y = %d }\n\t\tinfantry = { x = 2 y = %d }\n" % (y, y, y) for y in range(3))
        divs = "".join("\tdivision = {\n\t\tname = \"%d. %s Division\"\n\t\tlocation = %s\n\t\tdivision_template = \"Infantry Division\"\n"
                       "\t\tstart_experience_factor = 0.3\n\t}\n" % (i + 1, name, cap_prov) for i in range(12))
        write_text(os.path.join(out, "history", "units", "%s_1936.txt" % tag),
                   "division_template = {\n\tname = \"Infantry Division\"\n\tregiments = {\n%s\t}\n}\n"
                   "units = {\n%s}\n" % (regs_txt, divs))
        for suffix in ("", "_DEF", "_ADJ"):
            loc.append(' %s%s:0 "%s"' % (tag, suffix, name))
        for ideo in ("democratic", "fascism", "communism", "neutrality"):
            for suffix in ("", "_DEF", "_ADJ"):
                loc.append(' %s_%s%s:0 "%s"' % (tag, ideo, suffix, name))
        loc.append(' %s_PLANETS_DESC:0 "All of %s, one of the planets of the loader\'s planets test."' % (tag, name))
        bookmark_countries.append('\t\t"%s" = {\n\t\t\thistory = "%s_PLANETS_DESC"\n\t\t\tideology = neutrality\n'
                                  '\t\t\tideas = { }\n\t\t\tfocuses = { }\n\t\t}\n' % (tag, tag))
        for size, (fw, fh) in (("", (82, 52)), ("medium", (41, 26)), ("small", (10, 7))):
            px = bytearray()
            for y in range(fh):
                for x in range(fw):
                    stripe = fh // 3 <= y < 2 * fh // 3
                    px += bytes((min(255, b + 60), min(255, g + 60), min(255, r + 60), 255) if stripe else (b, g, r, 255))
            tga = struct.pack("<BBBHHBHHHHBB", 0, 0, 2, 0, 0, 0, 0, 0, fw, fh, 32, 8) + bytes(px)
            write_bytes(os.path.join(out, "gfx", "flags", size, tag + ".tga"), tga)
    write_text(os.path.join(out, "common", "country_tags", "hoi4_loader_planets.txt"), "\n".join(tag_lines) + "\n")
    if space:
        write_space_content(out, game, space, tags, loc)
        write_space_start(out, space, tags, capitals, variants)
    # Character names (generals, admirals, ministers), the same plain list for every planet.
    males = "Adam Bernard Carl David Emil Felix Georg Henry Isaac Jonas Karl Leon Martin Noah Oscar Paul Robert Simon Thomas Victor"
    females = "Anna Berta Clara Dora Emma Frieda Greta Hanna Ida Julia Klara Lena Maria Nora Olga Paula Rosa Sara Tilda Vera"
    surnames = "Adler Becker Brandt Fischer Graf Hartmann Keller Lang Meyer Neumann Otto Richter Schmidt Vogel Wagner Weber Wolf Zimmer"
    names = "".join("%s = {\n\tmale = { names = { %s } }\n\tfemale = { names = { %s } }\n\tsurnames = { %s }\n"
                    "\tcallsigns = { Alpha Bravo Delta Echo Falcon }\n}\n" % (tag, males, females, surnames) for tag in tags)
    write_text(os.path.join(out, "common", "names", "hoi4_loader_planets_names.txt"), names)

    # Vanilla's countries: only their capital (they own nothing in this scenario, so they do not exist).
    for path in glob.glob(os.path.join(game, "history", "countries", "*.txt")):
        m = re.search(r"^\s*capital\s*=\s*(\d+)", read_text(path), re.M)
        write_text(os.path.join(out, "history", "countries", os.path.basename(path)),
                   "capital = %s\n" % (m.group(1) if m else "1"))

    write_text(os.path.join(out, "common", "bookmarks", "hoi4_loader_planets.txt"),
               "bookmarks = {\n\tbookmark = {\n\t\tname = \"PLANETS_BOOKMARK_NAME\"\n\t\tdesc = \"PLANETS_BOOKMARK_DESC\"\n"
               "\t\tdate = 1936.1.1.12\n\t\tpicture = \"GFX_select_date_1936\"\n\t\tdefault_country = \"%s\"\n\t\tdefault = yes\n%s"
               "\t\t\"---\" = {\n\t\t\thistory = \"PLANETS_BOOKMARK_DESC\"\n\t\t}\n\t}\n}\n" % (tags[0], "".join(bookmark_countries)))
    loc.append(' PLANETS_BOOKMARK_NAME:0 "The Planets"')
    loc.append(' PLANETS_BOOKMARK_DESC:0 "%d copies of the world, one country each (hoi4_loader planets test)."' % copies)
    # No name: the game writes a sea region's name across the map, and the gaps' region surrounds the space
    # map, so its name would run across space.
    loc.append(' STRATEGICREGION_PLANET_GAP:0 ""')
    # The globe: one planet per copy, side by side in the overview (hoi4_loader's globe, NOTES.md Part T).
    planets = []
    for k, tag in enumerate(tags):
        letter = chr(ord("A") + k)
        loc.append(' PLANET_EARTH_%s:0 "Earth %s"' % (letter, letter))
        ox = (k - (copies - 1) / 2.0) * 1.4
        orbit = ""
        if space:
            # Its hole in the space map: the centre of its 5 x 5 cells; the disc reaches the hole's corners,
            # so its spaceport docks (only there for their naval bases) lie under the planet, out of sight.
            ix0, iy0 = space["holes"][k]
            cx, cz = space["centre"]((ix0 + 2, iy0 + 2))
            half = max(space["ws"] / space["nx"], space["hs"] / space["ny"]) * 2.5  # half the hole's side
            radius = int(half * 1.4142136) + 4
            orbit = "\t\torbit = { x = %.0f y = %.0f radius = %d }\n" % (cx, cz, radius)
        planets.append("\tplanet = {\n\t\tname = PLANET_EARTH_%s\n\t\tx = 0  y = %d  width = %d  height = %d\n"
                       "\t\toverview = { x = %.2f y = 0 size = 1 }\n%s\t}\n" % (letter, zoff(k), map_w, map_h, ox, orbit))
    # The camera may go higher, for the overview (hoi4_loader's camera_range raises the far plane with it).
    write_text(os.path.join(out, "common", "defines", "zz_hoi4_loader_planets.lua"),
               "-- hoi4_loader planets test: a higher camera for the overview of the planets."+"\n"
               "NDefines.NFrontend.CAMERA_MAX_HEIGHT = 6000.0"+"\n")
    space_keys = ""
    if space:
        # The space map: zoomed out past space_height over a planet's globe (NOTES.md Part U).
        space_keys = "\tspace = { x = 0 y = %d width = %d height = %d }\n\tspace_height = 2600\n" % (space["z0"], space["ws"], space_h)
    bg = ""
    if args.background:
        with open(args.background, "rb") as f:
            data = f.read()
        ext = os.path.splitext(args.background)[1].lower() or ".png"
        write_bytes(os.path.join(out, "gfx", "hoi4_loader_planets_test", "space" + ext), data)
        bg = '\tbackground = "gfx/hoi4_loader_planets_test/space%s"\n' % ext
    write_text(os.path.join(out, "common", "hoi4_loader_globe", "hoi4_loader_planets_globe.txt"),
               "# hoi4_loader's globe for the planets test (written by tools/make_planets_map.py).\n"
               "# Flat below 400; a globe from 2200 or once the view reaches past the planet's edge; from 2600 the\n"
               "# overview of all planets, the whole of it at 6000 (the camera's highest, raised in common/defines;\n"
               "# the loader's camera_range moves the far plane with it).\n"
               "globe = {\n\tdefault = yes\n\tflat_height = 400\n\tglobe_height = 2200\n\ttransition = 0.35\n"
               "\tnorth = 82\n\tsouth = -60\n\tpolar_cap_north = 85\n\tpolar_cap_south = -68\n%s"
               "\toverview_height = 2600\n\toverview_full_height = 6000\n%s%s}\n" % (bg, space_keys, "".join(planets)))
    # Victory point names for the copies.
    vp = {}
    for path in glob.glob(os.path.join(game, "localisation", "english", "*.yml")):
        for m in re.finditer(r'^\s*VICTORY_POINTS_(\d+):\d*\s*"(.*)"\s*$', read_text(path), re.M):
            vp[int(m.group(1))] = m.group(2)
    for k in range(1, copies):
        for pid, name in sorted(vp.items()):
            loc.append(' VICTORY_POINTS_%d:0 "%s"' % (pid + k * poff, name))
    write_text(os.path.join(out, "localisation", "english", "hoi4_loader_planets_l_english.yml"), "\n".join(loc) + "\n", bom=True)

    write_text(os.path.join(out, "descriptor.mod"),
               'version="1"\nname="hoi4_loader planets test"\nsupported_version="1.20.*"\n'
               'replace_path="history/states"\nreplace_path="map/strategicregions"\nreplace_path="common/bookmarks"\n')
    write_text(out.rstrip("/\\") + ".mod",
               'version="1"\nname="hoi4_loader planets test"\nsupported_version="1.20.*"\n'
               'replace_path="history/states"\nreplace_path="map/strategicregions"\nreplace_path="common/bookmarks"\n'
               'path="%s"\n' % os.path.abspath(out).replace("\\", "/"))
    print("done: %d planets (%s), map %d x %d, %d provinces, %d states, %d strategic regions"
          % (copies, ", ".join(tags), map_w, total_h, next_id - 1, soff * copies, roff * copies + (1 if lakes else 0)))
    print("planet k lies at z = k * %d (y in the globe settings), %d high" % (zstep, map_h))


if __name__ == "__main__":
    main()
