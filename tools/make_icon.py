"""Draws SteelTemple Sanctum's icon: assets/sanctum.ico (16 to 256 px) and assets/icon.png (the window's, 256 px).

What: a steel tile with rounded corners; on it a temple front in brushed steel (a roof, a frieze, four columns, a
step), and the sanctum between the middle columns lit in the selection cyan of Sanctum's menus, with the Dark
theme's red as a thin line under the step. Shapes and colours only, drawn at 1024 px and scaled down per size; the
small sizes (16, 24, 32) drop the frieze and use two columns so the temple still reads.
How: `python tools/make_icon.py` (Pillow).
"""

from pathlib import Path

from PIL import Image, ImageDraw, ImageFilter

ROOT = Path(__file__).resolve().parents[1]
S = 1024


def vgrad(w: int, h: int, top: tuple, bottom: tuple) -> Image.Image:
    g = Image.new("RGBA", (w, h))
    px = g.load()
    for y in range(h):
        t = y / max(1, h - 1)
        c = tuple(int(top[i] + (bottom[i] - top[i]) * t) for i in range(4))
        for x in range(w):
            px[x, y] = c
    return g


def mask_of(draw_fn) -> Image.Image:
    m = Image.new("L", (S, S), 0)
    draw_fn(ImageDraw.Draw(m))
    return m


def icon(simple: bool) -> Image.Image:
    img = Image.new("RGBA", (S, S), (0, 0, 0, 0))
    pad, rad = 40, 210
    # the tile: dark steel, lighter at the top, a soft edge light
    tile = mask_of(lambda d: d.rounded_rectangle((pad, pad, S - pad, S - pad), rad, fill=255))
    img.paste(vgrad(S, S, (52, 56, 68, 255), (16, 17, 22, 255)), (0, 0), tile)
    rim = mask_of(lambda d: d.rounded_rectangle((pad, pad, S - pad, S - pad), rad, outline=255, width=10))
    img.paste(vgrad(S, S, (120, 128, 146, 255), (40, 42, 50, 255)), (0, 0), rim)

    # the temple's geometry
    cx = S // 2
    roof_top, roof_base = 230, 400
    left, right = 190, S - 190
    frieze = (left + 20, roof_base, right - 20, roof_base + (0 if simple else 46))
    col_top = frieze[3] + (0 if simple else 14)
    col_bot = 740
    step = (left - 10, col_bot, right + 10, col_bot + 56)
    # columns in two pairs, leaving a wide doorway in the middle: the sanctum
    col_w = 118 if simple else 70
    span_l, span_r = left + 36, right - 36
    gap = 200 if simple else 190   # the doorway's width
    if simple:
        xs = [cx - gap / 2 - col_w, cx + gap / 2]
    else:
        pair = (span_r - span_l - gap) / 2   # each pair's width
        xs = [span_l, span_l + pair - col_w, cx + gap / 2, cx + gap / 2 + pair - col_w]
    ncols = len(xs)

    # the sanctum's light: an arched doorway lit cyan, glowing out over the stone
    door_l, door_r = cx - gap / 2 + 22, cx + gap / 2 - 22
    arch = (door_r - door_l) / 2

    def door(d: ImageDraw.ImageDraw, grow: float = 0) -> None:
        top = col_top + 26 - grow
        d.rectangle((door_l - grow, top + arch, door_r + grow, col_bot + grow), fill=255)
        d.ellipse((door_l - grow, top, door_r + grow, top + 2 * arch), fill=255)

    glow = Image.new("RGBA", (S, S), (63, 224, 255, 0))
    glow.putalpha(mask_of(lambda d: door(d, 40)).point(lambda v: v * 0.85))
    img.alpha_composite(glow.filter(ImageFilter.GaussianBlur(60)))
    img.paste(vgrad(S, S, (190, 248, 255, 255), (16, 140, 200, 255)), (0, 0), mask_of(door))
    # a brighter core, low in the doorway, as if lit from within
    core = Image.new("RGBA", (S, S), (235, 253, 255, 0))
    core.putalpha(mask_of(lambda d: d.ellipse((cx - arch * 0.55, col_bot - 190, cx + arch * 0.55, col_bot - 10), fill=200)))
    img.alpha_composite(core.filter(ImageFilter.GaussianBlur(40)))

    # brushed steel for the stone: roof, frieze, columns, step
    steel = vgrad(S, S, (244, 246, 250, 255), (150, 158, 174, 255))

    def stone(d: ImageDraw.ImageDraw) -> None:
        d.polygon([(left - 20, roof_base), (cx, roof_top), (right + 20, roof_base)], fill=255)
        if not simple:
            d.rectangle(frieze, fill=255)
        for x in xs:
            d.rounded_rectangle((x, col_top, x + col_w, col_bot), 10, fill=255)
        d.rounded_rectangle(step, 14, fill=255)

    shadow = mask_of(stone).filter(ImageFilter.GaussianBlur(18))
    img.paste((0, 0, 0, 150), (0, 26), shadow)
    img.paste(steel, (0, 0), mask_of(stone))
    if not simple:   # the roof's inner line, and the columns' flutes
        line = mask_of(lambda d: d.line([(left + 60, roof_base - 22), (cx, roof_top + 54), (right - 60, roof_base - 22)],
                                         fill=255, width=12))
        img.paste((110, 118, 136, 255), (0, 0), line)
        flutes = mask_of(lambda d: [d.line([(x + col_w / 2, col_top + 24), (x + col_w / 2, col_bot - 24)], fill=255,
                                            width=8) for x in xs])
        img.paste((168, 176, 192, 255), (0, 0), flutes)
    # the red line under the step (the Dark theme's accent)
    img.paste((232, 36, 48, 255), (0, 0),
              mask_of(lambda d: d.rounded_rectangle((step[0] + 40, step[3] + 30, step[2] - 40, step[3] + 52), 11, fill=255)))
    return img


def main() -> None:
    full, simple = icon(False), icon(True)
    sizes = [256, 128, 64, 48, 32, 24, 16]
    frames = [(simple if s <= 32 else full).resize((s, s), Image.LANCZOS) for s in sizes]
    out = ROOT / "assets"
    out.mkdir(exist_ok=True)
    frames[0].save(out / "sanctum.ico", sizes=[(s, s) for s in sizes], append_images=frames[1:])
    frames[0].save(out / "icon.png")
    print(out / "sanctum.ico", out / "icon.png")


if __name__ == "__main__":
    main()
