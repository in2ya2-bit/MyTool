"""
Generate a placeholder map_clean.png for Erangel.
Creates a simple top-down map with:
  - Blue ocean (HSV ~210, high sat, low val)
  - Green/brown land masses roughly matching Erangel's shape
  - Cyan inland water areas
"""
import numpy as np
from PIL import Image, ImageDraw

W, H = 1024, 1024

img = Image.new('RGB', (W, H), (30, 50, 110))  # ocean blue (dark)
draw = ImageDraw.Draw(img)

# Main island (rough Erangel shape — elongated NW-SE)
main_land = [
    (180, 100), (350, 60), (550, 80), (700, 120), (800, 200),
    (850, 350), (880, 500), (860, 650), (800, 720), (700, 750),
    (600, 770), (500, 800), (400, 780), (300, 720), (200, 600),
    (160, 450), (140, 300), (150, 180),
]
draw.polygon(main_land, fill=(120, 140, 80))  # green-brown land

# Southern military island
mil_island = [
    (350, 830), (420, 810), (550, 820), (620, 850),
    (640, 900), (600, 940), (500, 960), (380, 950),
    (330, 910), (320, 870),
]
draw.polygon(mil_island, fill=(110, 130, 75))

# Bridge connections
draw.line([(420, 800), (420, 830)], fill=(160, 160, 140), width=6)
draw.line([(560, 800), (560, 820)], fill=(160, 160, 140), width=6)

# Inland water (Erangel's small lake near Stalber)
draw.ellipse([650, 180, 720, 240], fill=(40, 130, 140))  # cyan-ish

# Small river-like feature in center
river_pts = [(350, 400), (380, 450), (400, 520), (380, 580), (350, 620)]
draw.line(river_pts, fill=(35, 120, 135), width=8)

# Add terrain variation with noise-like patches
np.random.seed(42)
pixels = np.array(img)

# Add slight noise to land areas to make it more realistic
for y in range(H):
    for x in range(W):
        r, g, b = int(pixels[y, x, 0]), int(pixels[y, x, 1]), int(pixels[y, x, 2])
        if g > 100 and r > 80:  # land pixel
            noise = np.random.randint(-15, 15)
            pixels[y, x, 0] = max(0, min(255, r + noise))
            pixels[y, x, 1] = max(0, min(255, g + noise))
            pixels[y, x, 2] = max(0, min(255, b + noise // 2))

img = Image.fromarray(pixels)

out_dir = r"E:\EEE\Shot1\S1\Plugins\LevelTool\Content\RefMaps\BR\Erangel"
img.save(f"{out_dir}\\map_clean.png")
print(f"Saved placeholder map_clean.png to {out_dir}")
print(f"Image size: {W}x{H}")
