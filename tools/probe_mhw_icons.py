#!/usr/bin/env python3
"""Inspect only the extracted MHW item table and icon atlas; write a local preview under build/.

The extracted game bytes are never committed. This probe does not change the game or launch it.
"""
from collections import Counter
from pathlib import Path
import struct

from PIL import Image


ROOT = Path(__file__).resolve().parent.parent
ITEMS = ROOT / "build/mhw/itemData.itm"
ATLAS = ROOT / "build/mhw/cmn_item00_ID.tex"


def dds_bc7(width: int, height: int, blocks: bytes) -> bytes:
    # DDS_HEADER + DDS_HEADER_DXT10, BC7_UNORM (DXGI 98).
    header = bytearray(148)
    header[:4] = b"DDS "
    struct.pack_into("<I", header, 4, 124)
    struct.pack_into("<I", header, 8, 0x81007)
    struct.pack_into("<III", header, 12, height, width, len(blocks))
    struct.pack_into("<I", header, 76, 32)
    struct.pack_into("<I", header, 80, 4)
    header[84:88] = b"DX10"
    struct.pack_into("<I", header, 108, 0x1000)
    struct.pack_into("<IIIII", header, 128, 98, 3, 0, 1, 0)
    return bytes(header) + blocks


def main() -> None:
    items = ITEMS.read_bytes()
    atlas = ATLAS.read_bytes()
    assert items[:4] == b"\x01\x10\x09\x18"
    count = struct.unpack_from("<H", items, 6)[0]
    assert len(items) == 10 + count * 32
    assert atlas[:4] == b"TEX\0"
    width, height = struct.unpack_from("<II", atlas, 0x18)
    assert width == height == 1024 and len(atlas) == 192 + width * height
    icon_ids = Counter(struct.unpack_from("<I", items, 10 + i * 32 + 18)[0] for i in range(count))
    assert max(icon_ids) <= 255
    unused = [i for i in range(256) if i not in icon_ids]
    out = ROOT / "build/mhw/item-atlas-preview.png"
    Image.open(__import__("io").BytesIO(dds_bc7(width, height, atlas[192:]))).save(out)
    print(f"items={count} icons-used={len(icon_ids)} icon-cells-unused={len(unused)}")
    print(f"atlas={width}x{height} BC7 preview={out}")
    print("unused cells:", ",".join(map(str, unused)))


if __name__ == "__main__":
    main()
