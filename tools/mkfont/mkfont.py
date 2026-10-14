#!/usr/bin/env python3
# Copyright (c) 2026 Muhammad Waleed Badar
# SPDX-License-Identifier: GPL-3.0-only
# Subsets TrueType fonts to a set of Unicode ranges and writes them as C
# arrays: the built-in fallback fonts of the zpdf renderer for the standard
# 14 fonts (which PDF files do not embed). Only glyf/loca/head/hhea/hmtx/
# maxp/cmap tables are kept; the cmap is rewritten as a single (3,1)
# format 4 subtable. No external packages needed.
import struct, sys, os

RANGES = [(0x20, 0x7E), (0xA0, 0x17F), (0x192, 0x192), (0x2C6, 0x2C7), (0x2D8, 0x2DD),
          (0x391, 0x3C9), (0x2010, 0x2027), (0x2030, 0x203A), (0x2044, 0x2044),
          (0x20AC, 0x20AC), (0x2122, 0x2122), (0x2126, 0x2126), (0x2190, 0x2199),
          (0x2202, 0x2213), (0x221A, 0x221E), (0x2229, 0x222B), (0x2248, 0x2248),
          (0x2260, 0x2265), (0x25A0, 0x25A1), (0x25CF, 0x25CF), (0x2713, 0x2714),
          (0xFB00, 0xFB04)]
SYMBOL_RANGES = RANGES + [(0x2200, 0x22FF), (0x2300, 0x2320), (0x2460, 0x2473),
                          (0x25A0, 0x25FF), (0x2600, 0x2611), (0x2660, 0x2667), (0x2700, 0x27BF)]

def rd(f, fmt, off):
    return struct.unpack_from(fmt, f, off)

def tables(f):
    n = rd(f, ">H", 4)[0]
    t = {}
    for i in range(n):
        tag, cs, off, ln = rd(f, ">4sIII", 12 + i * 16)
        t[tag.decode("latin1")] = (off, ln)
    return t

def parse_cmap(f, off, ln):
    n = rd(f, ">H", off + 2)[0]
    best = None
    for i in range(n):
        pid, eid, so = rd(f, ">HHI", off + 4 + i * 8)
        score = {(3, 10): 3, (3, 1): 2, (0, 3): 1, (0, 4): 1, (0, 6): 1}.get((pid, eid), 0)
        if score and (best is None or score > best[0]):
            best = (score, off + so)
    m = {}
    if best is None:
        return m
    so = best[1]
    fmt = rd(f, ">H", so)[0]
    if fmt == 4:
        segx2 = rd(f, ">H", so + 6)[0]
        ends = so + 14; starts = ends + segx2 + 2; deltas = starts + segx2; ranges = deltas + segx2
        for s in range(segx2 // 2):
            end = rd(f, ">H", ends + s * 2)[0]; start = rd(f, ">H", starts + s * 2)[0]
            delta = rd(f, ">H", deltas + s * 2)[0]; ro = rd(f, ">H", ranges + s * 2)[0]
            for c in range(start, min(end, 0xFFFE) + 1):
                if ro == 0:
                    g = (c + delta) & 0xFFFF
                else:
                    gi = ranges + s * 2 + ro + (c - start) * 2
                    g = rd(f, ">H", gi)[0]
                    if g: g = (g + delta) & 0xFFFF
                if g: m[c] = g
    elif fmt == 12:
        ng = rd(f, ">I", so + 12)[0]
        for g in range(ng):
            a, b, gid = rd(f, ">III", so + 16 + g * 12)
            for c in range(a, b + 1):
                m[c] = gid + (c - a)
    return m

def glyph_deps(f, glyf, loca, gid, out):
    if gid in out:
        return
    out.add(gid)
    a, b = loca[gid], loca[gid + 1]
    if b - a < 10:
        return
    nc = rd(f, ">h", glyf + a)[0]
    if nc >= 0:
        return
    p = glyf + a + 10
    while True:
        flags, gi = rd(f, ">HH", p)
        glyph_deps(f, glyf, loca, gi, out)
        p += 4
        p += 4 if flags & 1 else 2
        if flags & 8: p += 2
        elif flags & 0x40: p += 4
        elif flags & 0x80: p += 8
        if not flags & 0x20:
            break

def remap_composite(f, glyf, a, b, newgid):
    d = bytearray(f[glyf + a:glyf + b])
    p = 10
    while True:
        flags, gi = struct.unpack_from(">HH", d, p)
        struct.pack_into(">H", d, p + 2, newgid[gi])
        p += 4
        p += 4 if flags & 1 else 2
        if flags & 8: p += 2
        elif flags & 0x40: p += 4
        elif flags & 0x80: p += 8
        if not flags & 0x20:
            break
    return bytes(d)

def subset(path, ranges):
    f = open(path, "rb").read()
    t = tables(f)
    head = f[t["head"][0]:t["head"][0] + t["head"][1]]
    ilf = rd(head, ">h", 50)[0]
    nglyphs = rd(f, ">H", t["maxp"][0] + 4)[0]
    lo, ll = t["loca"]
    loca = [rd(f, ">I" if ilf else ">H", lo + i * (4 if ilf else 2))[0] * (1 if ilf else 2) for i in range(nglyphs + 1)]
    glyf = t["glyf"][0]
    cmap = parse_cmap(f, *t["cmap"])
    hh = t["hhea"][0]
    nhm = rd(f, ">H", hh + 34)[0]
    hm = t["hmtx"][0]
    def adv(g):
        i = min(g, nhm - 1)
        return rd(f, ">Hh", hm + i * 4)
    keep = set([0])
    chars = {}
    for a, b in ranges:
        for c in range(a, b + 1):
            if c in cmap:
                chars[c] = cmap[c]
                glyph_deps(f, glyf, loca, cmap[c], keep)
    order = sorted(keep)
    newgid = {g: i for i, g in enumerate(order)}
    # glyf + loca
    gdata = bytearray(); nloca = [0]
    for g in order:
        a, b = loca[g], loca[g + 1]
        d = f[glyf + a:glyf + b]
        if b - a >= 10 and rd(f, ">h", glyf + a)[0] < 0:
            d = remap_composite(f, glyf, a, b, newgid)
        gdata += d
        while len(gdata) % 4: gdata += b"\0"
        nloca.append(len(gdata))
    locab = b"".join(struct.pack(">I", x) for x in nloca)
    headb = bytearray(head); struct.pack_into(">h", headb, 50, 1)
    maxpb = bytearray(f[t["maxp"][0]:t["maxp"][0] + t["maxp"][1]]); struct.pack_into(">H", maxpb, 4, len(order))
    hheab = bytearray(f[hh:hh + t["hhea"][1]]); struct.pack_into(">H", hheab, 34, len(order))
    hmtxb = b"".join(struct.pack(">Hh", *adv(g)) for g in order)
    # cmap format 4 with one segment per char (simple, slightly larger)
    segs = []
    for c in sorted(chars):
        g = newgid[chars[c]]
        if segs and segs[-1][1] == c - 1 and segs[-1][2] + (c - segs[-1][0]) == g:
            segs[-1][1] = c
        else:
            segs.append([c, c, g])
    segs.append([0xFFFF, 0xFFFF, 0])
    n = len(segs)
    sub = struct.pack(">HHHHHHH", 4, 16 + n * 8, 0, n * 2, 0, 0, 0)
    sub += b"".join(struct.pack(">H", s[1]) for s in segs) + b"\0\0"
    sub += b"".join(struct.pack(">H", s[0]) for s in segs)
    sub += b"".join(struct.pack(">H", (s[2] - s[0]) & 0xFFFF if s[0] != 0xFFFF else 1) for s in segs)
    sub += b"\0\0" * n
    cmapb = struct.pack(">HHHHI", 0, 1, 3, 1, 12) + sub
    tabs = [("cmap", cmapb), ("glyf", bytes(gdata)), ("head", bytes(headb)), ("hhea", bytes(hheab)),
            ("hmtx", hmtxb), ("loca", locab), ("maxp", bytes(maxpb))]
    out = bytearray(struct.pack(">IHHHH", 0x10000, len(tabs), 64, 2, len(tabs) * 16 - 64))
    off = 12 + len(tabs) * 16
    body = bytearray()
    for tag, d in tabs:
        out += struct.pack(">4sIII", tag.encode(), 0, off + len(body), len(d))
        body += d
        while len(body) % 4: body += b"\0"
    return bytes(out + body), len(order), len(chars)

def carray(name, data):
    lines = ["\t" + ", ".join("0x%02x" % b for b in data[i:i + 16]) + "," for i in range(0, len(data), 16)]
    return "const unsigned char %s[] = {\n%s\n};\nconst unsigned int %s_len = %u;\n" % (name, "\n".join(lines), name, len(data))

FONTS = [
    ("sans", "/usr/share/fonts/liberation-sans-fonts/LiberationSans-Regular.ttf", RANGES),
    ("sans_bold", "/usr/share/fonts/liberation-sans-fonts/LiberationSans-Bold.ttf", RANGES),
    ("sans_italic", "/usr/share/fonts/liberation-sans-fonts/LiberationSans-Italic.ttf", RANGES),
    ("sans_bolditalic", "/usr/share/fonts/liberation-sans-fonts/LiberationSans-BoldItalic.ttf", RANGES),
    ("serif", "/usr/share/fonts/liberation-serif-fonts/LiberationSerif-Regular.ttf", RANGES),
    ("serif_bold", "/usr/share/fonts/liberation-serif-fonts/LiberationSerif-Bold.ttf", RANGES),
    ("serif_italic", "/usr/share/fonts/liberation-serif-fonts/LiberationSerif-Italic.ttf", RANGES),
    ("serif_bolditalic", "/usr/share/fonts/liberation-serif-fonts/LiberationSerif-BoldItalic.ttf", RANGES),
    ("mono", "/usr/share/fonts/liberation-mono-fonts/LiberationMono-Regular.ttf", RANGES),
    ("mono_bold", "/usr/share/fonts/liberation-mono-fonts/LiberationMono-Bold.ttf", RANGES),
    ("symbol", "/usr/share/fonts/dejavu-sans-fonts/DejaVuSans.ttf", SYMBOL_RANGES),
]

if __name__ == "__main__":
    outdir = sys.argv[1]
    total = 0
    for name, path, ranges in FONTS:
        data, ng, nc = subset(path, ranges)
        total += len(data)
        with open(os.path.join(outdir, "font_%s.c" % name), "w") as o:
            o.write("/* Generated by tools/mkfont/mkfont.py from %s (SIL Open Font License / Bitstream Vera license). */\n" % os.path.basename(path))
            o.write(carray("zpdf_font_%s" % name, data))
        print("%-18s %6d bytes, %4d glyphs, %4d chars" % (name, len(data), ng, nc))
    print("total %d bytes" % total)
