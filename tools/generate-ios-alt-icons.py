#!/usr/bin/env python3
import argparse
import plistlib
from pathlib import Path


ICON_SIZES = (120, 152, 167)


def load_icon_names(plist_path):
    with open(plist_path, "rb") as plist:
        icons = plistlib.load(plist)
    return [name for name in sorted(icons.keys()) if name]


def write_info_header(icon_names, header_path):
    def icon_entry(name):
        files = "".join(f"<string>{name}-{size}</string>" for size in ICON_SIZES)
        return (
            f"<key>{name}</key>"
            "<dict>"
            "<key>CFBundleIconFiles</key>"
            f"<array>{files}</array>"
            "</dict>"
        )

    alt_icons = "<key>CFBundleAlternateIcons</key><dict>"
    alt_icons += "".join(icon_entry(name) for name in icon_names)
    alt_icons += "</dict>"
    icon_stuff = (
        "</string>"
        f"<key>CFBundleIcons</key><dict>{alt_icons}</dict>"
        f"<key>CFBundleIcons~ipad</key><dict>{alt_icons}</dict>"
        "<key>kxcode</key><string>"
    )

    header_path.parent.mkdir(parents=True, exist_ok=True)
    header_path.write_text("#define ICON_STUFF " + icon_stuff, encoding="utf-8")

def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--icons-plist", required=True, type=Path)
    parser.add_argument("--info-header", type=Path)
    parser.add_argument("--icons-dir", type=Path, help=argparse.SUPPRESS)
    parser.add_argument("--resources-dir", type=Path, help=argparse.SUPPRESS)
    args = parser.parse_args()

    icon_names = load_icon_names(args.icons_plist)
    if args.info_header is not None:
        write_info_header(icon_names, args.info_header)


if __name__ == "__main__":
    main()
