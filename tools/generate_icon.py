#!/usr/bin/env python3
"""
Draw Gateway's Finder icon and emit it as Rez source.

Classic Mac icons are hand-placed pixels, not scaled art: at 32x32 every pixel
carries weight, so the shapes are drawn here in code rather than exported from
a drawing program and resampled.

The design is a stone gateway arch with a padlock in the opening -- a gateway
you pass through, secured. Platinum greys and a blue depth inside the arch put
it next to the Mac OS 9 system icons rather than against them.

    python3 tools/generate_icon.py --out src/ui/gateway_icon.r [--preview p.png]

Colours are restricted to the 6x6x6 cube of the Macintosh 256-colour system
palette (components from 0, 51, 102, 153, 204, 255), where the palette index is
simply r*36 + g*6 + b on the descending 0..5 scale. That makes the mapping
exact without needing the whole system colour table; pure black is the one
exception, living at index 255.
"""

import argparse

# --- Macintosh 8-bit system palette, cube subset -----------------------------

LEVELS = [255, 204, 153, 102, 51, 0]


def mac8(rgb):
    """Palette index for a colour drawn from the cube."""
    if rgb == (0, 0, 0):
        return 255                      # black is not at cube index 215
    try:
        r, g, b = (LEVELS.index(c) for c in rgb)
    except ValueError:
        raise SystemExit("colour %r is not on the 51-step cube" % (rgb,))
    return r * 36 + g * 6 + b


# --- Palette ----------------------------------------------------------------

CLEAR   = None
BLACK   = (0, 0, 0)
WHITE   = (255, 255, 255)
LIGHT   = (204, 204, 204)           # platinum face
MID     = (153, 153, 153)           # platinum shadow
DARK    = (102, 102, 102)           # deep shadow
SKY_1   = (0, 0, 153)               # top of the opening
SKY_2   = (0, 51, 153)
SKY_3   = (0, 102, 204)
SKY_4   = (51, 153, 255)            # horizon
GOLD    = (255, 204, 0)
GOLD_D  = (204, 153, 0)
SILVER  = (204, 204, 204)


def arch(x, y, cx, cy, rx, bottom):
    """True inside an arch: a half circle on top of a rectangle."""
    if y > bottom:
        return False
    if y >= cy:
        return abs(x - cx) <= rx
    dx = x - cx
    dy = y - cy
    return dx * dx + dy * dy <= rx * rx


def draw32():
    px = [[CLEAR] * 32 for _ in range(32)]

    CX, CY = 15.5, 16.0
    OUTER, INNER = 12.5, 7.5
    BOTTOM = 27

    # The arch body, shaded so the light falls from the top left.
    for y in range(32):
        for x in range(32):
            if not arch(x, y, CX, CY, OUTER, BOTTOM):
                continue
            if arch(x, y, CX, CY, INNER, BOTTOM + 1):
                continue                      # the opening, filled later
            edge_out = not arch(x, y, CX, CY, OUTER - 1.0, BOTTOM)
            edge_in = arch(x, y, CX, CY, INNER + 1.0, BOTTOM + 1)
            if edge_out or edge_in:
                px[y][x] = BLACK
            elif x < CX - 2 and y > 10:
                px[y][x] = WHITE if x < CX - 8 else LIGHT
            elif x > CX + 6:
                px[y][x] = MID
            elif y < 10:
                px[y][x] = WHITE if x < CX else LIGHT
            else:
                px[y][x] = LIGHT

    # The view through the gateway: darker at the crown, brighter at the base.
    for y in range(32):
        for x in range(32):
            if not arch(x, y, CX, CY, INNER, BOTTOM + 1):
                continue
            if y < 12:
                px[y][x] = SKY_1
            elif y < 17:
                px[y][x] = SKY_2
            elif y < 22:
                px[y][x] = SKY_3
            else:
                px[y][x] = SKY_4

    # Plinth the arch stands on.
    for y in range(28, 31):
        for x in range(1, 31):
            if y == 28:
                px[y][x] = BLACK if x in (1, 30) else LIGHT
            elif y == 29:
                px[y][x] = BLACK if x in (1, 30) else MID
            else:
                px[y][x] = BLACK
    for x in range(1, 31):
        px[28][x] = BLACK if x in (1, 30) else px[28][x]

    # Padlock, kept small so the opening still reads as a passage: a shackle
    # arc, then the body drawn over its feet.
    for y in range(14, 20):
        for x in range(11, 21):
            dx, dy = x - 15.5, y - 19.0
            d = (dx * dx + dy * dy) ** 0.5
            if 2.2 <= d <= 3.4:
                px[y][x] = SILVER
            elif 1.4 <= d < 2.2 or 3.4 < d <= 4.2:
                px[y][x] = BLACK

    for y in range(19, 26):
        for x in range(11, 21):
            if x in (11, 20) or y in (19, 25):
                px[y][x] = BLACK
            else:
                px[y][x] = GOLD if y < 23 else GOLD_D

    # Keyhole: a round pin over a short slot.
    px[21][15] = BLACK
    px[21][16] = BLACK
    px[22][15] = BLACK
    px[22][16] = BLACK
    px[23][15] = BLACK
    px[23][16] = BLACK

    return px


def draw16():
    px = [[CLEAR] * 16 for _ in range(16)]

    CX, CY = 7.5, 8.0
    OUTER, INNER = 6.5, 3.5
    BOTTOM = 13

    for y in range(16):
        for x in range(16):
            if not arch(x, y, CX, CY, OUTER, BOTTOM):
                continue
            if arch(x, y, CX, CY, INNER, BOTTOM + 1):
                continue
            edge = (not arch(x, y, CX, CY, OUTER - 1.0, BOTTOM)) or \
                   arch(x, y, CX, CY, INNER + 1.0, BOTTOM + 1)
            if edge:
                px[y][x] = BLACK
            elif x < CX - 1:
                px[y][x] = WHITE
            else:
                px[y][x] = MID

    for y in range(16):
        for x in range(16):
            if arch(x, y, CX, CY, INNER, BOTTOM + 1):
                px[y][x] = SKY_2 if y < 9 else SKY_4

    for x in range(1, 15):
        px[14][x] = LIGHT if x not in (1, 14) else BLACK
        px[15][x] = BLACK

    # At 16x16 the padlock is barely more than an outline and a fill.
    for y in range(9, 14):
        for x in range(5, 11):
            if x in (5, 10) or y in (9, 13):
                px[y][x] = BLACK
            else:
                px[y][x] = GOLD
    px[8][6] = BLACK
    px[8][9] = BLACK
    px[7][7] = BLACK
    px[7][8] = BLACK
    px[11][7] = BLACK
    px[11][8] = BLACK

    return px


# --- Rez emission -----------------------------------------------------------

def hexrows(data, per=16, indent="\t"):
    rows = []
    for i in range(0, len(data), per):
        rows.append(indent + '$"' +
                    " ".join("%02X" % b for b in data[i:i + per]) + '"')
    return "\n".join(rows)


def bitmap(px, size, predicate):
    """Pack a 1-bit-per-pixel bitmap, MSB first."""
    out = bytearray()
    for y in range(size):
        for byte in range(size // 8):
            v = 0
            for bit in range(8):
                x = byte * 8 + bit
                if predicate(px[y][x]):
                    v |= 0x80 >> bit
            out.append(v)
    return bytes(out)


def luminance(c):
    return 0.299 * c[0] + 0.587 * c[1] + 0.114 * c[2]


def emit(px32, px16):
    # 1-bit icon: ink where the pixel is dark. Mask: every non-clear pixel.
    icn = bitmap(px32, 32, lambda c: c is not CLEAR and luminance(c) < 150)
    icn_mask = bitmap(px32, 32, lambda c: c is not CLEAR)
    ics = bitmap(px16, 16, lambda c: c is not CLEAR and luminance(c) < 150)
    ics_mask = bitmap(px16, 16, lambda c: c is not CLEAR)

    icl8 = bytes(mac8(c) if c is not CLEAR else 0
                 for row in px32 for c in row)
    ics8 = bytes(mac8(c) if c is not CLEAR else 0
                 for row in px16 for c in row)

    parts = []
    parts.append("""/*
 * gateway_icon.r - Finder icon for Gateway.
 *
 * DO NOT EDIT. Regenerate with:
 *   python3 tools/generate_icon.py --out src/ui/gateway_icon.r
 *
 * A stone gateway arch with a padlock in the opening: something you pass
 * through, secured. Written as raw data blocks for the same reason as
 * gateway.r -- Rez runs against whichever RIncludes the toolchain has linked,
 * and a resource file with no includes does not care which.
 *
 * BNDL and FREF tie the icon family to the 'GT9A' creator so the Finder uses
 * it. If a freshly built copy shows a generic application icon, the desktop
 * database has not caught up: rebuild it by holding Command-Option through
 * startup, or move the application to another folder and back.
 */
""")
    parts.append('data \'ICN#\' (128, "Gateway", purgeable) {\n%s\n};\n'
                 % hexrows(icn + icn_mask))
    parts.append('data \'icl8\' (128, "Gateway", purgeable) {\n%s\n};\n'
                 % hexrows(icl8))
    parts.append('data \'ics#\' (128, "Gateway", purgeable) {\n%s\n};\n'
                 % hexrows(ics + ics_mask))
    parts.append('data \'ics8\' (128, "Gateway", purgeable) {\n%s\n};\n'
                 % hexrows(ics8))

    # FREF: application, icon local ID 0, no name.
    parts.append('data \'FREF\' (128, "Gateway", purgeable) {\n'
                 '\t$"4150 504C"        /* type APPL          */\n'
                 '\t$"0000"             /* local icon list ID */\n'
                 '\t$"00"               /* empty name         */\n'
                 '};\n')

    # BNDL: creator GT9A, mapping local ID 0 to resource 128 for both lists.
    parts.append('data \'BNDL\' (128, "Gateway", purgeable) {\n'
                 '\t$"4754 3941"        /* signature GT9A     */\n'
                 '\t$"0000"             /* version            */\n'
                 '\t$"0001"             /* two type entries   */\n'
                 '\t$"4943 4E23"        /* ICN#               */\n'
                 '\t$"0000"             /* one mapping        */\n'
                 '\t$"0000 0080"        /* local 0 -> 128     */\n'
                 '\t$"4652 4546"        /* FREF               */\n'
                 '\t$"0000"             /* one mapping        */\n'
                 '\t$"0000 0080"        /* local 0 -> 128     */\n'
                 '};\n')

    # The signature resource the bundle points back at.
    parts.append('data \'GT9A\' (0, "Gateway", purgeable) {\n'
                 '\t$"0B" "Gateway 0.1"\n'
                 '};\n')

    return "\n".join(parts)


def preview(px32, px16, path):
    from PIL import Image
    img = Image.new("RGBA", (32 + 4 + 16, 32), (0, 0, 0, 0))
    for y in range(32):
        for x in range(32):
            c = px32[y][x]
            img.putpixel((x, y), (0, 0, 0, 0) if c is CLEAR else c + (255,))
    for y in range(16):
        for x in range(16):
            c = px16[y][x]
            img.putpixel((36 + x, y), (0, 0, 0, 0) if c is CLEAR else c + (255,))
    img.resize((img.width * 8, img.height * 8), Image.NEAREST).save(path)


def ascii_art(px):
    ramp = " .:-=+*#%@"
    for row in px:
        line = ""
        for c in row:
            line += " " if c is CLEAR else ramp[min(9, int((255 - luminance(c)) / 26))]
        print(line)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--out", required=True)
    ap.add_argument("--preview")
    ap.add_argument("--ascii", action="store_true")
    args = ap.parse_args()

    px32, px16 = draw32(), draw16()
    open(args.out, "w").write(emit(px32, px16))
    print("wrote", args.out)
    if args.ascii:
        ascii_art(px32)
    if args.preview:
        preview(px32, px16, args.preview)
        print("preview", args.preview)


if __name__ == "__main__":
    main()
