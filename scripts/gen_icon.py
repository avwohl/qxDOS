#!/usr/bin/env python3
"""Generate FreeDOS app icon with Blinky the Fish in all App Store sizes.

Blinky the Fish mascot by Bas Snabilie, CC-BY 2.5.
SVG source: commons.wikimedia.org/wiki/File:FreeDOS_logo4_2010.svg

Requires: brew install librsvg; pip3 install Pillow
"""

import os
import subprocess
import sys
import tempfile

try:
    from PIL import Image
except ImportError:
    print("Pillow not installed. Run: pip3 install Pillow")
    sys.exit(1)

SCRIPT_DIR = os.path.dirname(os.path.abspath(__file__))
ASSETS_DIR = os.path.join(SCRIPT_DIR, "..", "qxDOS", "Assets.xcassets")
ICON_DIR = os.path.join(ASSETS_DIR, "AppIcon.appiconset")
LOGO_DIR = os.path.join(ASSETS_DIR, "AppLogo.imageset")
SVG_PATH = os.path.join(SCRIPT_DIR, "blinky.svg")

# Standalone Blinky fish SVG (paths extracted from the full FreeDOS logo)
FISH_SVG = """\
<?xml version="1.0" encoding="UTF-8"?>
<svg xmlns="http://www.w3.org/2000/svg" viewBox="-400 -200 3400 2600">
  <g>
    <path d="m 1017.2002,7.23844 c -318.99996,23 -569.99996,185 -784.99996,504 -122,181.99996 -158.000003,292.99996 -226.0000033,693.99996 -5.99999999,36 -8,117 -4,203 8.0000003,157 1,141 84.0000003,195 19.000003,12 63.000003,60 98.000003,105 34,46 74,93 87,103 l 25,19 -31,67 c -48,102 -45,243 5,243 19,0 92,-53 147,-106 l 45,-44 100,39 c 356,139 780.99996,108 1110.99996,-80 l 88,-51 34,38 c 19,22 60,71 90,109 103,128 123,139 148,78 41,-96 31,-178 -42,-364 l -18,-47 69,-66 c 83,-79 73,-77 118,-18 63,84 193,180 293,218 70,26 71,2 8,-101 -116,-186 -123,-241 -45,-351 54,-78 115,-137 166,-161 41,-19 43,-32 13,-60 -51,-46 -152,-58 -235,-29 -50,18 -59,12 -72,-45 -158,-702.99996 -620,-1127.99996 -1196,-1096.99996 -17,1 -50,3 -75,5 z" fill="black"/>
    <path d="m 1402.2002,93.23844 c 169,42 296,117 421,249 223,237.99996 350,522.99996 413,925.99996 1,2 16,-9 34,-24 55,-46 166,-75 235,-59 27,5 26,7 -63,99 -155,160 -164,275 -34,458 13,17 16,28 8,28 -49,0 -265,-234 -260,-282 6,-60 -74,-18 -122,65 -14,23 -37,53 -52,67 l -27,25 -28,-34 c -26,-30 -30,-32 -49,-19 l -20,14 20,34 c 63,102 128,290 129,365 0,83 -24,82 -83,-4 -46,-69 -176,-210 -208,-226 -68,-36 -106,-12 -54,34 51,44 51,44 -76,114 -232,127 -778.99996,158 -979.99996,57 -111,-57 -287,-191 -331,-252 l -21,-31 24,6 c 13,3 65,17 116,31 205,56 773.99996,65 882.99996,14 142,-66 158,-161 18,-108 -249,95 -761.99996,67 -1067.99996,-57 -74,-30 -141.000003,-58 -149.000003,-61 -18,-7 -9,-324 13,-452 60.000003,-351 248.000003,-686.99996 469.000003,-837.99996 23,-16 59,-40 80,-55 140,-96 534.99996,-139 761.99996,-84 z M 386.20024,1931.2384 c 13,17 22,31 20,33 -44,30 -111,67 -116,62 -9,-9 14,-78 34,-103 23,-29 33,-28 62,8 z" fill="#bfcfe7"/>
    <path d="m 355.20024,1086.2384 c -119,64 -155,279 -61,365 156,145 374,40 377,-181 2,-138 -193,-251 -316,-184 z" fill="black"/>
    <path d="m 532.20024,1138.2384 c 111,69 128,154 53,259 -120,167 -362,12 -287,-184 32,-85 153,-124 234,-75 z" fill="#ffffff"/>
    <path d="m 412.20024,1179.2384 c -94,39 -79,201 19,209 44,4 62,-8 82,-56 42,-101 -15,-188 -101,-153 z m 79,44 c 6,9 7,19 3,23 -12,13 -82,-10 -85,-28 -6,-32 61,-28 82,5 z" fill="black"/>
    <path d="m 836.20024,1150.2384 c -152,46 -147,280 8,336 67,23 106,18 147,-20 132.99996,-127 14.99996,-368 -155,-316 z" fill="black"/>
    <path d="m 927.20024,1205.2384 c 87.99996,46 100.99996,145 28,222 -61,65 -178,-10 -178,-112 0,-96 73,-150 150,-110 z" fill="#ffffff"/>
    <path d="m 832.20024,1256.2384 c -43,66 -7,167 53,148 59,-19 82,-48 82,-104 0,-63 -101,-96 -135,-44 z m 85,9 c 16,19 5,34 -20,28 -24,-6 -34,-19 -25,-33 9,-14 31,-12 45,5 z" fill="black"/>
  </g>
</svg>"""

BG_COLOR = (21, 101, 192)  # Material Blue 800

# All sizes needed for iOS/macOS App Store
SIZES = [
    ("icon_1024.png", 1024),
    ("icon_180.png", 180),
    ("icon_120.png", 120),
    ("icon_167.png", 167),
    ("icon_152.png", 152),
    ("icon_76.png", 76),
    ("icon_80.png", 80),
    ("icon_87.png", 87),
    ("icon_58.png", 58),
    ("icon_40.png", 40),
    ("icon_60.png", 60),
    ("icon_20.png", 20),
    ("icon_29.png", 29),
    ("icon_1024_mac.png", 1024),
    ("icon_512.png", 512),
    ("icon_256.png", 256),
    ("icon_128.png", 128),
    ("icon_64.png", 64),
    ("icon_32.png", 32),
    ("icon_16.png", 16),
]


def render_svg(svg_content, width, height):
    """Render SVG content to a PIL Image using rsvg-convert."""
    with tempfile.NamedTemporaryFile(suffix=".svg", mode="w", delete=False) as f:
        f.write(svg_content)
        svg_tmp = f.name
    png_tmp = svg_tmp.replace(".svg", ".png")
    try:
        subprocess.run(
            ["rsvg-convert", "-w", str(width), "-h", str(height),
             "--keep-aspect-ratio", "-o", png_tmp, svg_tmp],
            check=True, capture_output=True
        )
        return Image.open(png_tmp).convert("RGBA")
    finally:
        os.unlink(svg_tmp)
        if os.path.exists(png_tmp):
            pass  # caller uses the image


def render_fish_icon(size):
    """Render Blinky on a blue background at the given size."""
    # Render fish at high res then scale
    render_size = max(size * 4, 2048)
    fish = render_svg(FISH_SVG, render_size, render_size)

    # Trim transparent area
    bbox = fish.getbbox()
    if bbox:
        fish = fish.crop(bbox)

    # Fit fish into square with padding (10% each side)
    fw, fh = fish.size
    target = int(size * 0.80)
    scale = min(target / fw, target / fh)
    new_w = int(fw * scale)
    new_h = int(fh * scale)
    fish = fish.resize((new_w, new_h), Image.LANCZOS)

    # Create blue background and paste fish centered
    icon = Image.new("RGB", (size, size), BG_COLOR)
    ox = (size - new_w) // 2
    oy = (size - new_h) // 2
    icon.paste(fish, (ox, oy), fish)  # use alpha as mask

    return icon


def render_full_logo():
    """Render the full FreeDOS logo (text + fish) for the About view."""
    # Render at 3x for quality (original is 565x96)
    logo = render_svg(open(SVG_PATH).read(), 1695, 288)
    bbox = logo.getbbox()
    if bbox:
        logo = logo.crop(bbox)
    return logo


def generate_contents_json():
    return """{
  "images" : [
    {
      "filename" : "icon_40.png",
      "idiom" : "iphone",
      "scale" : "2x",
      "size" : "20x20"
    },
    {
      "filename" : "icon_60.png",
      "idiom" : "iphone",
      "scale" : "3x",
      "size" : "20x20"
    },
    {
      "filename" : "icon_58.png",
      "idiom" : "iphone",
      "scale" : "2x",
      "size" : "29x29"
    },
    {
      "filename" : "icon_87.png",
      "idiom" : "iphone",
      "scale" : "3x",
      "size" : "29x29"
    },
    {
      "filename" : "icon_80.png",
      "idiom" : "iphone",
      "scale" : "2x",
      "size" : "40x40"
    },
    {
      "filename" : "icon_120.png",
      "idiom" : "iphone",
      "scale" : "3x",
      "size" : "40x40"
    },
    {
      "filename" : "icon_120.png",
      "idiom" : "iphone",
      "scale" : "2x",
      "size" : "60x60"
    },
    {
      "filename" : "icon_180.png",
      "idiom" : "iphone",
      "scale" : "3x",
      "size" : "60x60"
    },
    {
      "filename" : "icon_20.png",
      "idiom" : "ipad",
      "scale" : "1x",
      "size" : "20x20"
    },
    {
      "filename" : "icon_40.png",
      "idiom" : "ipad",
      "scale" : "2x",
      "size" : "20x20"
    },
    {
      "filename" : "icon_29.png",
      "idiom" : "ipad",
      "scale" : "1x",
      "size" : "29x29"
    },
    {
      "filename" : "icon_58.png",
      "idiom" : "ipad",
      "scale" : "2x",
      "size" : "29x29"
    },
    {
      "filename" : "icon_40.png",
      "idiom" : "ipad",
      "scale" : "1x",
      "size" : "40x40"
    },
    {
      "filename" : "icon_80.png",
      "idiom" : "ipad",
      "scale" : "2x",
      "size" : "40x40"
    },
    {
      "filename" : "icon_76.png",
      "idiom" : "ipad",
      "scale" : "1x",
      "size" : "76x76"
    },
    {
      "filename" : "icon_152.png",
      "idiom" : "ipad",
      "scale" : "2x",
      "size" : "76x76"
    },
    {
      "filename" : "icon_167.png",
      "idiom" : "ipad",
      "scale" : "2x",
      "size" : "83.5x83.5"
    },
    {
      "filename" : "icon_1024.png",
      "idiom" : "ios-marketing",
      "scale" : "1x",
      "size" : "1024x1024"
    },
    {
      "filename" : "icon_16.png",
      "idiom" : "mac",
      "scale" : "1x",
      "size" : "16x16"
    },
    {
      "filename" : "icon_32.png",
      "idiom" : "mac",
      "scale" : "2x",
      "size" : "16x16"
    },
    {
      "filename" : "icon_32.png",
      "idiom" : "mac",
      "scale" : "1x",
      "size" : "32x32"
    },
    {
      "filename" : "icon_64.png",
      "idiom" : "mac",
      "scale" : "2x",
      "size" : "32x32"
    },
    {
      "filename" : "icon_128.png",
      "idiom" : "mac",
      "scale" : "1x",
      "size" : "128x128"
    },
    {
      "filename" : "icon_256.png",
      "idiom" : "mac",
      "scale" : "2x",
      "size" : "128x128"
    },
    {
      "filename" : "icon_256.png",
      "idiom" : "mac",
      "scale" : "1x",
      "size" : "256x256"
    },
    {
      "filename" : "icon_512.png",
      "idiom" : "mac",
      "scale" : "2x",
      "size" : "256x256"
    },
    {
      "filename" : "icon_512.png",
      "idiom" : "mac",
      "scale" : "1x",
      "size" : "512x512"
    },
    {
      "filename" : "icon_1024_mac.png",
      "idiom" : "mac",
      "scale" : "2x",
      "size" : "512x512"
    }
  ],
  "info" : {
    "author" : "xcode",
    "version" : 1
  }
}"""


def main():
    # Generate app icons
    os.makedirs(ICON_DIR, exist_ok=True)

    unique_sizes = sorted(set(s for _, s in SIZES))
    rendered = {}
    for sz in unique_sizes:
        print(f"  Rendering icon {sz}x{sz}...")
        rendered[sz] = render_fish_icon(sz)

    for filename, sz in SIZES:
        path = os.path.join(ICON_DIR, filename)
        rendered[sz].save(path, "PNG")
        print(f"  Saved {filename} ({sz}x{sz})")

    contents_path = os.path.join(ICON_DIR, "Contents.json")
    with open(contents_path, "w") as f:
        f.write(generate_contents_json())
    print("  Saved AppIcon Contents.json")

    # Generate full FreeDOS logo for About view
    os.makedirs(LOGO_DIR, exist_ok=True)
    print("  Rendering FreeDOS logo for About view...")
    logo = render_full_logo()
    logo_path = os.path.join(LOGO_DIR, "freedos_logo.png")
    logo.save(logo_path, "PNG")
    print(f"  Saved freedos_logo.png ({logo.size[0]}x{logo.size[1]})")

    # Also save @2x version
    logo_2x = render_svg(open(SVG_PATH).read(), 3390, 576)
    bbox = logo_2x.getbbox()
    if bbox:
        logo_2x = logo_2x.crop(bbox)
    logo_2x_path = os.path.join(LOGO_DIR, "freedos_logo@2x.png")
    logo_2x.save(logo_2x_path, "PNG")
    print(f"  Saved freedos_logo@2x.png ({logo_2x.size[0]}x{logo_2x.size[1]})")

    logo_contents = """{
  "images" : [
    {
      "filename" : "freedos_logo.png",
      "idiom" : "universal",
      "scale" : "1x"
    },
    {
      "filename" : "freedos_logo@2x.png",
      "idiom" : "universal",
      "scale" : "2x"
    }
  ],
  "info" : {
    "author" : "xcode",
    "version" : 1
  }
}"""
    with open(os.path.join(LOGO_DIR, "Contents.json"), "w") as f:
        f.write(logo_contents)
    print("  Saved FreeDOSLogo Contents.json")

    # Top-level Assets.xcassets Contents.json
    top_contents = os.path.join(ASSETS_DIR, "Contents.json")
    with open(top_contents, "w") as f:
        f.write('{\n  "info" : {\n    "author" : "xcode",\n    "version" : 1\n  }\n}\n')

    print(f"\nDone! Generated {len(SIZES)} icon files + FreeDOS logo.")
    print("Blinky the Fish mascot by Bas Snabilie, CC-BY 2.5")


if __name__ == "__main__":
    main()
