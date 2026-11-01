#!/usr/bin/env python3
"""Generate glyph masks for the Knob Timer firmware.

Renders Manrope (variable font) into 8-bit coverage masks and writes
main/assets.c and main/assets.h. Sizes match the HTML preview
(preview/toggl-pomodoro-timer.html):

  time      88 px (MM:SS) and 62 px (HH:MM:SS), weight 600, tabular figures,
            plus a Gaussian-blurred 88 px set (sigma 8) for the last-minute pulse glow
  task      28 px, weight 600, letter-spacing -0.01em (laid out at run time)
  lists     18 px, weight 600
  pill      15 px, weight 700 (the project name)
  status    14 px, weight 600 (fixed strings)
  icons     mic button and ring (1.5x), Wi-Fi glyph

Usage:  python3 tools/gen_assets.py      (needs Pillow and fontTools: pip install pillow fonttools)
"""
import os
import tempfile
from pathlib import Path

from PIL import Image, ImageDraw, ImageFilter, ImageFont

ROOT = Path(__file__).resolve().parent.parent
FONT = ROOT / "tools" / "fonts" / "Manrope-VariableFont_wght.ttf"
OUT_C = ROOT / "main" / "assets.c"
OUT_H = ROOT / "main" / "assets.h"

TIME_PX, TIME_SMALL_PX = 88, 62
STATUS_PX = 14
GLOW_SIGMA = 8
GLOW_PAD = 3 * GLOW_SIGMA
TASK_PX, TASK_TRACK = 28, -0.01  # letter-spacing in em
LIST_PX = 18
PILL_PX = 15
CHARS = [chr(c) for c in range(32, 127)] + ["…"]  # index 95 = ellipsis
TIME_CHARS = "0123456789:"

# Order must match str_id_t in assets.h (generated below).
STRINGS = [
    ("STR_TAP_TO_TRACK", "tap to track"),
    ("STR_TAP_TO_START", "tap to start"),
    ("STR_PAUSED", "paused"),
    ("STR_TIMES_UP", "time’s up"),
    ("STR_RESET", "reset"),
    ("STR_MIN_1", "1 min minimum"),
    ("STR_MAX_60", "60 min maximum"),
    ("STR_PAUSE_TO_ADJUST", "pause to adjust"),
    ("STR_PAUSE_TO_SWITCH", "pause to switch"),
    ("STR_TRACKING", "tracking"),
    ("STR_POMODORO", "pomodoro"),
    ("STR_MONO", "mono"),
    ("STR_INVERTED", "inverted"),
    ("STR_SYNCING", "syncing…"),
    ("STR_SYNCED", "synced from Toggl"),
    ("STR_STOPPED_IN_TOGGL", "stopped in Toggl"),
    ("STR_IN_SYNC", "in sync"),
    ("STR_TRANSCRIBING", "transcribing…"),
    ("STR_NOT_HEARD", "didn’t catch that"),
    ("STR_NO_WIFI", "no Wi-Fi"),
    ("STR_STT_FAILED", "transcription failed"),
    ("STR_TAP_TO_CHOOSE", "tap to choose"),
    ("STR_NO_PROJECTS", "no projects yet"),
    ("STR_TOGGL_OFF", "Toggl isn’t set up"),
    ("STR_LOADING", "loading…"),
    ("STR_TOGGL_ERROR", "couldn’t reach Toggl"),
    ("STR_NO_ENTRIES", "no recent entries"),
    ("STR_NOT_SAVED", "not saved in Toggl"),
]


BTN_SCALE = 1.5  # the mic is drawn 50% larger than the base geometry below (ring r 19 -> ~28.5)


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
    """Wi-Fi status: a dot and three arcs opening upward, about 15 x 11 px.
    The arcs' common center sits 5.5 px below the anchor (the preview's
    (180, 352.5) is anchor (180, 347)). Drawn 8x larger and scaled down."""
    S, R = 8, 12
    img = Image.new("L", (2 * R * S, 2 * R * S), 0)
    d = ImageDraw.Draw(img)
    w = round(1.7 * S)
    cx, cy = R * S, (R + 5.5) * S
    for r in (3.5, 6.5, 9.5):
        rr = r * S
        d.arc([cx - rr, cy - rr, cx + rr, cy + rr], start=225, end=315, fill=255, width=w)
    dot = 1.5 * S
    d.ellipse([cx - dot, cy - dot, cx + dot, cy + dot], fill=255)
    img = img.resize((2 * R, 2 * R), Image.LANCZOS)
    return 2 * R, 2 * R, -R, -R, img.tobytes()


def tabular_font_file():
    """Manrope at weight 600 with the digits mapped to their tabular (.tf) forms,
    like CSS font-variant-numeric: tabular-nums (Pillow can't apply OpenType
    features without libraqm). Returns the path of a temporary TTF."""
    from fontTools.ttLib import TTFont
    from fontTools.varLib import instancer

    f = instancer.instantiateVariableFont(TTFont(str(FONT)), {"wght": 600})
    names = {"0": "zero", "1": "one", "2": "two", "3": "three", "4": "four",
             "5": "five", "6": "six", "7": "seven", "8": "eight", "9": "nine"}
    glyphs = set(f.getGlyphOrder())
    for table in f["cmap"].tables:
        if not table.isUnicode():
            continue
        for d, name in names.items():
            if name + ".tf" in glyphs:
                table.cmap[ord(d)] = name + ".tf"
    fd, path = tempfile.mkstemp(suffix=".ttf")
    os.close(fd)
    f.save(path)
    return path


def load_font(px, wght=600):
    f = ImageFont.truetype(str(FONT), px)
    f.set_variation_by_axes([wght])
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


def c_string(text):
    out = ""
    for byte in text.encode("utf-8"):
        out += chr(byte) if 32 <= byte < 127 and chr(byte) not in '"\\' else f"\\{byte:03o}"
    return f'"{out}"'


def main():
    status_font = load_font(STATUS_PX)
    parts = ['// Generated by tools/gen_assets.py. Do not edit by hand.\n#include "assets.h"\n']
    tables = {}

    def add(table, key, name, rendered):
        w, h, ox, oy, data = rendered
        parts.append(c_array(name, data))
        tables.setdefault(table, []).append((key, f"{{{w}, {h}, {ox}, {oy}, {name}}}"))

    for enum_name, text in STRINGS:
        add("str", enum_name, f"s_{enum_name.lower()}", render(text, status_font))

    icon, ring = mic_masks()
    add("icon", "mic", "mic_icon", icon)
    add("icon", "ring", "mic_ring", ring)
    add("icon", "wifi", "wifi_icon", wifi_mask())

    # Glyph fonts: masks anchored at the pen position on the baseline, plus advances in 1/16 px.
    def build_font(font, chars, prefix, track_px=0.0, pad=0, blur=0):
        entries = []
        for i, ch in enumerate(chars):
            l, t, r, b = font.getbbox(ch, anchor="ls")
            adv16 = round((font.getlength(ch) + track_px) * 16)
            if r <= l or b <= t:
                entries.append(f"{{{{0, 0, 0, 0, 0}}, {adv16}}}")
                continue
            w, h = r - l + 2 * pad, b - t + 2 * pad
            img = Image.new("L", (w, h), 0)
            ImageDraw.Draw(img).text((pad - l, pad - t), ch, fill=255, font=font, anchor="ls")
            if blur:
                img = img.filter(ImageFilter.GaussianBlur(blur))
            name = f"{prefix}_{i}"
            parts.append(c_array(name, img.tobytes()))
            entries.append(f"{{{{{w}, {h}, {l - pad}, {t - pad}, {name}}}, {adv16}}}")
        return entries

    tab_path = tabular_font_file()
    try:
        time_l = ImageFont.truetype(tab_path, TIME_PX)
        time_s = ImageFont.truetype(tab_path, TIME_SMALL_PX)
        fonts = {
            "TIME_FONT": build_font(time_l, TIME_CHARS, "tl"),
            "TIME_SMALL_FONT": build_font(time_s, TIME_CHARS, "ts"),
            "TIME_GLOW": build_font(time_l, TIME_CHARS, "tg", pad=GLOW_PAD, blur=GLOW_SIGMA),
        }
    finally:
        os.unlink(tab_path)
    fonts["TASK_FONT"] = build_font(load_font(TASK_PX), CHARS, "tk", track_px=TASK_TRACK * TASK_PX)
    fonts["LIST_FONT"] = build_font(load_font(LIST_PX), CHARS, "li")
    fonts["PILL_FONT"] = build_font(load_font(PILL_PX, 700), CHARS, "pl")

    body = ",\n".join(f"    {v}" for _, v in tables["str"])
    parts.append(f"const mask_t STRINGS[{len(STRINGS)}] = {{\n{body}\n}};\n")
    parts.append("const char *const STRING_TEXT[] = {\n" + ",\n".join(f"    {c_string(t)}" for _, t in STRINGS) + "\n};\n")
    icons = dict(tables["icon"])
    parts.append(f"const mask_t MIC_ICON = {icons['mic']};\nconst mask_t MIC_RING = {icons['ring']};\n"
                 f"const mask_t WIFI_ICON = {icons['wifi']};\n")
    for c_name, entries in fonts.items():
        parts.append(f"const glyph_t {c_name}[{len(entries)}] = {{\n" + ",\n".join(f"    {e}" for e in entries) + "\n};\n")
    OUT_C.write_text("\n".join(parts))

    enum = ",\n".join(f"    {n}" for n, _ in STRINGS)
    OUT_H.write_text(f"""// Generated by tools/gen_assets.py. Do not edit by hand.
#pragma once
#include <stdint.h>

// 8-bit coverage mask. (ox, oy) is the offset from the anchor to the mask's top-left
// pixel: the text's horizontal middle on the baseline for strings, the center for icons.
typedef struct {{
    uint16_t w, h;
    int16_t ox, oy;
    const uint8_t *data;
}} mask_t;

typedef enum {{
{enum},
    STR_COUNT
}} str_id_t;

// Glyph fonts. Each mask is anchored at the pen position on the baseline; adv16 is the
// advance in 1/16 px. Text fonts hold printable ASCII from FONT_FIRST, then an ellipsis;
// time fonts hold the digits 0-9 (tabular) and a colon (TIME_COLON).
typedef struct {{
    mask_t m;
    uint16_t adv16;
}} glyph_t;

#define FONT_FIRST 32
#define FONT_ELLIPSIS 95
#define FONT_COUNT 96
#define TIME_COLON 10

extern const mask_t STRINGS[STR_COUNT];   // 14 px status words
extern const char *const STRING_TEXT[];   // the same strings as text (debug)
extern const glyph_t TIME_FONT[11];       // 88 px: MM:SS
extern const glyph_t TIME_SMALL_FONT[11]; // 62 px: HH:MM:SS
extern const glyph_t TIME_GLOW[11];       // 88 px, blurred (sigma {GLOW_SIGMA}): the pulse glow
extern const glyph_t TASK_FONT[FONT_COUNT]; // 28 px: the task
extern const glyph_t LIST_FONT[FONT_COUNT]; // 18 px: list rows
extern const glyph_t PILL_FONT[FONT_COUNT]; // 15 px bold: the project pill
extern const mask_t MIC_ICON, MIC_RING;   // mic button, anchored at its center
extern const mask_t WIFI_ICON;            // Wi-Fi status, in the rim at 6 o'clock
""")
    print(f"wrote {OUT_C.relative_to(ROOT)} ({OUT_C.stat().st_size // 1024} KB source)")


if __name__ == "__main__":
    main()
