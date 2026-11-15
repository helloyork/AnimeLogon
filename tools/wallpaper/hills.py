"""Soft hills -- a procedural, seamlessly looping background of soft horizontal
hills in neighbouring shades of one colour.

Every frame is a pure function of (width, height, phase), so the same script
renders the loop video at any resolution and stills at any size or aspect
(banners): the composition is laid out in units of the frame height and
centred horizontally, so wider frames show more of the same hills.

Colour work happens in OKLab; the result is converted to BT.709 Y'CbCr on the
GPU and dithered there, so 8-bit video does not band.

Run with a Python that has torch (CUDA if available, CPU works, slowly):
  python hills.py still --size 3840x2160 --t 0 --out still.png [--png16]
  python hills.py video --size 3840x2160 --fps 60 --seconds 40 --out loop.mp4
  python hills.py sheet --size 960x540 --out sheet.png   (six moments of the loop)
Every command takes --style lavender|violet|mauve|dusk (default mauve).
"""

import argparse
import math
import subprocess
import sys

import numpy as np
import torch

TAU = 2.0 * math.pi
DEV = torch.device("cuda" if torch.cuda.is_available() else "cpu")


# --------------------------------------------------------------------------
# colour helpers
# --------------------------------------------------------------------------

def hex_to_lin(h):
    h = h.lstrip("#")
    c = [int(h[i:i + 2], 16) / 255.0 for i in (0, 2, 4)]
    return [x / 12.92 if x <= 0.04045 else ((x + 0.055) / 1.055) ** 2.4 for x in c]


def lin_to_oklab(rgb):
    r, g, b = rgb
    l = 0.4122214708 * r + 0.5363325363 * g + 0.0514459929 * b
    m = 0.2119034982 * r + 0.6806995451 * g + 0.1073969566 * b
    s = 0.0883024619 * r + 0.2817188376 * g + 0.6299787005 * b
    l, m, s = (math.copysign(abs(x) ** (1 / 3), x) for x in (l, m, s))
    return [
        0.2104542553 * l + 0.7936177850 * m - 0.0040720468 * s,
        1.9779984951 * l - 2.4285922050 * m + 0.4505937099 * s,
        0.0259040371 * l + 0.7827717662 * m - 0.8086757660 * s,
    ]


def lab(h):
    return torch.tensor(lin_to_oklab(hex_to_lin(h)), device=DEV, dtype=torch.float32).view(3, 1, 1)


def oklab_to_lin(L, a, b):
    l = L + 0.3963377774 * a + 0.2158037573 * b
    m = L - 0.1055613458 * a - 0.0638541728 * b
    s = L - 0.0894841775 * a - 1.2914855480 * b
    l, m, s = l ** 3, m ** 3, s ** 3
    r = 4.0767416621 * l - 3.3077115913 * m + 0.2309699292 * s
    g = -1.2684380046 * l + 2.6097574011 * m - 0.3413193965 * s
    bb = -0.0041960863 * l - 0.7034186147 * m + 1.7076147010 * s
    return r, g, bb


def lin_to_srgb(x):
    x = x.clamp(0.0, 1.0)
    return torch.where(x <= 0.0031308, x * 12.92, 1.055 * x.pow(1 / 2.4) - 0.055)


def mix(a, b, t):
    return a + (b - a) * t



# --------------------------------------------------------------------------
# the design: soft horizontal hills in neighbouring shades of one colour
# --------------------------------------------------------------------------
# Units: v = 0 at the top edge, 1 at the bottom; u = horizontal distance from
# the centre, in frame heights.  phi = 2*pi*t/T, so anything built from
# integer multiples of phi repeats exactly every T seconds.
#
# No glow, no shadows, no outlines: depth comes only from small, even steps
# in lightness (far = lighter), and each hill carries a faint gradient.

# sky: top, horizon.  hills: far -> near.
STYLES = {
    "lavender": dict(sky=("#eee5fb", "#e2d4f6"),
                     hills=["#d8c6f3", "#ccb6ee", "#c0a6e8", "#b397e1", "#a688d9", "#9a7ad0", "#8e6dc6"]),
    "violet":   dict(sky=("#cbb7ee", "#b9a0e6"),
                     hills=["#aa8ddd", "#9e7fd6", "#9171cd", "#8564c4", "#7958ba", "#6e4daf", "#6343a3"]),
    "mauve":    dict(sky=("#f0e0f0", "#e5cdea"),
                     hills=["#dabbe6", "#ceabe0", "#c29cd9", "#b58ed1", "#a881c8", "#9b75be", "#8f69b3"]),
    "dusk":     dict(sky=("#3a2a6e", "#6650a4"),
                     hills=["#5c4796", "#533f8c", "#4a3881", "#423176", "#3a2a6b", "#322460", "#2b1e55"]),
}
STYLE = dict(STYLES["mauve"])

# back -> front.  Each wave: (amplitude, wavelength, phase, sway, sway_k, sway_phase, drift)
# A wave's phase swings by sway*sin(sway_k*phi + sway_phase), so the hills
# undulate around a fixed composition; drift slides a wave a whole number of
# wavelengths along its ridge per loop.
LAYERS = [
    dict(base=0.30, tilt=-0.04, waves=[(0.068, 2.8, 0.3, 0.7, 1, 0.0, 0), (0.027, 1.3, 2.1, 1.0, 1, 1.7, 1)]),
    dict(base=0.41, tilt=0.03, waves=[(0.075, 2.6, 1.9, 0.7, 1, 2.3, 0), (0.027, 1.2, 0.4, 1.0, 1, 4.1, -1)]),
    dict(base=0.52, tilt=-0.05, waves=[(0.078, 3.0, 4.2, 0.6, 1, 3.6, 0), (0.030, 1.4, 5.3, 0.9, 1, 0.9, 1)]),
    dict(base=0.62, tilt=0.02, waves=[(0.075, 2.6, 3.1, 0.7, 1, 5.2, 0), (0.028, 1.3, 1.6, 1.0, 1, 3.0, -1)]),
    dict(base=0.72, tilt=-0.03, waves=[(0.078, 2.9, 5.6, 0.6, 1, 1.1, 0), (0.030, 1.4, 2.9, 1.0, 1, 5.9, 1)]),
    dict(base=0.82, tilt=0.04, waves=[(0.072, 2.7, 0.9, 0.7, 1, 4.4, 0), (0.028, 1.3, 4.6, 0.9, 1, 2.2, -1)]),
    dict(base=0.92, tilt=-0.02, waves=[(0.068, 3.1, 2.4, 0.6, 1, 0.6, 0), (0.027, 1.5, 3.8, 1.0, 1, 3.7, 1)]),
]

CREST_LIFT = 0.030     # how much lighter (OKLab L) a hill is at its crest than in its body
CREST_FADE = 0.14      # how far down (frame heights) that lightness fades
EDGE_SOFT = 0.0012     # extra edge softness beyond one pixel (frame heights)


def edge_curve(layer, u, phi):
    """Ridge line y(u) and its slope dy/du for one layer."""
    y = torch.full_like(u, layer["base"]) + layer["tilt"] * u
    dy = torch.full_like(u, layer["tilt"])
    for (amp, lam, ph, sway, k, sph, drift) in layer["waves"]:
        f = TAU / lam
        theta = f * u + ph + sway * math.sin(k * phi + sph) - drift * phi
        y = y + amp * torch.sin(theta)
        dy = dy + amp * f * torch.cos(theta)
    return y, dy


def render_lab(w, h, phi):
    """Return the frame as OKLab planes (3, h, w)."""
    st = STYLE
    v = (torch.arange(h, device=DEV, dtype=torch.float32) + 0.5).view(h, 1) / h
    u = ((torch.arange(w, device=DEV, dtype=torch.float32) + 0.5) - w / 2).view(1, w) / h
    px = 1.0 / h
    vv = v.expand(h, w)

    top, horizon = lab(st["sky"][0]), lab(st["sky"][1])
    col = mix(top, horizon, (vv / 0.45).clamp(0, 1).unsqueeze(0))

    soft = px + EDGE_SOFT
    for layer, hex_ in zip(LAYERS, st["hills"]):
        y, dy = edge_curve(layer, u, phi)
        d = (vv - y) * torch.rsqrt(1.0 + dy * dy)      # >0 below the ridge line
        base = lab(hex_)
        lift = CREST_LIFT * torch.exp(-d.clamp(min=0) / CREST_FADE)
        body = torch.cat([base[0:1] + lift.unsqueeze(0), base[1:3].expand(2, h, w)], 0)
        a = ((d + 0.5 * soft) / soft).clamp(0, 1).unsqueeze(0)
        col = mix(col, body, a)
    return col


def lab_to_rgb_lin(col):
    r, g, b = oklab_to_lin(col[0], col[1], col[2])
    return torch.stack([r, g, b], 0).clamp(0, 1)


def to_srgb8(col, gen):
    s = lin_to_srgb(lab_to_rgb_lin(col)) * 255.0
    n = torch.rand(s.shape, device=DEV, generator=gen) - torch.rand(s.shape, device=DEV, generator=gen)
    return (s + n).round().clamp(0, 255).to(torch.uint8)


def to_srgb16(col, gen):
    s = lin_to_srgb(lab_to_rgb_lin(col)) * 65535.0
    n = torch.rand(s.shape, device=DEV, generator=gen) - torch.rand(s.shape, device=DEV, generator=gen)
    return (s + n).round().clamp(0, 65535).to(torch.int32)


def to_yuv420(col, gen):
    """BT.709 limited-range Y'CbCr 4:2:0, dithered before rounding."""
    s = lin_to_srgb(lab_to_rgb_lin(col))
    r, g, b = s[0], s[1], s[2]
    Y = 0.2126 * r + 0.7152 * g + 0.0722 * b
    Cb = (b - Y) / 1.8556
    Cr = (r - Y) / 1.5748
    h, w = Y.shape
    Cb = Cb.view(h // 2, 2, w // 2, 2).mean((1, 3))
    Cr = Cr.view(h // 2, 2, w // 2, 2).mean((1, 3))

    def q(x, scale, off):
        x = x * scale + off
        n = torch.rand(x.shape, device=DEV, generator=gen) - torch.rand(x.shape, device=DEV, generator=gen)
        return (x + n).round().clamp(16, 235 if off == 16 else 240).to(torch.uint8)

    return q(Y, 219.0, 16), q(Cb, 224.0, 128), q(Cr, 224.0, 128)


# --------------------------------------------------------------------------
# commands
# --------------------------------------------------------------------------

def parse_size(s):
    w, h = s.lower().split("x")
    return int(w), int(h)


def save_png(path, col, sixteen, seed=1):
    from PIL import Image
    gen = torch.Generator(device=DEV)
    gen.manual_seed(seed)
    if sixteen:
        arr = to_srgb16(col, gen).permute(1, 2, 0).cpu().numpy().astype(np.uint16)
        # Pillow cannot write 16-bit RGB; go through a raw PNG writer
        write_png16(path, arr)
    else:
        arr = to_srgb8(col, gen).permute(1, 2, 0).cpu().numpy()
        Image.fromarray(arr, "RGB").save(path, optimize=True)


def write_png16(path, arr):
    import struct
    import zlib
    h, w, _ = arr.shape
    raw = arr.astype(">u2").tobytes()
    stride = w * 6
    rows = b"".join(b"\x00" + raw[y * stride:(y + 1) * stride] for y in range(h))

    def chunk(tag, data):
        c = struct.pack(">I", len(data)) + tag + data
        return c + struct.pack(">I", zlib.crc32(tag + data) & 0xFFFFFFFF)

    with open(path, "wb") as f:
        f.write(b"\x89PNG\r\n\x1a\n")
        f.write(chunk(b"IHDR", struct.pack(">IIBBBBB", w, h, 16, 2, 0, 0, 0)))
        f.write(chunk(b"sRGB", b"\x00"))
        f.write(chunk(b"IDAT", zlib.compress(rows, 6)))
        f.write(chunk(b"IEND", b""))


def cmd_still(a):
    w, h = parse_size(a.size)
    col = render_lab(w, h, TAU * (a.t / a.period))
    save_png(a.out, col, a.png16)
    print(a.out)


def cmd_sheet(a):
    from PIL import Image
    w, h = parse_size(a.size)
    gen = torch.Generator(device=DEV)
    gen.manual_seed(1)
    tiles = []
    for k in range(6):
        col = render_lab(w, h, TAU * k / 6)
        tiles.append(to_srgb8(col, gen).permute(1, 2, 0).cpu().numpy())
    sheet = np.concatenate([np.concatenate(tiles[0:2], 1), np.concatenate(tiles[2:4], 1),
                            np.concatenate(tiles[4:6], 1)], 0)
    Image.fromarray(sheet, "RGB").save(a.out)
    print(a.out)


def cmd_video(a):
    w, h = parse_size(a.size)
    n = int(round(a.fps * a.seconds))
    gop = 4 * a.fps
    if n % gop:
        gop = n
    args = [
        "ffmpeg", "-hide_banner", "-loglevel", "error", "-y",
        "-f", "rawvideo", "-pix_fmt", "yuv420p", "-s", f"{w}x{h}", "-r", str(a.fps),
        # what the piped frames are; tagged on the input so every field reaches the file
        "-color_primaries", "bt709", "-color_trc", "bt709", "-colorspace", "bt709", "-color_range", "tv",
        "-i", "-",
        "-c:v", "libx264", "-preset", a.preset, "-crf", str(a.crf), "-tune", "film",
        "-x264-params", "aq-mode=3:aq-strength=0.9:deblock=-1,-1",
        "-pix_fmt", "yuv420p", "-profile:v", "high", "-level", a.level,
        # a keyframe every 4 s; the loop length is a multiple, so frame 0 (the loop point) is one
        "-g", str(gop), "-keyint_min", str(gop), "-sc_threshold", "0",
        "-movflags", "+faststart", a.out,
    ]
    p = subprocess.Popen(args, stdin=subprocess.PIPE)
    gen = torch.Generator(device=DEV)
    gen.manual_seed(7)
    for k in range(n):
        col = render_lab(w, h, TAU * k / n)
        Y, U, V = to_yuv420(col, gen)
        p.stdin.write(Y.cpu().numpy().tobytes())
        p.stdin.write(U.cpu().numpy().tobytes())
        p.stdin.write(V.cpu().numpy().tobytes())
        if k % 120 == 0:
            print(f"frame {k}/{n}", file=sys.stderr, flush=True)
    p.stdin.close()
    sys.exit(p.wait())


def main():
    ap = argparse.ArgumentParser()
    sub = ap.add_subparsers(dest="cmd", required=True)
    s = sub.add_parser("still")
    s.add_argument("--size", default="3840x2160")
    s.add_argument("--t", type=float, default=0.0, help="seconds into the loop")
    s.add_argument("--period", type=float, default=40.0)
    s.add_argument("--png16", action="store_true")
    s.add_argument("--out", required=True)
    s = sub.add_parser("sheet")
    s.add_argument("--size", default="960x540")
    s.add_argument("--out", required=True)
    s = sub.add_parser("video")
    s.add_argument("--size", default="3840x2160")
    s.add_argument("--fps", type=int, default=60)
    s.add_argument("--seconds", type=float, default=40.0)
    s.add_argument("--crf", type=float, default=15)
    s.add_argument("--preset", default="slow")
    s.add_argument("--level", default="5.2")
    s.add_argument("--out", required=True)
    for sp in sub.choices.values():
        sp.add_argument("--style", default="mauve", choices=sorted(STYLES))
    a = ap.parse_args()
    STYLE.clear()
    STYLE.update(STYLES[a.style])
    with torch.no_grad():
        {"still": cmd_still, "sheet": cmd_sheet, "video": cmd_video}[a.cmd](a)


if __name__ == "__main__":
    main()
