#!/usr/bin/env python3
"""Build a test map mod with several copies of the vanilla map ("planets"), stacked in z.

    python -I tools/make_planets_map.py --game "<HoI4 folder>" --out "<Documents>/mod/hoi4_loader_planets_test"
        [--tags PLA,PLB] [--gap 256] [--background <image>]

Reads the owner's own vanilla install and writes a total-conversion map mod (NOTES Part T): copy k of
the map lies at z = k * (map height + gap), its provinces, states and strategic regions numbered after
the copies before it; the gaps between the copies are lake provinces (neither land nor sea for
movement, so nothing walks or sails from one copy to another). Every copy is owned entirely by one new
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


def stack_rows(rows, copies, gap_rows, gap_row, transforms=None):
    out = []
    for k in range(copies):
        if k:
            out.extend([gap_row] * gap_rows)
        t = transforms[k] if transforms else None
        out.extend(rows if t is None else [r.translate(t) for r in rows])
    return out


def stack_plain_bmp(src, dst, copies, gap_map_rows, map_h):
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
    write_bmp(dst, bmp, stack_rows(bmp["rows"], copies, gap, gap_row))
    return bmp["w"], bmp["h"]


def stack_dds(src, dst, copies, gap_map_rows, map_h):
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
    out_levels = []
    for lvl, (lw, lh, bw, bh, pix) in enumerate(levels):
        gap = gap0 / (1 << lvl)
        if gap != int(gap) or int(gap) % block or lh % block:
            break
        gap_blocks = int(gap) // block
        row_bytes = bw * bsize
        rows = [pix[r * row_bytes:(r + 1) * row_bytes] for r in range(bh)]
        out_levels.append(b"".join(stack_rows(rows, copies, gap_blocks, rows[0])))
    if not out_levels:
        fail("%s: the gap does not fit its blocks" % src)
    new_h = h * copies + int(gap0) * (copies - 1)
    struct.pack_into("<I", head, 12, new_h)
    if block == 4:
        struct.pack_into("<I", head, 20, len(out_levels[0]))  # linear size of the top level
    if mips > 1:
        struct.pack_into("<I", head, 28, len(out_levels))
    write_bytes(dst, bytes(head) + b"".join(out_levels))
    return w, h


# --- The mod ---------------------------------------------------------------------------------------

def main():
    ap = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    ap.add_argument("--game", required=True)
    ap.add_argument("--out", required=True)
    ap.add_argument("--tags", default="PLA,PLB")
    ap.add_argument("--gap", type=int, default=256, help="map rows of lake between the copies (a multiple of 256)")
    ap.add_argument("--background", default="", help="an image (PNG, JPEG) shown behind the planets")
    args = ap.parse_args()
    game, out = args.game, args.out
    tags = [t.strip().upper() for t in args.tags.split(",") if t.strip()]
    copies = len(tags)
    if copies < 1 or any(not re.fullmatch(r"[A-Z][A-Z0-9]{2}", t) for t in tags):
        fail("--tags: three-letter tags, e.g. PLA,PLB")
    if args.gap <= 0 or args.gap % 256:
        fail("--gap: a positive multiple of 256")
    gap = args.gap
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
    for k in range(1, copies):
        for col in range(lake_tiles):
            lakes.append((next_id, free_colour(next_id * 7919), k, col))
            next_id += 1
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
    omap = os.path.join(out, "map")
    write_bmp(os.path.join(omap, "provinces.bmp"), prov, rows)
    del rows, prov
    print("provinces.bmp: %d x %d, %d copies, gap %d" % (map_w, map_h * copies + gap * (copies - 1), copies, gap))

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
    write_text(os.path.join(omap, "definition.csv"), "\n".join(lines) + "\n")

    for name in ("heightmap", "terrain", "rivers", "trees", "cities", "world_normal"):
        stack_plain_bmp(os.path.join(vmap, name + ".bmp"), os.path.join(omap, name + ".bmp"), copies, gap, map_h)
    for path in glob.glob(os.path.join(vmap, "terrain", "*.dds")):
        with open(path, "rb") as f:
            hdr = f.read(20)
        h, w = struct.unpack_from("<II", hdr, 12)
        if w * map_h == h * map_w and w >= map_w // 8:  # map-sized (same proportions as the map)
            stack_dds(path, os.path.join(omap, "terrain", os.path.basename(path)), copies, gap, map_h)
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
                hist = [("owner", "=", tag), ("add_core_of", "=", tag)]
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
    if lakes:
        base = regs[0][2]
        t = re.sub(r"\bid\s*=\s*\d+", "id=%d" % gap_region, base, count=1)
        t = re.sub(r'name\s*=\s*"[^"]*"', 'name="STRATEGICREGION_PLANET_GAP"', t, count=1)
        t = re.sub(r"(provinces\s*=\s*\{)([^}]*)(\})", lambda m: m.group(1) + " " + " ".join(str(l[0]) for l in lakes) + " " + m.group(3), t, count=1)
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

    write_text(os.path.join(omap, "buildings.txt"), "\n".join(copy_lines("buildings.txt", buildings_line)) + "\n")
    write_text(os.path.join(omap, "unitstacks.txt"), "\n".join(copy_lines("unitstacks.txt", unitstacks_line)) + "\n")
    weather = copy_lines("weatherpositions.txt", weather_line)
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
    for name in ("default.map", "continent.txt", "seasons.txt", "ambient_object.txt", "cities.txt", "colors.txt", "positions.txt"):
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
    have = [t for t in wanted if t in techs]
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
                   % (cap_state, tag, "".join("\t%s = 1\n" % t for t in have)))
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
    loc.append(' STRATEGICREGION_PLANET_GAP:0 "Between the planets"')
    # The globe: one planet per copy, side by side in the overview (hoi4_loader's globe, NOTES.md Part T).
    planets = []
    for k, tag in enumerate(tags):
        letter = chr(ord("A") + k)
        loc.append(' PLANET_EARTH_%s:0 "Earth %s"' % (letter, letter))
        ox = (k - (copies - 1) / 2.0) * 1.4
        planets.append("\tplanet = {\n\t\tname = PLANET_EARTH_%s\n\t\tx = 0  y = %d  width = %d  height = %d\n"
                       "\t\toverview = { x = %.2f y = 0 size = 1 }\n\t}\n" % (letter, zoff(k), map_w, map_h, ox))
    bg = ""
    if args.background:
        with open(args.background, "rb") as f:
            data = f.read()
        ext = os.path.splitext(args.background)[1].lower() or ".png"
        write_bytes(os.path.join(out, "gfx", "hoi4_loader_planets_test", "space" + ext), data)
        bg = '\tbackground = "gfx/hoi4_loader_planets_test/space%s"\n' % ext
    write_text(os.path.join(out, "common", "hoi4_loader_globe", "hoi4_loader_planets_globe.txt"),
               "# hoi4_loader's globe for the planets test (written by tools/make_planets_map.py).\n"
               "# Flat below 400; a globe from 2200 or once the view reaches past the planet's edge; from 2500 the\n"
               "# overview of all planets, the whole of it at 3000 (vanilla's highest camera).\n"
               "globe = {\n\tdefault = yes\n\tflat_height = 400\n\tglobe_height = 2200\n\ttransition = 0.35\n"
               "\tnorth = 82\n\tsouth = -60\n\tpolar_cap_north = 85\n\tpolar_cap_south = -68\n%s"
               "\toverview_height = 2500\n\toverview_full_height = 3000\n%s}\n" % (bg, "".join(planets)))
    # Victory point names for the copies.
    vp = {}
    for path in glob.glob(os.path.join(game, "localisation", "english", "*.yml")):
        for m in re.finditer(r'^\s*VICTORY_POINTS_(\d+):\d*\s*"(.*)"\s*$', read_text(path), re.M):
            vp[int(m.group(1))] = m.group(2)
    for k in range(1, copies):
        for pid, name in sorted(vp.items()):
            loc.append(' VICTORY_POINTS_%d:0 "%s"' % (pid + k * poff, name))
    write_text(os.path.join(out, "localisation", "english", "hoi4_loader_planets_l_english.yml"), "\n".join(loc) + "\n", bom=True)

    total_h = map_h * copies + gap * (copies - 1)
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
