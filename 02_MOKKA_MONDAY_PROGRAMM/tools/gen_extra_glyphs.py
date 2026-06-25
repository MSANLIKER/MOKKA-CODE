#!/usr/bin/env python3
# MOKKA – fehlende Glyphen in MokkaAmsiFonts.h ergaenzen (nur anhaengen, nichts aendern).
# Rendert aus den echten AMSI-Pro-OTFs im exakt gleichen MokkaGlyph-Format.
#   P (Pixelgroesse) = font.size  -> Hoehe/Scale matcht die vorhandenen Glyphen
#   yOffset = K - FT.bitmap_top, K aus vorhandenem 'A' bestimmt (Baseline-konstant)
# Aufruf:  python3 tools/gen_extra_glyphs.py
import re, sys, freetype

HEADER = "MokkaAmsiFonts.h"
OTF = "/Users/imac2021/Dropbox/MOKKA/WERBUNG/ALWAYS LINKS/MOKKA SCHRIFTEN/Amsi.Pro/OTF/"

# Font -> (OTF-Datei, size).  AmsiUltraPrice (nur Ziffern) wird bewusst NICHT ergaenzt.
FONTS = {
    "AmsiUltraBig": (OTF + "AmsiPro-Ultra.otf",     230),
    "AmsiCond120":  (OTF + "AmsiProCond-Ultra.otf", 120),
    "AmsiCond88":   (OTF + "AmsiProCond-Ultra.otf",  88),
    "AmsiCond60":   (OTF + "AmsiProCond-Ultra.otf",  60),
}

# Zu ergaenzende Zeichen (MOKKA-CI): Raute, Prozent, Grad, Guillemets, typogr. Anfuehrungen
NEW_CPS = [0x23, 0x25, 0xB0, 0xAB, 0xBB, 0x2018, 0x2019, 0x201C, 0x201D, 0x201E]

src = open(HEADER, encoding="utf-8", errors="replace").read()

def glyph_block(name):
    s = src.index(name + "Glyphs[")
    e = src.index("};", s)
    return s, e

def existing_cps_and_meta(name):
    s, e = glyph_block(name)
    blk = src[s:e]
    cps = set(int(m) for m in re.findall(r"\{(\d+),", blk))
    mA = re.search(r"\{65,\s*(\d+),\s*(\d+),\s*(\d+),\s*(-?\d+),\s*(-?\d+),\s*(\d+)\}", blk)
    aYoff = int(mA.group(5))
    # genutzte Bitmap-Laenge = max(offset + height*byteWidth)
    total = 0
    for o, w, h in re.findall(r"\{\d+,\s*(\d+),\s*(\d+),\s*(\d+),", blk):
        o, w, h = int(o), int(w), int(h)
        total = max(total, o + h * ((w + 7) // 8))
    return cps, aYoff, total

def pack_mono(g):
    bm = g.bitmap
    w, h, pitch, buf = bm.width, bm.rows, bm.pitch, bm.buffer
    bw = (w + 7) // 8
    out = []
    for r in range(h):
        base = r * pitch
        for c in range(bw):
            out.append(buf[base + c])
    return out, w, h

new_glyph_lines = {}   # name -> list of "{...}, // c"
new_byte_lists  = {}   # name -> list of int bytes
add_counts      = {}

for name, (otf, size) in FONTS.items():
    cps, aYoff, total = existing_cps_and_meta(name)
    face = freetype.Face(otf)
    face.set_pixel_sizes(0, size)
    # K aus 'A'
    face.load_char('A', freetype.FT_LOAD_RENDER | freetype.FT_LOAD_TARGET_MONO)
    K = aYoff + face.glyph.bitmap_top

    glines, gbytes = [], []
    offset = total
    for cp in NEW_CPS:
        if cp in cps:
            continue
        if face.get_char_index(cp) == 0:
            print(f"  [{name}] U+{cp:04X} nicht in OTF -> uebersprungen")
            continue
        face.load_char(chr(cp), freetype.FT_LOAD_RENDER | freetype.FT_LOAD_TARGET_MONO)
        g = face.glyph
        data, w, h = pack_mono(g)
        if w == 0 or h == 0:
            print(f"  [{name}] U+{cp:04X} leer -> uebersprungen")
            continue
        xo = g.bitmap_left
        yo = K - g.bitmap_top
        xa = round(g.advance.x / 64.0)
        glines.append("  {%d, %d, %d, %d, %d, %d, %d}, // U+%04X" % (cp, offset, w, h, xo, yo, xa, cp))
        gbytes.extend(data)
        offset += len(data)
    new_glyph_lines[name] = glines
    new_byte_lists[name]  = gbytes
    add_counts[name] = len(glines)
    print(f"{name}: +{len(glines)} Glyphen (K={K}, P={size}), +{len(gbytes)} Bytes")

# --- in den Header einfuegen (von hinten nach vorne, damit Indizes stabil bleiben) ---
def insert_before_array_end(text, decl, payload):
    s = text.index(decl)
    e = text.index("};", s)
    return text[:e] + payload + text[e:]

# Reihenfolge: spaeter im File zuerst bearbeiten
order = ["AmsiCond60", "AmsiCond88", "AmsiCond120", "AmsiUltraBig"]
out = src
for name in order:
    n = add_counts.get(name, 0)
    if n == 0:
        continue
    # Glyphen-Eintraege
    out = insert_before_array_end(out, name + "Glyphs[",
                                  "".join("\n" + l for l in new_glyph_lines[name]) + "\n")
    # Bitmap-Bytes
    byts = ",".join(str(b) for b in new_byte_lists[name])
    out = insert_before_array_end(out, name + "Bitmap[", "\n  " + byts + ",\n")
    # glyphCount erhoehen
    pat = re.compile(r"(const MokkaFont %s = \{%sBitmap, %sGlyphs, )(\d+)(, \d+\};)" %
                     (name, name, name))
    m = pat.search(out)
    old = int(m.group(2))
    out = pat.sub(lambda mm: mm.group(1) + str(old + n) + mm.group(3), out, count=1)
    print(f"{name}: glyphCount {old} -> {old + n}")

open(HEADER, "w", encoding="utf-8").write(out)
print("OK: MokkaAmsiFonts.h aktualisiert.")
