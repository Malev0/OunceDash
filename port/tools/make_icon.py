# Draws OunceDash's hbmenu icon (256x256 JPEG): original artwork, no game assets.
# usage: python tools/make_icon.py OUT.jpg [style]
#   default: chev_steel4 (the icon in use: four red chevrons on steel)
#   others: chevrons, chev_<palette>, jump, pulse, monogram, drop, runner, trail, logo, orb
import math
import sys
from PIL import Image, ImageDraw, ImageFilter, ImageFont

S = 1024  # drawn at 4x, downsampled for smooth edges


def font(size):
    for f in ("C:/Windows/Fonts/segoeuib.ttf", "C:/Windows/Fonts/arialbd.ttf",
              "/usr/share/fonts/truetype/dejavu/DejaVuSans-Bold.ttf"):
        try:
            return ImageFont.truetype(f, size)
        except OSError:
            pass
    return ImageFont.load_default()


def gradient(c0, c1, diag=True):
    img = Image.new("RGB", (S, S))
    px = img.load()
    for y in range(S):
        for x in range(S):
            t = (x * 0.4 + y * 0.6) / S if diag else y / S
            px[x, y] = tuple(int(a + (b - a) * t) for a, b in zip(c0, c1))
    return img.convert("RGBA")


def glow_layer(draw_fn, blur=28):
    glow = Image.new("RGBA", (S, S), (0, 0, 0, 0))
    art = Image.new("RGBA", (S, S), (0, 0, 0, 0))
    draw_fn(ImageDraw.Draw(glow))
    draw_fn(ImageDraw.Draw(art))
    return glow.filter(ImageFilter.GaussianBlur(blur)), art


def compose(bg, draw_fn, blur=28):
    glow, art = glow_layer(draw_fn, blur)
    return Image.alpha_composite(Image.alpha_composite(bg, glow), art)


def title(img, y=760, size=168, color=(245, 250, 255, 255)):
    d = ImageDraw.Draw(img)
    f = font(size)
    text = "OunceDash"
    tw = d.textlength(text, font=f)
    d.text(((S - tw) / 2 + 6, y + 6), text, font=f, fill=(0, 0, 0, 140))
    d.text(((S - tw) / 2, y), text, font=f, fill=color)
    return img


def chevron(d, cx, cy, w, h, t, fill):
    pts = [(cx - w / 2, cy - h / 2), (cx - w / 2 + t, cy - h / 2), (cx + w / 2, cy),
           (cx - w / 2 + t, cy + h / 2), (cx - w / 2, cy + h / 2), (cx + w / 2 - t, cy)]
    d.polygon(pts, fill=fill)


# ---------------------------------------------------------------- styles

def style_chevrons():
    bg = gradient((28, 16, 66), (32, 76, 106))
    def draw(d):
        for i, c in enumerate([(90, 230, 255), (120, 255, 200), (255, 240, 120)]):
            chevron(d, 330 + i * 190, 430, 230, 400, 92, c + (255,))
        for (y, x0, x1, w) in [(300, 60, 250, 18), (380, 20, 230, 14), (480, 40, 235, 14), (560, 80, 250, 18)]:
            d.rounded_rectangle((x0, y - w / 2, x1, y + w / 2), radius=w / 2, fill=(160, 242, 255, 220))
    return title(compose(bg, draw))


def style_jump():
    # a glowing ball arcing over a row of blocks, on a dusk gradient
    bg = gradient((20, 10, 48), (120, 40, 110), diag=False)
    def draw(d):
        # floor and blocks
        d.rectangle((0, 640, S, 660), fill=(255, 120, 200, 255))
        for x, h in [(120, 90), (300, 150), (560, 110), (800, 190)]:
            d.rounded_rectangle((x, 640 - h, x + 110, 640), radius=14, fill=(70, 30, 120, 255),
                                outline=(255, 140, 220, 255), width=10)
        # dotted arc
        for i in range(14):
            t = i / 13
            x = 140 + t * 620
            y = 560 - math.sin(t * math.pi) * 380
            r = 10 + 6 * t
            d.ellipse((x - r, y - r, x + r, y + r), fill=(255, 230, 140, int(90 + 160 * t)))
        # the ball
        d.ellipse((730, 360, 870, 500), fill=(255, 225, 90, 255))
        d.ellipse((760, 385, 805, 430), fill=(255, 250, 210, 255))
    return title(compose(bg, draw), y=730)


def style_pulse():
    # rhythm: a beat line through nested rounded squares
    bg = gradient((8, 20, 40), (10, 70, 80))
    def draw(d):
        for i, r in enumerate([330, 250, 170]):
            a = 255 - i * 60
            d.rounded_rectangle((512 - r, 430 - r, 512 + r, 430 + r), radius=60 + i * 10,
                                outline=(80, 255, 210, a), width=22 - i * 4)
        pts = [(60, 430), (300, 430), (360, 300), (430, 600), (500, 230), (570, 520), (630, 430), (964, 430)]
        d.line(pts, fill=(255, 255, 255, 255), width=30, joint="curve")
        for x, y in pts[1:-1]:
            d.ellipse((x - 18, y - 18, x + 18, y + 18), fill=(255, 255, 255, 255))
    return title(compose(bg, draw, blur=24), y=780, size=160)


def style_monogram():
    # bold "OD" monogram on a rounded tile
    bg = gradient((255, 95, 70), (150, 40, 160))
    img = bg
    d = ImageDraw.Draw(img)
    d.rounded_rectangle((120, 90, 904, 690), radius=120, fill=(25, 14, 45, 235))
    f = font(430)
    text = "OD"
    tw = d.textlength(text, font=f)
    bbox = d.textbbox((0, 0), text, font=f)
    th = bbox[3] - bbox[1]
    x, y = (S - tw) / 2, 390 - th / 2 - bbox[1]
    glow = Image.new("RGBA", (S, S), (0, 0, 0, 0))
    ImageDraw.Draw(glow).text((x, y), text, font=f, fill=(255, 170, 90, 255))
    img = Image.alpha_composite(img, glow.filter(ImageFilter.GaussianBlur(26)))
    d = ImageDraw.Draw(img)
    d.text((x, y), text, font=f, fill=(255, 235, 200, 255))
    # speed slash
    d.polygon([(150, 600), (870, 520), (880, 560), (160, 640)], fill=(255, 140, 80, 255))
    return title(img, y=760, color=(255, 245, 235, 255))


def style_drop():
    # an "ounce" droplet streaking forward
    bg = gradient((12, 24, 70), (40, 20, 90))
    def draw(d):
        cx, cy, r = 600, 450, 170
        # droplet: circle + tapered tail pointing left
        d.ellipse((cx - r, cy - r, cx + r, cy + r), fill=(110, 200, 255, 255))
        d.polygon([(cx - r * 0.2, cy - r), (cx - r * 2.4, cy), (cx - r * 0.2, cy + r)], fill=(110, 200, 255, 255))
        d.ellipse((cx - 30, cy - 110, cx + 70, cy - 30), fill=(230, 248, 255, 255))
        for (y, x0, x1, w) in [(330, 70, 250, 16), (450, 20, 200, 12), (570, 70, 250, 16)]:
            d.rounded_rectangle((x0, y - w / 2, x1, y + w / 2), radius=w / 2, fill=(150, 220, 255, 200))
    return title(compose(bg, draw), y=760)


# ---------------------------------------------------------------- arcade styles
# Bright, chunky, black-outlined shapes in the spirit of rhythm platformers,
# with an original cube character (a four-point star emblem, no face).

OUTLINE = (10, 10, 20, 255)


def sky(c0, c1):
    img = gradient(c0, c1, diag=False)
    d = ImageDraw.Draw(img)
    for x in range(0, S, 128):  # faint background grid
        d.line((x, 0, x, S), fill=(255, 255, 255, 22), width=4)
    for y in range(0, S, 128):
        d.line((0, y, S, y), fill=(255, 255, 255, 22), width=4)
    return img


def cube(d, cx, cy, size, fill, accent, angle=0.0):
    """An outlined cube with an inner panel and a four-point star emblem."""
    def rot(px, py):
        a = math.radians(angle)
        x, y = px - cx, py - cy
        return (cx + x * math.cos(a) - y * math.sin(a), cy + x * math.sin(a) + y * math.cos(a))

    def square(h, col, width=0):
        pts = [rot(cx - h, cy - h), rot(cx + h, cy - h), rot(cx + h, cy + h), rot(cx - h, cy + h)]
        d.polygon(pts, fill=col, outline=OUTLINE if width else None, width=width)

    h = size / 2
    square(h + 14, OUTLINE)
    square(h, fill)
    square(h * 0.62, OUTLINE)
    square(h * 0.62 - 12, accent)
    # four-point star
    s, w = h * 0.5, h * 0.16
    star = [(cx, cy - s), (cx + w, cy - w), (cx + s, cy), (cx + w, cy + w),
            (cx, cy + s), (cx - w, cy + w), (cx - s, cy), (cx - w, cy - w)]
    d.polygon([rot(*p) for p in star], fill=fill, outline=OUTLINE, width=8)
    # gloss
    d.polygon([rot(cx - h, cy - h), rot(cx + h, cy - h), rot(cx + h * 0.6, cy - h * 0.72),
               rot(cx - h * 0.72, cy - h * 0.72)], fill=(255, 255, 255, 70))


def spike(d, x, base, w, h, fill=(30, 30, 40, 255)):
    d.polygon([(x - w / 2, base), (x, base - h), (x + w / 2, base)], fill=OUTLINE)
    d.polygon([(x - w / 2 + 16, base - 6), (x, base - h + 26), (x + w / 2 - 16, base - 6)], fill=fill)
    d.polygon([(x - w / 2 + 16, base - 6), (x, base - h + 26), (x - 4, base - 6)], fill=(255, 255, 255, 40))


def floor(d, y, top=(80, 140, 255, 255), body=(30, 50, 140, 255)):
    d.rectangle((0, y, S, S), fill=body)
    for x in range(0, S, 160):  # tile seams
        d.line((x, y, x, S), fill=(0, 0, 0, 60), width=6)
    d.rectangle((0, y - 6, S, y + 10), fill=(255, 255, 255, 255))
    d.rectangle((0, y + 10, S, y + 26), fill=top)


def arcade_title(img, y, size=176, fill=(255, 226, 60, 255)):
    d = ImageDraw.Draw(img)
    f = font(size)
    text = "OunceDash"
    tw = d.textlength(text, font=f, )
    x = (S - tw) / 2
    d.text((x + 10, y + 12), text, font=f, fill=(0, 0, 0, 160), stroke_width=22, stroke_fill=(0, 0, 0, 160))
    d.text((x, y), text, font=f, fill=fill, stroke_width=20, stroke_fill=OUTLINE)
    return img


def style_runner():
    # cube jumping over a spike, blue sky
    img = sky((40, 90, 255), (120, 30, 200))
    d = ImageDraw.Draw(img)
    floor(d, 600)
    spike(d, 560, 600, 200, 190)
    # jump trail
    for i, (x, y, r) in enumerate([(250, 520, 26), (320, 450, 30), (390, 400, 34)]):
        d.rounded_rectangle((x - r, y - r, x + r, y + r), radius=6, fill=(255, 230, 80, 90 + i * 50))
    cube(d, 560, 330, 210, (255, 210, 40, 255), (40, 220, 255, 255), angle=-20)
    return arcade_title(img, 690)


def style_trail():
    # tilted cube racing right with a square trail, green-to-cyan sky
    img = sky((20, 200, 140), (10, 60, 140))
    d = ImageDraw.Draw(img)
    floor(d, 610, top=(120, 255, 160, 255), body=(10, 90, 70, 255))
    for i in range(6):
        x = 120 + i * 70
        r = 14 + i * 7
        d.rectangle((x - r, 420 - r, x + r, 420 + r), fill=(255, 255, 255, 40 + i * 30))
    spike(d, 860, 610, 150, 140, fill=(20, 40, 50, 255))
    cube(d, 600, 420, 250, (60, 230, 255, 255), (255, 80, 200, 255), angle=15)
    return arcade_title(img, 690, fill=(120, 255, 120, 255))


def style_logo():
    # title-first: big outlined wordmark over a spike floor, small cube
    img = sky((255, 90, 60), (140, 20, 120))
    d = ImageDraw.Draw(img)
    floor(d, 700, top=(255, 160, 60, 255), body=(90, 20, 60, 255))
    for x in (130, 300, 720, 890):
        spike(d, x, 700, 150, 140, fill=(60, 10, 40, 255))
    cube(d, 512, 560, 200, (60, 255, 140, 255), (255, 240, 70, 255), angle=8)
    f = font(210)
    text = "Ounce"
    tw = d.textlength(text, font=f)
    d.text(((S - tw) / 2, 70), text, font=f, fill=(255, 226, 60, 255), stroke_width=22, stroke_fill=OUTLINE)
    text = "Dash"
    tw = d.textlength(text, font=f)
    d.text(((S - tw) / 2, 250), text, font=f, fill=(90, 220, 255, 255), stroke_width=22, stroke_fill=OUTLINE)
    return img


def style_orb():
    # cube touching a glowing jump ring above a block pillar
    img = sky((60, 30, 180), (230, 60, 140))
    d = ImageDraw.Draw(img)
    floor(d, 640, top=(255, 120, 220, 255), body=(70, 20, 110, 255))
    # block pillar
    for row in range(2):
        d.rectangle((700, 640 - (row + 1) * 120, 840, 640 - row * 120), fill=OUTLINE)
        d.rectangle((714, 654 - (row + 1) * 120, 826, 626 - row * 120), fill=(40, 20, 80, 255))
    glow = Image.new("RGBA", (S, S), (0, 0, 0, 0))
    ImageDraw.Draw(glow).ellipse((380, 160, 600, 380), outline=(255, 240, 90, 255), width=40)
    img = Image.alpha_composite(img, glow.filter(ImageFilter.GaussianBlur(24)))
    d = ImageDraw.Draw(img)
    d.ellipse((380, 160, 600, 380), outline=OUTLINE, width=50)
    d.ellipse((392, 172, 588, 368), outline=(255, 240, 90, 255), width=26)
    cube(d, 300, 470, 190, (255, 120, 220, 255), (90, 230, 255, 255), angle=-30)
    return arcade_title(img, 700)


# ---------------------------------------------------------------- centered chevrons
# The chevron icon with the whole group (speed lines + three chevrons)
# centered in the area above the title, in several colour schemes.

CHEVRON_PALETTES = {
    # background top-left, bottom-right, three chevrons, speed lines, title
    "sunset": ((40, 12, 70), (120, 30, 90), [(255, 120, 90), (255, 170, 70), (255, 225, 90)],
               (255, 190, 150), (255, 244, 230)),
    "ocean": ((8, 20, 60), (10, 90, 130), [(60, 170, 255), (80, 220, 255), (170, 245, 255)],
              (140, 210, 255), (235, 250, 255)),
    "neon": ((14, 8, 30), (50, 16, 80), [(255, 70, 200), (170, 90, 255), (70, 220, 255)],
             (200, 150, 255), (250, 240, 255)),
    "lime": ((6, 40, 46), (14, 90, 80), [(120, 255, 140), (210, 255, 90), (255, 220, 70)],
            (170, 255, 190), (240, 255, 240)),
    "candy": ((60, 20, 90), (230, 90, 140), [(255, 255, 255), (255, 210, 240), (255, 160, 220)],
              (255, 220, 245), (255, 255, 255)),
    # red "top speed" schemes on different backgrounds
    "red_night": ((14, 10, 24), (40, 14, 30), [(200, 20, 40), (255, 50, 50), (255, 120, 100)],
                  (255, 110, 110), (255, 240, 240)),
    "red_ember": ((60, 6, 10), (140, 30, 10), [(255, 70, 50), (255, 130, 60), (255, 200, 90)],
                  (255, 170, 120), (255, 245, 230)),
    "red_steel": ((24, 28, 38), (60, 66, 82), [(220, 30, 50), (255, 60, 70), (255, 110, 110)],
                  (255, 140, 140), (240, 244, 250)),
    "red_crimson": ((90, 0, 20), (200, 20, 50), [(255, 255, 255), (255, 200, 200), (255, 140, 140)],
                    (255, 200, 205), (255, 255, 255)),
    # the chosen icon: steel background, four chevrons and the speed lines in one red ramp
    "steel4": ((24, 28, 38), (60, 66, 82),
               [(200, 25, 45), (230, 40, 55), (255, 65, 70), (255, 105, 105)],
               (210, 40, 55), (255, 250, 250), (255, 178, 182)),   # title: white -> light red
}


def gradient_title(img, y, top, bottom, size=176):
    """Title with a vertical colour gradient, a thin dark outline and a soft shadow."""
    f = font(size)
    text = "OunceDash"
    probe = ImageDraw.Draw(img)
    tw = probe.textlength(text, font=f)
    x = (S - tw) / 2
    l, t, r, b = probe.textbbox((x, y), text, font=f, stroke_width=10)
    # soft shadow
    shadow = Image.new("RGBA", (S, S), (0, 0, 0, 0))
    ImageDraw.Draw(shadow).text((x + 4, y + 12), text, font=f, fill=(0, 0, 0, 170),
                                stroke_width=10, stroke_fill=(0, 0, 0, 170))
    img = Image.alpha_composite(img, shadow.filter(ImageFilter.GaussianBlur(10)))
    # dark outline
    ImageDraw.Draw(img).text((x, y), text, font=f, fill=(0, 0, 0, 0), stroke_width=10,
                             stroke_fill=(30, 14, 20, 230))
    # gradient fill through the glyph mask
    mask = Image.new("L", (S, S), 0)
    ImageDraw.Draw(mask).text((x, y), text, font=f, fill=255)
    fill = Image.new("RGBA", (S, S))
    fp = fill.load()
    for yy in range(S):
        k = min(1.0, max(0.0, (yy - t) / max(1, b - t)))
        k = k * k * (3 - 2 * k)  # smooth
        c = tuple(int(a + (bb - a) * k) for a, bb in zip(top, bottom)) + (255,)
        for xx in range(int(l), int(r) + 1):
            fp[xx, yy] = c
    img.paste(fill, (0, 0), mask)
    return img


def style_chevron_palette(name):
    c0, c1, chev, streak, text, *text_bottom = CHEVRON_PALETTES[name]
    bg = gradient(c0, c1)
    k = 3.0 / max(3, len(chev)) * (1.09 if len(chev) > 3 else 1.0)  # shrink to fit more chevrons
    w, h, t, gap = 230 * k, 400 * k, 92 * k, 190 * k   # chevron size, thickness, spacing
    streaks = [(-130 * k, 18 * k, 190 * k), (-50 * k, 14 * k, 210 * k),
               (50 * k, 14 * k, 205 * k), (130 * k, 18 * k, 190 * k)]  # (dy, width, length)
    lead = 40 * k                                # space between the lines and the first chevron
    longest = max(s[2] for s in streaks)
    group_w = longest + lead + (len(chev) - 1) * gap + w
    x0 = (S - group_w) / 2                       # left edge of the group, centered
    cy = 395                                     # centre of the area above the title
    first_cx = x0 + longest + lead + w / 2

    def draw(d):
        for i, c in enumerate(chev):
            chevron(d, first_cx + i * gap, cy, w, h, t, c + (255,))
        right = first_cx - w / 2 - lead
        for dy, sw, ln in streaks:
            d.rounded_rectangle((right - ln, cy + dy - sw / 2, right, cy + dy + sw / 2), radius=sw / 2,
                                fill=streak + (230,))
    img = compose(bg, draw)
    if text_bottom:
        return gradient_title(img, 744, text, text_bottom[0])
    return title(img, y=748, color=text + (255,))


for _name in CHEVRON_PALETTES:
    globals()["style_chev_" + _name] = (lambda n: lambda: style_chevron_palette(n))(_name)

STYLES = {"chevrons": style_chevrons,
          **{"chev_" + n: globals()["style_chev_" + n] for n in CHEVRON_PALETTES}, "jump": style_jump, "pulse": style_pulse,
          "monogram": style_monogram, "drop": style_drop,
          "runner": style_runner, "trail": style_trail, "logo": style_logo, "orb": style_orb}

if __name__ == "__main__":
    out = sys.argv[1]
    style = sys.argv[2] if len(sys.argv) > 2 else "chev_steel4"
    img = STYLES[style]()
    img.convert("RGB").resize((256, 256), Image.LANCZOS).save(out, quality=92)
    print("wrote", out, "(" + style + ")")
