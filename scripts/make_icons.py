#!/usr/bin/env python3
"""Makes engine/icons.h: the pictures of the inventory (16x16 pixel art).

Every picture is drawn here from lines, boxes and ellipses - nothing of the
game is in it - in a small palette of roles rather than colours: the panel
colours "tint" pixels with the item's own colour (a red healing potion, a
blue one of mana), the rest is fixed (metal, wood, gold, paper). A dark
outline is put round every picture afterwards.

    python3 scripts/make_icons.py [preview.png]

With a path it also writes the pictures side by side, eight times as large.
"""
import sys
from PIL import Image, ImageDraw

# Roles of the pixels: the characters of the strings in icons.h.
ROLES = ".#xo+wmMbBgGrpPk"
T, OUT, TD, TM, TL, WH, ME, MD, WO, WD, GO, LE, RE, PA, PD, DK = range(16)
# Preview colours (the panel's are in panel.cpp); the tint shown is a dull red.
PREVIEW = [(0, 0, 0, 0), (26, 20, 16), (120, 40, 34), (190, 64, 52), (232, 140, 120), (245, 240, 225),
           (196, 200, 208), (112, 116, 128), (150, 98, 50), (92, 58, 28), (232, 199, 122), (96, 168, 72),
           (200, 60, 50), (232, 220, 190), (176, 160, 128), (40, 32, 26)]

icons = []  # (enum name, image)


def icon(name):
    def wrap(fn):
        im = Image.new("P", (16, 16), T)
        fn(ImageDraw.Draw(im), im)
        icons.append((name, im))
        return fn
    return wrap


def px(im, pts, c):
    for x, y in pts:
        im.putpixel((x, y), c)


def diag(d, x0, y0, n, c, w=1):
    # A line up and to the right, n pixels long, w thick (downwards).
    for i in range(n):
        for k in range(w):
            d.point((x0 + i, y0 - i + k), fill=c)


@icon("ICON_SWORD")
def _(d, im):
    diag(d, 4, 10, 10, ME, 2)          # blade
    diag(d, 5, 10, 9, WH)              # its edge catching the light
    d.line([(2, 8), (6, 12)], fill=GO) # guard
    d.line([(2, 13), (3, 12)], fill=WD)
    d.line([(1, 13), (2, 12)], fill=WO)
    px(im, [(1, 14), (0, 15)], GO)     # pommel


@icon("ICON_AXE")
def _(d, im):
    d.line([(3, 14), (11, 3)], fill=WO, width=2)
    d.polygon([(8, 1), (14, 2), (15, 7), (12, 9), (9, 5)], fill=ME)
    d.line([(14, 3), (15, 7)], fill=WH)
    d.line([(9, 4), (11, 7)], fill=MD)


@icon("ICON_CLUB")
def _(d, im):
    d.line([(2, 14), (9, 6)], fill=WO, width=2)
    d.ellipse([(8, 1), (14, 8)], fill=WO)
    d.ellipse([(9, 2), (12, 5)], fill=(TL if False else WO))
    px(im, [(10, 2), (12, 3), (13, 6), (9, 6), (11, 7)], MD)  # studs
    px(im, [(10, 3), (11, 3)], WD)


@icon("ICON_GREATSWORD")
def _(d, im):
    diag(d, 4, 11, 11, ME, 3)
    diag(d, 5, 11, 10, WH)
    d.line([(1, 8), (7, 14)], fill=GO, width=1)
    d.line([(2, 8), (7, 13)], fill=GO)
    px(im, [(2, 13), (1, 14), (0, 15)], WD)


@icon("ICON_BOW")
def _(d, im):
    d.arc([(2, 1), (14, 23)], 180, 270, fill=WO, width=2)
    d.arc([(-8, -8), (14, 14)], 0, 90, fill=WO, width=0)
    d.line([(3, 13), (13, 3)], fill=WH)
    d.arc([(1, 1), (27, 27)], 180, 270, fill=WO, width=2)


@icon("ICON_CROSSBOW")
def _(d, im):
    d.line([(2, 14), (11, 5)], fill=WO, width=2)    # stock
    d.line([(7, 1), (15, 9)], fill=MD, width=2)     # bow, across it
    d.line([(7, 3), (13, 9)], fill=WH)              # string
    px(im, [(1, 14), (2, 15)], WD)


@icon("ICON_ARROW")
def _(d, im):
    for off in (0, 3):
        d.line([(2 + off, 13), (12 + off, 3)], fill=WO)
        d.polygon([(12 + off, 3), (13 + off, 1), (14 + off, 2)], fill=ME) if off < 2 else None
    d.polygon([(11, 2), (14, 1), (13, 4)], fill=ME)
    d.polygon([(8, 5), (11, 4), (10, 7)], fill=ME)
    px(im, [(1, 13), (2, 14), (1, 14), (4, 13), (5, 14), (4, 14)], TM)  # fletching


@icon("ICON_ARMOR")
def _(d, im):
    d.polygon([(4, 1), (11, 1), (15, 4), (13, 7), (12, 6), (12, 14), (3, 14), (3, 6), (2, 7), (0, 4)], fill=TM)
    d.polygon([(6, 1), (9, 1), (8, 4), (7, 4)], fill=DK)        # neck
    d.line([(4, 8), (11, 8)], fill=TD)
    d.line([(4, 11), (11, 11)], fill=TD)
    d.line([(7, 5), (7, 13)], fill=TD)
    px(im, [(4, 2), (3, 3), (11, 2)], TL)


@icon("ICON_AMULET")
def _(d, im):
    d.arc([(3, 0), (12, 10)], 180, 360, fill=GO)
    d.line([(3, 5), (6, 9)], fill=GO)
    d.line([(12, 5), (9, 9)], fill=GO)
    d.ellipse([(4, 8), (11, 15)], fill=GO)
    d.ellipse([(6, 10), (9, 13)], fill=TM)
    px(im, [(7, 10)], TL)


@icon("ICON_RING")
def _(d, im):
    d.ellipse([(2, 5), (13, 15)], outline=GO, width=2)
    d.polygon([(6, 1), (9, 1), (11, 4), (7, 7), (4, 4)], fill=TM)
    px(im, [(6, 2), (7, 2)], TL)
    px(im, [(5, 13), (4, 12)], WH)


@icon("ICON_RUNE")
def _(d, im):
    d.ellipse([(1, 2), (14, 14)], fill=MD)
    d.ellipse([(2, 3), (13, 12)], fill=ME)
    d.line([(7, 5), (7, 11)], fill=TD)
    d.line([(7, 5), (10, 8)], fill=TD)
    d.line([(5, 9), (9, 9)], fill=TD)


@icon("ICON_SCROLL")
def _(d, im):
    d.rectangle([(3, 3), (12, 13)], fill=PA)
    d.rectangle([(2, 1), (13, 3)], fill=PD)
    d.rectangle([(2, 13), (13, 15)], fill=PD)
    for y in (6, 8, 10):
        d.line([(5, y), (10, y)], fill=TM)
    px(im, [(2, 2), (13, 14)], WH)


@icon("ICON_POTION")
def _(d, im):
    d.rectangle([(6, 0), (9, 1)], fill=WO)        # cork
    d.rectangle([(6, 2), (9, 5)], fill=PA)         # neck (glass)
    d.ellipse([(2, 5), (13, 15)], fill=TM)
    d.chord([(2, 5), (13, 15)], 200, 340, fill=PA)  # the empty top of the flask
    px(im, [(4, 9), (4, 10), (5, 8)], TL)
    px(im, [(5, 12), (6, 13)], TD)


@icon("ICON_APPLE")
def _(d, im):
    d.ellipse([(2, 4), (14, 15)], fill=TM)
    d.line([(8, 1), (8, 5)], fill=WD)
    d.polygon([(9, 2), (13, 0), (12, 3)], fill=LE)
    px(im, [(5, 7), (4, 8), (5, 8)], TL)
    px(im, [(8, 5)], TD)


@icon("ICON_MEAT")
def _(d, im):
    d.ellipse([(4, 1), (15, 11)], fill=TM)
    d.ellipse([(6, 3), (12, 8)], fill=TL)
    d.line([(1, 14), (6, 9)], fill=PA, width=2)    # the bone
    px(im, [(0, 13), (1, 15), (0, 14)], PA)
    px(im, [(13, 4), (12, 9)], TD)


@icon("ICON_BREAD")
def _(d, im):
    d.ellipse([(1, 4), (15, 14)], fill=WO)
    d.ellipse([(2, 4), (14, 11)], fill=GO)
    for x in (5, 8, 11):
        d.line([(x, 6), (x + 1, 8)], fill=WO)


@icon("ICON_BOTTLE")
def _(d, im):
    d.rectangle([(7, 0), (8, 1)], fill=WO)
    d.rectangle([(6, 2), (9, 5)], fill=TD)
    d.rounded_rectangle([(4, 5), (11, 15)], radius=2, fill=TD)
    d.rectangle([(5, 9), (10, 12)], fill=PA)      # label
    px(im, [(5, 6), (5, 7)], TL)


@icon("ICON_PLANT")
def _(d, im):
    d.line([(7, 15), (7, 6)], fill=LE)
    d.ellipse([(1, 5), (7, 10)], fill=LE)
    d.ellipse([(8, 3), (14, 8)], fill=LE)
    d.ellipse([(4, 0), (10, 5)], fill=TM)          # the bloom
    px(im, [(7, 2)], TL)
    px(im, [(3, 7), (10, 5)], WH)


@icon("ICON_BOWL")
def _(d, im):
    d.chord([(1, 2), (14, 15)], 0, 180, fill=WO)
    d.ellipse([(1, 6), (14, 11)], fill=TM)
    d.ellipse([(3, 7), (8, 9)], fill=TL)
    d.line([(10, 1), (13, 8)], fill=PA)            # a spoon


@icon("ICON_LETTER")
def _(d, im):
    d.rectangle([(1, 3), (14, 13)], fill=PA)
    d.line([(1, 3), (7, 8)], fill=PD)
    d.line([(14, 3), (8, 8)], fill=PD)
    d.ellipse([(6, 7), (9, 10)], fill=RE)          # the seal


@icon("ICON_MAP")
def _(d, im):
    d.polygon([(1, 2), (5, 1), (10, 3), (14, 1), (14, 13), (10, 15), (5, 13), (1, 15)], fill=PA)
    d.line([(5, 1), (5, 13)], fill=PD)
    d.line([(10, 3), (10, 15)], fill=PD)
    d.line([(3, 11), (7, 7), (12, 9)], fill=RE)
    px(im, [(12, 5), (11, 4), (13, 6), (13, 4), (11, 6)], RE)


@icon("ICON_BOOK")
def _(d, im):
    d.rectangle([(2, 1), (13, 14)], fill=TD)
    d.rectangle([(4, 2), (13, 13)], fill=TM)
    d.rectangle([(3, 13), (13, 15)], fill=PA)
    d.rectangle([(6, 4), (11, 6)], fill=GO)


@icon("ICON_TORCH")
def _(d, im):
    d.line([(4, 15), (9, 6)], fill=WO, width=2)
    d.polygon([(8, 7), (12, 7), (13, 3), (11, 0), (10, 3), (8, 1), (7, 4)], fill=GO)
    px(im, [(10, 5), (10, 4), (11, 5)], WH)
    d.line([(8, 7), (11, 7)], fill=WD)


@icon("ICON_KEY")
def _(d, im):
    d.ellipse([(1, 1), (7, 7)], outline=GO, width=2)
    d.line([(6, 6), (14, 14)], fill=GO, width=2)
    d.line([(11, 13), (13, 11)], fill=GO)
    px(im, [(10, 14), (13, 9)], GO)


@icon("ICON_ORE")
def _(d, im):
    d.polygon([(2, 9), (5, 4), (10, 3), (14, 7), (13, 13), (7, 15), (3, 13)], fill=TM)
    d.polygon([(5, 5), (9, 4), (8, 8), (5, 8)], fill=TL)
    px(im, [(6, 6), (11, 6)], WH)
    d.line([(9, 9), (12, 12)], fill=TD)


@icon("ICON_TROPHY")
def _(d, im):
    d.polygon([(3, 15), (6, 3), (9, 0), (10, 4), (9, 15)], fill=PA)  # a tooth or a claw
    d.line([(9, 1), (8, 12)], fill=PD)
    d.polygon([(10, 15), (12, 8), (14, 6), (14, 10), (13, 15)], fill=PA)
    px(im, [(6, 5)], WH)


@icon("ICON_BAG")
def _(d, im):
    d.ellipse([(1, 5), (14, 15)], fill=WO)
    d.polygon([(5, 2), (10, 2), (9, 6), (6, 6)], fill=WO)
    d.line([(5, 6), (10, 6)], fill=WD)
    px(im, [(4, 8), (3, 9)], GO)


@icon("ICON_ALL")
def _(d, im):
    for x in (1, 9):
        for y in (1, 9):
            d.rectangle([(x, y), (x + 5, y + 5)], fill=TM)
            d.point((x + 1, y + 1), fill=TL)


@icon("ICON_WEAPONS")
def _(d, im):
    d.line([(3, 12), (14, 1)], fill=TM, width=2)
    d.line([(1, 1), (12, 12)], fill=TM, width=2)
    d.line([(1, 10), (5, 14)], fill=TL)             # guards
    d.line([(10, 14), (14, 10)], fill=TL)
    px(im, [(1, 14), (0, 15), (14, 14), (15, 15)], TD)


@icon("ICON_MAGIC")
def _(d, im):
    d.polygon([(8, 0), (10, 6), (15, 8), (10, 10), (8, 15), (6, 10), (1, 8), (6, 6)], fill=TM)
    d.ellipse([(6, 6), (10, 10)], fill=TL)


@icon("ICON_OTHER")
def _(d, im):
    d.ellipse([(1, 5), (14, 15)], fill=TM)
    d.polygon([(5, 2), (10, 2), (9, 6), (6, 6)], fill=TM)
    d.line([(5, 6), (10, 6)], fill=TD)
    px(im, [(4, 8), (3, 9)], TL)


def outlined(im):
    out = im.copy()
    for y in range(16):
        for x in range(16):
            if im.getpixel((x, y)) != T:
                continue
            for dx, dy in ((1, 0), (-1, 0), (0, 1), (0, -1)):
                nx, ny = x + dx, y + dy
                if 0 <= nx < 16 and 0 <= ny < 16 and im.getpixel((nx, ny)) not in (T, OUT):
                    out.putpixel((x, y), OUT)
                    break
    return out


rows = []
for name, im in icons:
    o = outlined(im)
    lines = ["".join(ROLES[o.getpixel((x, y))] for x in range(16)) for y in range(16)]
    rows.append((name, lines, o))

with open("engine/icons.h", "w") as f:
    f.write("// Generated by scripts/make_icons.py. Do not edit.\n")
    f.write("// The inventory's pictures, 16x16; the characters are roles of pixels:\n")
    f.write("//   .  nothing     #  outline     x o +  the item's colour: dark, itself, light\n")
    f.write("//   w  highlight   m M  metal, light and dark    b B  wood, light and dark\n")
    f.write("//   g  gold        G  leaf        r  red         p P  paper, light and dark   k  shadow\n")
    f.write("// In the order of ItemIcon (inventory.h).\n")
    f.write("#pragma once\n\nnamespace g3 {\nnamespace icons {\n\n")
    f.write("constexpr int SIZE = 16;\nconstexpr int COUNT = %d;\n" % len(rows))
    f.write("static const char* const kRoles = \"%s\";\n" % ROLES)
    f.write("static const char kPixels[COUNT][SIZE][SIZE + 1] = {\n")
    for name, lines, _ in rows:
        f.write("    // %s\n    {" % name)
        f.write(",\n     ".join('"%s"' % l for l in lines))
        f.write("},\n")
    f.write("};\n\n} // namespace icons\n} // namespace g3\n")

if len(sys.argv) > 1:
    scale, per = 6, 8
    lines_n = (len(rows) + per - 1) // per
    sheet = Image.new("RGBA", (per * 18 * scale, lines_n * 18 * scale), (34, 28, 22, 255))
    draw = ImageDraw.Draw(sheet)
    for i, (_, _, o) in enumerate(rows):
        for y in range(16):
            for x in range(16):
                c = o.getpixel((x, y))
                if c == T:
                    continue
                col = PREVIEW[c] + ((255,) if len(PREVIEW[c]) == 3 else ())
                x0 = ((i % per) * 18 + x) * scale
                y0 = ((i // per) * 18 + y) * scale
                draw.rectangle([(x0, y0), (x0 + scale - 1, y0 + scale - 1)], fill=col)
    sheet.save(sys.argv[1])
print("icons: %d" % len(rows))
