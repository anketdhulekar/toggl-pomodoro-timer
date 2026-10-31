#!/usr/bin/env python3
"""Generate glyph masks for the Knob Pomodoro firmware.

Renders Manrope SemiBold (wght 600) into 8-bit coverage masks and writes
main/assets.c and main/assets.h. Sizes and positions match the HTML preview:
digits at 96 px, status words at 14 px, and a Gaussian-blurred copy of each
digit (sigma 8) for the last-minute pulse glow. Also a small 18 px font
(printable ASCII plus an ellipsis) for the spoken task name, which is laid
out at run time.

Usage:  python3 tools/gen_assets.py      (needs Pillow: pip install pillow)
"""
from pathlib import Path

from PIL import Image, ImageDraw, ImageFilter, ImageFont

ROOT = Path(__file__).resolve().parent.parent
FONT = ROOT / "tools" / "fonts" / "Manrope-VariableFont_wght.ttf"
OUT_C = ROOT / "main" / "assets.c"
OUT_H = ROOT / "main" / "assets.h"

DIGIT_PX = 96
STATUS_PX = 14
GLOW_SIGMA = 8
GLOW_PAD = 3 * GLOW_SIGMA
TASK_PX = 18
SMALL_PX = 15  # project name under the task
TASK_CHARS = [chr(c) for c in range(32, 127)] + ["\u2026"]  # index 95 = ellipsis

# Order must match str_id_t in assets.h (generated below).
STRINGS = [
    ("STR_TAP_TO_START", "tap to start"),
    ("STR_PAUSED", "paused"),
    ("STR_TIMES_UP", "time’s up"),
    ("STR_RESET", "reset"),
    ("STR_MIN_1", "1 min minimum"),
    ("STR_MAX_60", "60 min maximum"),
    ("STR_LESS_THAN_MIN", "less than a minute left"),
    ("STR_PAUSE_TO_CHANGE", "pause to change colors"),
    ("STR_PAUSE_TO_ADJUST", "pause to adjust"),
    ("STR_PAL_SUNFLOWER", "sunflower"),
    ("STR_PAL_LAGOON", "lagoon"),
    ("STR_PAL_LIME", "lime"),
    ("STR_PAL_GLACIER", "glacier"),
    ("STR_PAL_MINT", "mint"),
    ("STR_PAL_SEAFOAM", "seafoam"),
    ("STR_PAL_CREAM", "cream"),
    ("STR_PAL_MONO", "mono"),
    ("STR_LISTENING", "listening\u2026"),
    ("STR_TRANSCRIBING", "transcribing\u2026"),
    ("STR_NOT_HEARD", "didn\u2019t catch that"),
    ("STR_NO_WIFI", "no Wi-Fi"),
    ("STR_VOICE_OFF", "voice isn\u2019t set up"),
    ("STR_STT_FAILED", "transcription failed"),
    ("STR_TAP_TO_CHOOSE", "tap to choose"),
    ("STR_PAUSE_TO_PICK", "pause to change project"),
    ("STR_NO_PROJECTS", "no projects yet"),
    ("STR_TOGGL_OFF", "Toggl isn\u2019t set up"),
    ("STR_LOADING", "loading\u2026"),
    ("STR_TOGGL_ERROR", "couldn\u2019t reach Toggl"),
    ("STR_NO_ENTRIES", "no recent entries"),
]


BTN_SCALE = 1.5  # buttons are drawn 50% larger than the base geometry below (ring r 19 -> ~28.5)


def mic_masks():
    """Mic button, same geometry as the preview's SVG (centered on the anchor):
    ring r=19, capsule 9x15, cradle arc r=8.5, stem and base; strokes 1.8 px,
    all scaled by BTN_SCALE. Drawn 8x larger and scaled down for anti-aliasing.
    Returns (icon, ring)."""
    S, R, K = 8, 36, BTN_SCALE  # supersampling factor, half size of the mask in px, scale

    def canvas():
        return Image.new("L", (2 * R * S, 2 * R * S), 0)

    def P(x, y):  # preview coordinates (center 180, 298) -> canvas
        return (((x - 180) * K + R) * S, ((y - 298) * K + R) * S)

    def finish(img):
        img = img.resize((2 * R, 2 * R), Image.LANCZOS)
        return 2 * R, 2 * R, -R, -R, img.tobytes()

    w = round(1.8 * K * S)
    ring = canvas()
    ImageDraw.Draw(ring).ellipse([P(161, 279), P(199, 317)], outline=255, width=w)

    icon = canvas()
    d = ImageDraw.Draw(icon)
    d.rounded_rectangle([P(175.5, 286), P(184.5, 301)], radius=4.5 * K * S, fill=255)
    d.arc([P(171.5, 287), P(188.5, 304)], start=0, end=180, fill=255, width=w)
    d.line([P(180, 304), P(180, 309)], fill=255, width=w)
    d.line([P(175.5, 309.5), P(184.5, 309.5)], fill=255, width=w)
    for x, y in [(171.5, 295.5), (188.5, 295.5), (175.5, 309.5), (184.5, 309.5)]:  # round caps
        cx, cy = P(x, y)
        d.ellipse([cx - w / 2, cy - w / 2, cx + w / 2, cy + w / 2], fill=255)
    return finish(icon), finish(ring)


def wifi_mask():
    """Wi-Fi status: a dot and three arcs opening upward, about 15 x 11 px,
    anchored at its visual center. Drawn 8x larger and scaled down."""
    S, R = 8, 12
    img = Image.new("L", (2 * R * S, 2 * R * S), 0)
    d = ImageDraw.Draw(img)
    w = round(1.7 * S)
    cx, cy = R * S, (R + 5) * S  # the arcs' common center sits low so the icon is centered
    for r in (3.5, 6.5, 9.5):
        rr = r * S
        d.arc([cx - rr, cy - rr, cx + rr, cy + rr], start=225, end=315, fill=255, width=w)
    dot = 1.5 * S
    d.ellipse([cx - dot, cy - dot, cx + dot, cy + dot], fill=255)
    img = img.resize((2 * R, 2 * R), Image.LANCZOS)
    return 2 * R, 2 * R, -R, -R, img.tobytes()


def history_mask():
    """Recent-entries badge: a clock with a counter-clockwise arrow around it,
    same geometry as the preview's SVG (centered on the anchor, arc r=9.5,
    strokes 1.8 px, scaled by BTN_SCALE). Drawn 8x larger and scaled down."""
    S, R, K = 8, 36, BTN_SCALE
    img = Image.new("L", (2 * R * S, 2 * R * S), 0)
    d = ImageDraw.Draw(img)
    w = round(1.8 * K * S)

    def P(x, y):  # local coordinates around the center -> canvas
        return ((x * K + R) * S, (y * K + R) * S)

    r = 9.5  # (as big as the mic glyph reads)
    # Arc from the upper left, clockwise round to 9 o'clock (Pillow angles: 0 = 3 o'clock, clockwise).
    d.arc([P(-r, -r), P(r, r)], start=225, end=180, fill=255, width=w)
    d.polygon([P(-r, -4.4), P(-r - 3.6, 0.8), P(-r + 3.6, 0.8)], fill=255)  # arrowhead at 9 o'clock
    d.line([P(0, 0), P(0, -5.4)], fill=255, width=w)                          # clock hands
    d.line([P(0, 0), P(3.8, 2.5)], fill=255, width=w)
    for x, y in [(0, -5.4), (3.8, 2.5), (0, 0)]:
        cx, cy = P(x, y)
        d.ellipse([cx - w / 2, cy - w / 2, cx + w / 2, cy + w / 2], fill=255)
    img = img.resize((2 * R, 2 * R), Image.LANCZOS)
    return 2 * R, 2 * R, -R, -R, img.tobytes()


def load_font(px):
    f = ImageFont.truetype(str(FONT), px)
    f.set_variation_by_axes([600])
    return f


def render(text, font, pad=0, blur=0):
    """Return (w, h, ox, oy, bytes). ox/oy are offsets from the anchor
    point (horizontal middle, alphabetic baseline) to the mask's top-left."""
    l, t, r, b = font.getbbox(text, anchor="ms")
    w, h = (r - l) + 2 * pad, (b - t) + 2 * pad
    img = Image.new("L", (w, h), 0)
    ImageDraw.Draw(img).text((pad - l, pad - t), text, fill=255, font=font, anchor="ms")
    if blur:
        img = img.filter(ImageFilter.GaussianBlur(blur))
    return w, h, l - pad, t - pad, img.tobytes()


def c_array(name, data):
    lines = []
    for i in range(0, len(data), 24):
        lines.append("    " + ",".join(str(v) for v in data[i:i + 24]) + ",")
    return f"static const uint8_t {name}[{len(data)}] = {{\n" + "\n".join(lines) + "\n};\n"


def main():
    digit_font = load_font(DIGIT_PX)
    status_font = load_font(STATUS_PX)

    parts = ['// Generated by tools/gen_assets.py. Do not edit by hand.\n#include "assets.h"\n']
    tables = {}

    def add(table, key, name, rendered):
        w, h, ox, oy, data = rendered
        parts.append(c_array(name, data))
        tables.setdefault(table, []).append((key, f"{{{w}, {h}, {ox}, {oy}, {name}}}"))

    for ch, label in [(str(d), str(d)) for d in range(10)] + [(":", "colon")]:
        add("glyph", ch, f"g_{label}", render(ch, digit_font))
        add("glow", ch, f"glow_{label}", render(ch, digit_font, pad=GLOW_PAD, blur=GLOW_SIGMA))
    for enum_name, text in STRINGS:
        add("str", enum_name, f"s_{enum_name.lower()}", render(text, status_font))

    icon, ring = mic_masks()
    add("mic", "icon", "mic_icon", icon)
    add("mic", "ring", "mic_ring", ring)
    add("mic", "history", "history_icon", history_mask())
    add("mic", "wifi", "wifi_icon", wifi_mask())

    # Run-time fonts: masks anchored at the pen position on the baseline, plus advances in 1/16 px.
    def build_font(px, prefix):
        font = load_font(px)
        entries = []
        for i, ch in enumerate(TASK_CHARS):
            l, t, r, b = font.getbbox(ch, anchor="ls")
            adv16 = round(font.getlength(ch) * 16)
            if r <= l or b <= t:
                entries.append(f"{{{{0, 0, 0, 0, 0}}, {adv16}}}")
                continue
            img = Image.new("L", (r - l, b - t), 0)
            ImageDraw.Draw(img).text((-l, -t), ch, fill=255, font=font, anchor="ls")
            name = f"{prefix}_{i}"
            parts.append(c_array(name, img.tobytes()))
            entries.append(f"{{{{{r - l}, {b - t}, {l}, {t}, {name}}}, {adv16}}}")
        return entries

    task_entries = build_font(TASK_PX, "t")
    small_entries = build_font(SMALL_PX, "sm")

    def emit(c_name, entries):
        body = ",\n".join(f"    {v}" for _, v in entries)
        return f"const mask_t {c_name}[{len(entries)}] = {{\n{body}\n}};\n"

    glyphs, glows = tables["glyph"], tables["glow"]
    parts.append(emit("GLYPH_DIGITS", glyphs[:10]))
    parts.append(f"const mask_t GLYPH_COLON = {glyphs[10][1]};\n")
    parts.append(emit("GLOW_DIGITS", glows[:10]))
    parts.append(f"const mask_t GLOW_COLON = {glows[10][1]};\n")
    parts.append(emit("STRINGS", tables["str"]))
    parts.append(f"const mask_t MIC_ICON = {tables['mic'][0][1]};\nconst mask_t MIC_RING = {tables['mic'][1][1]};\n"
                 f"const mask_t HISTORY_ICON = {tables['mic'][2][1]};\nconst mask_t WIFI_ICON = {tables['mic'][3][1]};\n")
    for c_name, entries in [("TASK_FONT", task_entries), ("SMALL_FONT", small_entries)]:
        parts.append(f"const glyph_t {c_name}[{len(TASK_CHARS)}] = {{\n" + ",\n".join(f"    {e}" for e in entries) + "\n};\n")
    OUT_C.write_text("\n".join(parts))

    enum = ",\n".join(f"    {n}" for n, _ in STRINGS)
    OUT_H.write_text(f"""// Generated by tools/gen_assets.py. Do not edit by hand.
#pragma once
#include <stdint.h>

// 8-bit coverage mask. (ox, oy) is the offset from the text anchor
// (horizontal middle, alphabetic baseline) to the mask's top-left pixel.
typedef struct {{
    uint16_t w, h;
    int16_t ox, oy;
    const uint8_t *data;
}} mask_t;

typedef enum {{
{enum},
    STR_COUNT
}} str_id_t;

#define STR_PAL_FIRST STR_PAL_SUNFLOWER

// Run-time font for the task name: printable ASCII from TASK_FONT_FIRST, then an ellipsis.
// Each mask is anchored at the pen position on the baseline; adv16 is the advance in 1/16 px.
typedef struct {{
    mask_t m;
    uint16_t adv16;
}} glyph_t;

#define TASK_FONT_FIRST 32
#define TASK_FONT_ELLIPSIS 95
#define TASK_FONT_COUNT 96

extern const mask_t GLYPH_DIGITS[10];
extern const mask_t GLYPH_COLON;
extern const mask_t GLOW_DIGITS[10];
extern const mask_t GLOW_COLON;
extern const mask_t STRINGS[STR_COUNT];
extern const glyph_t TASK_FONT[TASK_FONT_COUNT];  // 18 px: the task
extern const glyph_t SMALL_FONT[TASK_FONT_COUNT]; // 15 px: the project under it
extern const mask_t MIC_ICON, MIC_RING; // mic button, anchored at its center
extern const mask_t HISTORY_ICON;      // recent Toggl entries button (shares MIC_RING)
extern const mask_t WIFI_ICON;         // Wi-Fi status, between the buttons
""")
    total = sum(len(render(c, digit_font)[4]) for c in "0123456789:")
    print(f"wrote {OUT_C.relative_to(ROOT)} ({OUT_C.stat().st_size // 1024} KB source), digit masks {total // 1024} KB")


if __name__ == "__main__":
    main()
