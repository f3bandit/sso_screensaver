"""
16:9 version, v3 -- fixes both real bugs from v2:
1. Scroll motion: reverted to the EXACT proven tile-index-loop formula
   the working 4:3 version uses (the v2 "continuous crop" rewrite was
   the actual cause of the jerky motion, not a fix for anything).
2. Sharpness: rendering at 2x resolution (482px tall instead of 241)
   so runtime upscaling to a real screen needs much less stretching,
   which is what was causing the blur/jagged edges/logo pixelation --
   that's a nearest-neighbor runtime-scaling artifact, made more
   visible because 16:9 content fills nearly the whole screen instead
   of being letterboxed like 4:3.
"""
from PIL import Image, ImageDraw, ImageFilter, ImageEnhance, ImageOps
import struct

SCALE = 2
CH = 241 * SCALE   # 482
CW = int(CH * 16 / 9)  # 856
W, H = CW, CH
ORIG_CW = 319 * SCALE  # reference width the original layout was designed at, scaled up

logo_title = Image.open("/home/claude/gif_work/logo_title_top.png").convert("RGBA")
logo_r2 = Image.open("/home/claude/gif_work/logo_r2_center.png").convert("RGBA")
logo_taki = Image.open("/home/claude/gif_work/logo_taki_odon_bottom.png").convert("RGBA")

def upscale2x(img):
    return img.resize((img.width * SCALE, img.height * SCALE), Image.LANCZOS)

logo_title = upscale2x(logo_title)
logo_r2 = upscale2x(logo_r2)
logo_taki = upscale2x(logo_taki)

src = Image.open("/home/claude/gif_work/bg_schematic_hires.png").convert("RGB")

# NOTE: no blur applied here. That processing step was inherited from
# an unrelated PS1 video-encoding pipeline, where it helped a lossy
# compressor spend its fixed bit budget on the important content
# instead of fine circuit-trace noise. This screensaver reads raw,
# uncompressed pixel data straight into RAM -- there's no codec, no
# bit budget, nothing for a blur to help with. It only cost sharpness.

def to_tinted_layer(img, tint, boost=1.5):
    gray = ImageOps.grayscale(img)
    gray = ImageEnhance.Contrast(gray).enhance(1.15)
    gray = ImageEnhance.Brightness(gray).enhance(boost)
    alpha = gray.point(lambda p: min(255, int(p * 1.25)))
    solid = Image.new("RGBA", img.size, tint + (0,))
    solid.putalpha(alpha)
    return solid

def to_default_layer(img, dim=0.75):
    gray = ImageOps.grayscale(img)
    alpha = gray.point(lambda p: min(255, int(p * 1.15)))
    r, g, b = img.split()
    solid = Image.merge("RGBA", (r, g, b, alpha))
    solid = ImageEnhance.Brightness(solid).enhance(dim)
    return solid

RED_A = (215, 25, 20)
RED_B = (255, 130, 30)

layer1_src = to_tinted_layer(src, RED_A)
layer2_src = to_default_layer(src)
layer3_src = to_tinted_layer(src, RED_B, boost=1.3)

def make_h_tile(base, target_h, min_w):
    scale = target_h / base.height
    tw = int(base.width * scale)
    tile = base.resize((tw, target_h), Image.LANCZOS)
    reps = max(3, (min_w * 3) // tw + 2)
    tiled = Image.new("RGBA", (tw * reps, target_h), (0, 0, 0, 0))
    for i in range(reps):
        tiled.paste(tile, (i * tw, 0))
    return tiled, tw

def make_v_tile(base, target_w, min_h):
    scale = target_w / base.width
    th = int(base.height * scale)
    tile = base.resize((target_w, th), Image.LANCZOS)
    reps = max(3, (min_h * 3) // th + 2)
    tiled = Image.new("RGBA", (target_w, th * reps), (0, 0, 0, 0))
    for i in range(reps):
        tiled.paste(tile, (0, i * th))
    return tiled, th

L1_H = int(CH * 1.35)
l1_tiled, l1_tile_w = make_h_tile(layer1_src, L1_H, CW)

# fixed reference width (scaled), independent of the wider CW -- keeps
# the same zoom level as the 4:3 version, just tiled wider
L2_FIXED_W = int(ORIG_CW * 1.35)
l2_tiled, l2_tile_h = make_v_tile(layer2_src, L2_FIXED_W, CH)

FPS = 15
N_FRAMES = 132
GLOBAL_FADE_IN = 12
GLOBAL_FADE_OUT = 13
L2_FADE_START, L2_FADE_END = 18, 45
L3_FADE_START, L3_FADE_END = 45, 74
TITLE_FADE_START, TITLE_FADE_END = 15, 33
R2_FADE_START, R2_FADE_END = 38, 54
TAKI_FADE_START, TAKI_FADE_END = 41, 58
L1_ALPHA = 0.8
L2_ALPHA = 0.85
L3_ALPHA = 1.0

def ease(t):
    t = max(0.0, min(1.0, t))
    return t * t * (3 - 2 * t)

def alpha_scale(layer_rgba, factor):
    if factor >= 0.999:
        return layer_rgba
    if factor <= 0.001:
        return Image.new("RGBA", layer_rgba.size, (0, 0, 0, 0))
    r, g, b, a = layer_rgba.split()
    a = a.point(lambda p: int(p * factor))
    return Image.merge("RGBA", (r, g, b, a))

def env(i, start, end):
    if i < start:
        return 0.0
    if i >= end:
        return 1.0
    return ease((i - start) / max(1, (end - start)))

X_OFFSET = (CW - ORIG_CW) // 2

def make_frame(i):
    t = i / (N_FRAMES - 1)
    canvas = Image.new("RGBA", (CW, CH), (0, 0, 0, 255))

    # ---- Layer 1: horizontal scroll -- EXACT proven tile-index formula ----
    off1 = int((t * l1_tile_w) % l1_tile_w)
    tile_x = -off1
    starty = (L1_H - CH) // 2
    n_tiles_1 = (CW // l1_tile_w) + 3
    for k in range(n_tiles_1):
        px = tile_x + k * l1_tile_w
        if px > CW or px + l1_tile_w < 0:
            continue
        crop = l1_tiled.crop((0, starty, l1_tile_w, starty + CH))
        crop = alpha_scale(crop, L1_ALPHA)
        canvas.alpha_composite(crop, (px, 0))

    # ---- Layer 2: vertical scroll -- same proven formula, tiled across width ----
    l2_env = env(i, L2_FADE_START, L2_FADE_END)
    if l2_env > 0:
        off2 = int((t * l2_tile_h) % l2_tile_h)
        starty2 = (2 * l2_tile_h) - off2
        n_h = (CW // L2_FIXED_W) + 3
        start_x = -(((n_h * L2_FIXED_W) - CW) // 2)
        for kx in range(n_h):
            px = start_x + kx * L2_FIXED_W
            l2_frame = l2_tiled.crop((0, starty2, L2_FIXED_W, starty2 + CH))
            l2_frame = alpha_scale(l2_frame, L2_ALPHA * l2_env)
            canvas.alpha_composite(l2_frame, (px, 0))

    # ---- Layer 3: zoom (correct aspect, based on CW not ORIG_CW) ----
    l3_env = env(i, L3_FADE_START, L3_FADE_END)
    if l3_env > 0:
        zoom_t = ease(max(0.0, min(1.0, (i - L3_FADE_START) / (N_FRAMES - 1 - L3_FADE_START))))
        scale = 0.55 + 0.95 * zoom_t
        zw, zh = max(1, int(CW * scale)), max(1, int(CH * scale))
        base_crop_w = int(layer3_src.width * 0.55)
        base_crop_h = int(base_crop_w * CH / CW)
        cx = (layer3_src.width - base_crop_w) // 2
        cy = (layer3_src.height - base_crop_h) // 2
        crop = layer3_src.crop((cx, cy, cx + base_crop_w, cy + base_crop_h))
        l3_resized = crop.resize((zw, zh), Image.LANCZOS)
        l3_frame = Image.new("RGBA", (CW, CH), (0, 0, 0, 0))
        px = (CW - zw) // 2
        py = (CH - zh) // 2
        l3_frame.paste(l3_resized, (px, py), l3_resized)
        l3_frame = alpha_scale(l3_frame, L3_ALPHA * l3_env)
        canvas = Image.alpha_composite(canvas, l3_frame)

    vig = Image.new("L", (CW, CH), 0)
    vd = ImageDraw.Draw(vig)
    vd.ellipse((-CW * 0.3, -CH * 0.5, CW * 1.3, CH * 1.5), fill=255)
    vig = vig.filter(ImageFilter.GaussianBlur(45 * SCALE))
    black = Image.new("RGBA", (CW, CH), (0, 0, 0, 255))
    canvas = Image.composite(canvas, black, vig)

    t_env = env(i, TITLE_FADE_START, TITLE_FADE_END)
    if t_env > 0:
        logo_full = Image.new("RGBA", (CW, CH), (0, 0, 0, 0))
        logo_full.paste(logo_title, (X_OFFSET, 0), logo_title)
        canvas = Image.alpha_composite(canvas, alpha_scale(logo_full, t_env))
    r2_env = env(i, R2_FADE_START, R2_FADE_END)
    if r2_env > 0:
        logo_full = Image.new("RGBA", (CW, CH), (0, 0, 0, 0))
        logo_full.paste(logo_r2, (X_OFFSET, 0), logo_r2)
        canvas = Image.alpha_composite(canvas, alpha_scale(logo_full, r2_env))
    taki_env = env(i, TAKI_FADE_START, TAKI_FADE_END)
    if taki_env > 0:
        logo_full = Image.new("RGBA", (CW, CH), (0, 0, 0, 0))
        logo_full.paste(logo_taki, (X_OFFSET, 0), logo_taki)
        canvas = Image.alpha_composite(canvas, alpha_scale(logo_full, taki_env))

    fade = 1.0
    if i < GLOBAL_FADE_IN:
        fade = ease(i / GLOBAL_FADE_IN)
    elif i > N_FRAMES - 1 - GLOBAL_FADE_OUT:
        fade = ease((N_FRAMES - 1 - i) / GLOBAL_FADE_OUT)
    if fade < 1.0:
        black_full = Image.new("RGBA", (CW, CH), (0, 0, 0, 255))
        canvas = Image.blend(black_full, canvas, fade)

    return canvas.convert("RGB")

print(f"rendering {N_FRAMES} frames at {W}x{H} (16:9, {SCALE}x resolution)")
frames = []
for i in range(N_FRAMES):
    frames.append(make_frame(i))
    if i % 20 == 0:
        print("frame", i)

OUT_PATH = "/home/claude/screensaver/intro_frames_169_v3.bin"
durations_ms = [50] * N_FRAMES
durations_ms[-1] = 350

def rgb888_to_565_bytes(img):
    px = img.load()
    w, h = img.size
    out = bytearray(w * h * 2)
    idx = 0
    for y in range(h):
        for x in range(w):
            r, g, b = px[x, y]
            v = ((r >> 3) << 11) | ((g >> 2) << 5) | (b >> 3)
            out[idx] = v & 0xFF
            out[idx + 1] = (v >> 8) & 0xFF
            idx += 2
    return bytes(out)

with open(OUT_PATH, "wb") as f:
    f.write(b"SSIF")
    f.write(struct.pack("<III", N_FRAMES, W, H))
    for d in durations_ms:
        f.write(struct.pack("<H", d))
    for idx, fr in enumerate(frames):
        f.write(rgb888_to_565_bytes(fr))

import os
print("wrote", OUT_PATH, os.path.getsize(OUT_PATH), "bytes")
