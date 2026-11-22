#!/usr/bin/env python3
"""The repository's social preview (1280x640, GitHub's size): the icon, the name and one line. Needs Pillow.
Writes docs/screenshots/social-preview.png. Upload it under the repository's Settings, General, Social preview."""
from pathlib import Path
from PIL import Image, ImageDraw, ImageFont

root = Path(__file__).resolve().parents[2]
W, H = 1280, 640
NAVY, CREAM, ORANGE, MUTED = (13, 34, 58), (240, 236, 224), (242, 90, 16), (150, 166, 186)
fonts = root / "browser" / "fonts"

img = Image.new("RGB", (W, H), NAVY)
d = ImageDraw.Draw(img)

icon = Image.open(root / "browser" / "branding" / "robin-icon-1024.png").convert("RGB").resize((360, 360), Image.LANCZOS)
mask = Image.new("L", (1440, 1440), 0)
ImageDraw.Draw(mask).rounded_rectangle((0, 0, 1439, 1439), radius=320, fill=255)
mask = mask.resize((360, 360), Image.LANCZOS)
img.paste(icon, (110, 140), mask)
d.rounded_rectangle((109, 139, 470, 500), radius=80, outline=(40, 62, 90), width=2)

x = 540
d.text((x, 168), "Robin", font=ImageFont.truetype(str(fonts / "Manrope-Bold.ttf"), 128), fill=CREAM)
sub = ImageFont.truetype(str(fonts / "Manrope-SemiBold.ttf"), 40)
d.text((x, 330), "A web browser for LG webOS TVs,", font=sub, fill=CREAM)
d.text((x, 382), "built for the remote.", font=sub, fill=CREAM)
small = ImageFont.truetype(str(fonts / "Manrope-Regular.ttf"), 28)
d.rectangle((x, 462, x + 64, 468), fill=ORANGE)
d.text((x, 490), "WPE WebKit 2.54.3  ·  ad blocker  ·  hardware video", font=small, fill=MUTED)
d.text((x, 530), "Based on Wren by sostk", font=small, fill=MUTED)

out = root / "docs" / "screenshots" / "social-preview.png"
img.save(out, optimize=True)
print(out, out.stat().st_size)
