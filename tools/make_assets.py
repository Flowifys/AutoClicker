"""Regenerate res/autoclicker.ico and res/background.bmp from the source images.

    pip install pillow
    python tools/make_assets.py
"""
from pathlib import Path
from PIL import Image, ImageDraw

RES = Path(__file__).resolve().parent.parent / "res"

# Icon: make the black background transparent, square it up, save a multi-size .ico
im = Image.open(RES / "autoclickericon.png").convert("RGBA")
w, h = im.size
tmp = im.convert("RGB")
for corner in [(0, 0), (w - 1, 0), (0, h - 1), (w - 1, h - 1)]:
    ImageDraw.floodfill(tmp, corner, (255, 0, 255), thresh=40)
px, pt = im.load(), tmp.load()
for y in range(h):
    for x in range(w):
        if pt[x, y] == (255, 0, 255):
            px[x, y] = (0, 0, 0, 0)
im = im.crop(im.getbbox())
cw, ch = im.size
side = int(max(cw, ch) * 1.06)
sq = Image.new("RGBA", (side, side), (0, 0, 0, 0))
sq.paste(im, ((side - cw) // 2, (side - ch) // 2))
sq = sq.resize((256, 256), Image.LANCZOS)
sq.save(RES / "autoclicker.ico", sizes=[(s, s) for s in (16, 20, 24, 32, 40, 48, 64, 128, 256)])

# Background: crop to the window's shape (460x474) at 1.5x for sharp high-DPI scaling
bg = Image.open(RES / "autoclickerbg.jpg").convert("RGB")
W, H, scale = 460, 474, 1.5
bw = int(bg.height * W / H)
x0 = 700
bg.crop((x0, 0, x0 + bw, bg.height)).resize((int(W * scale), int(H * scale)), Image.LANCZOS).save(RES / "background.bmp")
print("assets written to", RES)
