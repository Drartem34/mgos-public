"""Іконка MgShare.ico (зелений квадрат із телефоном) — малюється під час збірки, без бінарників у git."""
from PIL import Image, ImageDraw

def frame(n):
    s = n / 64
    im = Image.new("RGBA", (n, n), (0, 0, 0, 0)); d = ImageDraw.Draw(im)
    d.rounded_rectangle((2 * s, 2 * s, 62 * s, 62 * s), radius=15 * s, fill=(48, 209, 88))
    d.rounded_rectangle((21 * s, 11 * s, 43 * s, 53 * s), radius=5 * s, fill="white")
    d.rounded_rectangle((24 * s, 16 * s, 40 * s, 46 * s), radius=2 * s, fill=(48, 209, 88))
    d.ellipse((30 * s, 47.5 * s, 34 * s, 51.5 * s), fill="white")
    return im

frame(256).save("MgShare.ico", sizes=[(16, 16), (24, 24), (32, 32), (48, 48), (64, 64), (128, 128), (256, 256)])
