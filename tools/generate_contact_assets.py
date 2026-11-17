#!/usr/bin/env python3
"""Generate firmware-only contact details and a BGR565 portrait from local inputs.

contact_details.txt is a committed, deliberately non-private sample.
contact_details.private.txt, when present, overrides it for LOCAL builds only.
The generated headers stay under build/ and must not be committed.
No contact values are written to build logs.
"""
from pathlib import Path
import json
import struct
import sys
import zlib


def get_details(root: Path) -> tuple[dict[str, str], bool]:
    private = root / "contact_details.private.txt"
    path = private if private.is_file() else root / "contact_details.txt"
    if not path.is_file():
        raise ValueError("Missing contact_details.txt (the public placeholder)")
    data: dict[str, str] = {}
    for line in path.read_text(encoding="utf-8-sig").splitlines():
        line = line.strip()
        if not line or line.startswith("#"):
            continue
        if ":" in line:
            key, value = line.split(":", 1)
        elif "=" in line:
            key, value = line.split("=", 1)
        else:
            raise ValueError("Malformed contact line; use XMPP: value, Email: value or SMS: value")
        key = key.strip().lower()
        if key not in {"xmpp", "email", "sms"}:
            raise ValueError("Unexpected contact key (allowed: XMPP, Email, SMS)")
        value = value.strip()
        if "\x00" in value or len(value.encode("utf-8")) > 64:
            raise ValueError("A contact value is invalid or longer than 64 UTF-8 bytes")
        data[key] = value
    return data, private.is_file()


def get_face(root: Path, out_w: int = 128, out_h: int = 165) -> list[int]:
    data = (root / "face.bmp").read_bytes()
    if len(data) < 54 or data[:2] != b"BM":
        raise ValueError("face.bmp must be a Windows BMP (BM signature)")
    offset = struct.unpack_from("<I", data, 10)[0]
    dib = struct.unpack_from("<I", data, 14)[0]
    w, height = struct.unpack_from("<ii", data, 18)
    planes, bpp = struct.unpack_from("<HH", data, 26)
    compression = struct.unpack_from("<I", data, 30)[0]
    if (w <= 0 or w > 2048 or not 1 <= abs(height) <= 2048
            or dib < 40 or dib > 124 or planes != 1 or offset < 54
            or not ((bpp == 24 and compression == 0)
                    or (bpp == 32 and compression in (0, 3, 6)))):
        raise ValueError("Unsupported face.bmp: expected 24-bit RGB or standard 32-bit BGRA BMP")
    if bpp == 32 and compression != 0:
        if len(data) < 66 or struct.unpack_from("<III", data, 54) != (
                0x00FF0000, 0x0000FF00, 0x000000FF):
            raise ValueError("Unsupported face.bmp 32-bit channel masks")
    pixel_bytes = bpp // 8
    stride = (w * pixel_bytes + 3) & ~3
    if offset + stride * abs(height) > len(data):
        raise ValueError("face.bmp is incomplete (pixel data truncated)")

    # Aspect-preserving center crop to the actual 140x180 display viewport.
    h = abs(height)
    crop_w, crop_h = w, h
    if w * out_h > h * out_w:
        crop_w = max(1, h * out_w // out_h)
    else:
        crop_h = max(1, w * out_h // out_w)
    crop_x, crop_y = (w - crop_w) // 2, (h - crop_h) // 2
    pixels: list[int] = []
    for y in range(out_h):
        sy = crop_y + y * crop_h // out_h
        disk_y = h - 1 - sy if height > 0 else sy
        row_start = offset + disk_y * stride
        for x in range(out_w):
            sx = crop_x + x * crop_w // out_w
            i = row_start + sx * pixel_bytes
            b, g, r = data[i:i + 3]
            # Display is configured in BGR order: source red/blue swapped.
            pixels.append(((b & 0xF8) << 8) | ((g & 0xFC) << 3) | (r >> 3))
    return pixels


def main() -> None:
    if len(sys.argv) != 3:
        raise SystemExit("Usage: generate_contact_assets.py REPO_ROOT BUILD_DIR")
    root, out = Path(sys.argv[1]), Path(sys.argv[2])
    out.mkdir(parents=True, exist_ok=True)
    details, using_private = get_details(root)
    pairs = [details.get(k, "") for k in ("xmpp", "email", "sms")]
    digest = zlib.crc32("\x00".join(pairs).encode("utf-8"))
    header = (
        "#pragma once\n"
        "/* Auto-generated. Local private values must never be committed. */\n"
        f"#define CLOCK_CONTACT_HASH 0x{digest:08x}u\n"
        f"#define CLOCK_CONTACT_XMPP {json.dumps(pairs[0], ensure_ascii=False)}\n"
        f"#define CLOCK_CONTACT_EMAIL {json.dumps(pairs[1], ensure_ascii=False)}\n"
        f"#define CLOCK_CONTACT_SMS {json.dumps(pairs[2], ensure_ascii=False)}\n"
    )
    (out / "contact_defaults.h").write_text(header, encoding="utf-8")
    pixels = get_face(root)
    sections = []
    for i in range(0, len(pixels), 12):
        sections.append("    " + ", ".join(f"0x{v:04x}" for v in pixels[i:i + 12]) + ",")
    image_header = (
        "#pragma once\n#include <stdint.h>\n"
        "/* Auto-generated from root face.bmp; RGB565 picture stored in flash. */\n"
        "static const uint16_t clock_builtin_face[MEDIA_BMP_WIDTH * MEDIA_BMP_HEIGHT] = {\n"
        + "\n".join(sections) + "\n};\n"
    )
    (out / "embedded_face.h").write_text(image_header, encoding="utf-8")
    print(f"Generated built-in contact data ({'LOCAL PRIVATE override' if using_private else 'public placeholder'}) "
          "and face photo; no contact values shown")


if __name__ == "__main__":
    try:
        main()
    except (ValueError, OSError, struct.error) as exc:
        # Error messages must never include contact values or file contents.
        sys.exit(f"Contact asset generation failed: {exc}")
