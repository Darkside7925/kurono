# Original procedural wallpapers for Kurono (no third-party imagery).
# Renders at 2x and downsamples for smooth edges. Output: mist.png, peaks.png (960x540).
import numpy as np
from PIL import Image, ImageFilter

W, H = 1920, 1080
rng = np.random.default_rng(7)

def fbm1d(n, octaves, seed, base=1.0):
    r = np.random.default_rng(seed)
    x = np.linspace(0, 1, n)
    out = np.zeros(n)
    amp, freq = 1.0, base
    for _ in range(octaves):
        k = int(freq * 8) + 2
        pts = r.uniform(-1, 1, k)
        xs = np.linspace(0, 1, k)
        out += amp * np.interp(x, xs, pts)
        amp *= 0.5
        freq *= 2.0
    return out / 1.9

def noise2d(h, w, scale, seed):
    r = np.random.default_rng(seed)
    small = r.uniform(0, 1, (max(2, h // scale), max(2, w // scale))).astype(np.float32)
    img = Image.fromarray((small * 255).astype(np.uint8)).resize((w, h), Image.BICUBIC)
    return np.asarray(img).astype(np.float32) / 255.0

def lerp(a, b, t):
    return a + (b - a) * t

def sky(top, bottom, glow_center=None, glow_col=None, glow_r=0.5, glow_a=0.0):
    y = np.linspace(0, 1, H)[:, None, None]
    img = lerp(np.array(top, np.float32), np.array(bottom, np.float32), y ** 1.2)
    img = np.broadcast_to(img, (H, W, 3)).copy()
    if glow_center is not None:
        yy, xx = np.mgrid[0:H, 0:W].astype(np.float32)
        cx, cy = glow_center
        d = np.sqrt(((xx / W - cx) * 1.6) ** 2 + (yy / H - cy) ** 2)
        g = np.clip(1 - d / glow_r, 0, 1) ** 2.2 * glow_a
        img = img + g[..., None] * (np.array(glow_col, np.float32) - img)
    return img

def ridge_layer(img, ybase, amp, octaves, seed, color, fog_col, fog_amt, soften=0):
    prof = fbm1d(W, octaves, seed)
    ridge = (ybase + prof * amp) * H
    yy = np.arange(H)[:, None]
    mask = (yy >= ridge[None, :]).astype(np.float32)
    if soften:
        m = Image.fromarray((mask * 255).astype(np.uint8)).filter(ImageFilter.GaussianBlur(soften))
        mask = np.asarray(m).astype(np.float32) / 255.0
    col = lerp(np.array(color, np.float32), np.array(fog_col, np.float32), fog_amt)
    # slight vertical shading inside the layer
    shade = np.clip((yy - ridge[None, :]) / (0.35 * H), 0, 1)[..., None]
    layer = lerp(col, col * 0.82, shade)
    return img * (1 - mask[..., None]) + layer * mask[..., None]

def fog_band(img, ycenter, height, col, alpha, seed):
    yy = np.linspace(0, 1, H)[:, None]
    band = np.exp(-((yy - ycenter) / height) ** 2)
    n = noise2d(H, W, 90, seed) * 0.6 + noise2d(H, W, 30, seed + 1) * 0.4
    a = np.clip(band * (0.55 + 0.6 * n) * alpha, 0, 1)[..., None]
    return img * (1 - a) + np.array(col, np.float32) * a

def grain(img, amt, seed):
    r = np.random.default_rng(seed)
    return img + r.normal(0, amt, img.shape[:2])[..., None]

def save(img, name):
    img = np.clip(img, 0, 255).astype(np.uint8)
    im = Image.fromarray(img, 'RGB').resize((960, 540), Image.LANCZOS)
    im.save(name)

# --- Mist: cool blue-green misty hills, soft light top right ---
img = sky((20, 28, 34), (88, 104, 108), glow_center=(0.72, 0.18), glow_col=(196, 206, 200), glow_r=0.75, glow_a=0.55)
fog = (150, 164, 162)
img = ridge_layer(img, 0.48, 0.10, 6, 11, (62, 78, 80), fog, 0.62, soften=2)
img = fog_band(img, 0.52, 0.10, fog, 0.55, 21)
img = ridge_layer(img, 0.58, 0.09, 6, 12, (44, 58, 58), fog, 0.42, soften=2)
img = fog_band(img, 0.63, 0.08, fog, 0.45, 22)
img = ridge_layer(img, 0.68, 0.08, 7, 13, (28, 40, 40), fog, 0.22, soften=1)
img = fog_band(img, 0.74, 0.07, (120, 134, 132), 0.35, 23)
img = ridge_layer(img, 0.80, 0.07, 7, 14, (14, 22, 22), fog, 0.05, soften=1)
img = grain(img, 2.2, 31)
save(img, 'mist.png')

# --- Peaks: dusk mountains, dark slate to a low warm glow ---
img = sky((14, 16, 26), (120, 92, 84), glow_center=(0.35, 0.72), glow_col=(214, 150, 112), glow_r=0.8, glow_a=0.6)
haze = (132, 106, 100)
img = ridge_layer(img, 0.42, 0.20, 5, 41, (70, 64, 78), haze, 0.55, soften=1)
img = fog_band(img, 0.60, 0.06, haze, 0.35, 51)
img = ridge_layer(img, 0.55, 0.16, 6, 42, (46, 42, 56), haze, 0.35, soften=1)
img = ridge_layer(img, 0.68, 0.11, 7, 43, (28, 26, 36), haze, 0.15, soften=1)
img = ridge_layer(img, 0.82, 0.07, 7, 44, (12, 12, 18), haze, 0.0, soften=1)
img = grain(img, 2.0, 61)
save(img, 'peaks.png')
print('ok')
