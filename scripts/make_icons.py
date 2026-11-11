#!/usr/bin/env python3
"""生成「压索 Yasuo Compressor」品牌图标。

设计语言：三条自上而下逐级收窄的数据层，被一道亮青色束带收紧 —— 直观表达「压缩」。
所有尺寸独立绘制（而非简单缩放），保证 16x16 任务栏图标依然清晰。

用法: python scripts/make_icons.py
输出: src/resources/{logo.svg,app.png,logo.png,tray.png,app.ico,banner.png}
"""
import io
import struct
from pathlib import Path
from PIL import Image, ImageDraw, ImageOps, ImageFilter, ImageFont

ROOT = Path(__file__).resolve().parent.parent
OUT = ROOT / "src" / "resources"
OUT.mkdir(parents=True, exist_ok=True)

BLUE_A = (38, 92, 255)      # #245CFF
BLUE_B = (16, 186, 205)     # #10BACD
CYAN = (128, 238, 255)      # #80EEFF
WHITE = (255, 255, 255)
DARK = (14, 48, 96)

SVG = '''<?xml version="1.0" encoding="UTF-8"?>
<svg xmlns="http://www.w3.org/2000/svg" viewBox="0 0 256 256" width="256" height="256">
  <title>压索 Yasuo Compressor</title>
  <desc>三条逐级收窄的数据层被一道束带收紧，表达压缩</desc>
  <defs>
    <linearGradient id="bg" x1="0" y1="0" x2="1" y2="1">
      <stop offset="0" stop-color="#245CFF"/>
      <stop offset="1" stop-color="#10BACD"/>
    </linearGradient>
  </defs>
  <rect x="0" y="0" width="256" height="256" rx="56" fill="url(#bg)"/>
  <rect x="62" y="62" width="132" height="15" rx="7" fill="#FFFFFF" opacity="0.96"/>
  <rect x="82" y="93" width="92" height="13" rx="6" fill="#FFFFFF" opacity="0.86"/>
  <rect x="102" y="122" width="52" height="11" rx="5" fill="#FFFFFF" opacity="0.76"/>
  <path d="M124 145 l-14 16 h28 z" fill="#FFFFFF" opacity="0.9"/>
  <rect x="56" y="164" width="144" height="18" rx="9" fill="#80EEFF"/>
  <rect x="120" y="161" width="16" height="24" rx="4" fill="#0E3060"/>
</svg>
'''


def lerp(a, b, t):
    return tuple(int(round(a[i] + (b[i] - a[i]) * t)) for i in range(3))


def background(size):
    grad = ImageOps.colorize(Image.linear_gradient("L"), black=BLUE_A, white=BLUE_B)
    grad = grad.resize((size, size), Image.Resampling.BICUBIC).convert("RGBA")
    # 轻微高光，增强立体感但不破坏扁平风格
    glow = Image.new("RGBA", (size, size), (0, 0, 0, 0))
    gd = ImageDraw.Draw(glow)
    r = int(size * 0.62)
    gd.ellipse((-r // 3, -r // 2, r, r // 2), fill=(255, 255, 255, 34))
    glow = glow.filter(ImageFilter.GaussianBlur(size * 0.12))
    grad = Image.alpha_composite(grad, glow)
    mask = Image.new("L", (size, size), 0)
    ImageDraw.Draw(mask).rounded_rectangle(
        (0, 0, size - 1, size - 1), radius=int(size * 0.22), fill=255)
    out = Image.new("RGBA", (size, size), (0, 0, 0, 0))
    out.paste(grad, (0, 0), mask)
    return out


def draw_icon(size):
    """按尺寸自适应细节层级，小尺寸自动简化。"""
    S = max(64, size * 4)  # 超采样绘制，再缩到目标尺寸
    img = background(S)
    # 半透明元素必须画在独立图层再合成，否则会直接覆盖背景像素
    overlay = Image.new("RGBA", (S, S), (0, 0, 0, 0))
    d = ImageDraw.Draw(overlay, "RGBA")
    u = S / 256.0

    def rr(x, y, w, h, radius, fill):
        d.rounded_rectangle(
            (int(x * u), int(y * u), int((x + w) * u), int((y + h) * u)),
            radius=int(radius * u), fill=fill)

    # 三层逐级收窄的数据
    rr(62, 62, 132, 15, 7, (*WHITE, 245))
    rr(82, 93, 92, 13, 6, (*WHITE, 220))
    rr(102, 122, 52, 11, 5, (*WHITE, 195))

    if S >= 256:  # 足够大时画出向下汇聚的箭头
        d.polygon([(int(110 * u), int(143 * u)), (int(146 * u), int(143 * u)),
                   (int(128 * u), int(160 * u))], fill=(*WHITE, 225))

    # 束带与拉链头
    rr(56, 164, 144, 18, 9, (*CYAN, 255))
    rr(120, 161, 16, 24, 4, (*DARK, 255))
    img = Image.alpha_composite(img, overlay)
    return img.resize((size, size), Image.Resampling.LANCZOS)


def make_banner():
    w, h = 960, 260
    img = Image.new("RGBA", (w, h), (0, 0, 0, 0))
    grad = ImageOps.colorize(Image.linear_gradient("L"),
                             black=(18, 30, 66), white=(16, 92, 132))
    grad = grad.resize((w, h), Image.Resampling.BICUBIC).convert("RGBA")
    mask = Image.new("L", (w, h), 0)
    ImageDraw.Draw(mask).rounded_rectangle((0, 0, w - 1, h - 1), radius=28, fill=255)
    img.paste(grad, (0, 0), mask)
    icon = draw_icon(180)
    img.alpha_composite(icon, (40, 40))
    d = ImageDraw.Draw(img)
    cjk = None
    for candidate in ("C:/Windows/Fonts/msyh.ttc", "C:/Windows/Fonts/simhei.ttf",
                      "/usr/share/fonts/truetype/noto/NotoSansCJK-Regular.ttc"):
        if Path(candidate).exists():
            cjk = candidate
            break
    if cjk:
        f_title = ImageFont.truetype(cjk, 34)
        f_sub = ImageFont.truetype(cjk, 20)
        d.text((250, 74), "压索  YASUO COMPRESSOR", font=f_title, fill=(255, 255, 255))
        d.text((250, 122), "开源 · 免费 · 全格式压缩", font=f_sub, fill=(190, 220, 240))
        d.text((250, 160), "RAR 只解压不生成 · AES-256 加密 · 多线程加速",
               font=f_sub, fill=(150, 190, 220))
    else:
        d.text((250, 78), "YASUO COMPRESSOR", fill=(255, 255, 255))
        d.text((250, 118), "Open source / Free / All formats", fill=(190, 220, 240))
        d.text((250, 152), "RAR extract only - AES-256 - Multi-threaded", fill=(150, 190, 220))
    return img


def make_ico(sizes):
    """手工构造 ICONDIR，每帧用独立绘制的 PNG，保证全部尺寸清晰。"""
    blobs = []
    for s in sizes:
        buf = io.BytesIO()
        draw_icon(s).save(buf, format="PNG")
        blobs.append(buf.getvalue())
    header = struct.pack("<HHH", 0, 1, len(sizes))
    offset = 6 + 16 * len(sizes)
    entries = []
    for s, blob in zip(sizes, blobs):
        dim = 0 if s >= 256 else s  # ICO 中 0 表示 256
        entries.append(struct.pack("<BBBBHHII", dim, dim, 0, 0, 1, 32,
                                   len(blob), offset))
        offset += len(blob)
    return header + b"".join(entries) + b"".join(blobs)


def main():
    (OUT / "logo.svg").write_text(SVG, encoding="utf-8")
    for size, name in [(256, "app.png"), (64, "logo.png"), (32, "tray.png")]:
        draw_icon(size).save(OUT / name)

    sizes = [16, 24, 32, 48, 64, 128, 256]
    (OUT / "app.ico").write_bytes(make_ico(sizes))

    make_banner().save(OUT / "banner.png")
    print("generated:", ", ".join(sorted(p.name for p in OUT.iterdir())))
    print("ico sizes:", sizes)


if __name__ == "__main__":
    main()
