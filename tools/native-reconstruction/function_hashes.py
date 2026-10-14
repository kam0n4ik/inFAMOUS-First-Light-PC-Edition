"""FNV-1a 64 of guest function bodies, read from eboot.bin (module-relative addresses).

The native engine hooks (src/engine/guest_hooks.*) refuse to patch unless the loaded bytes hash to these
values, so a different game version or patched code never runs the reconstruction.

usage: py -I -X utf8 function_hashes.py <eboot.bin> <rva>:<size> [...]
"""
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / "tools"))
import ps4elf  # noqa: E402


def fnv1a64(data: bytes) -> int:
    h = 0xCBF29CE484222325
    for b in data:
        h ^= b
        h = (h * 0x100000001B3) & 0xFFFFFFFFFFFFFFFF
    return h


img, _ = ps4elf.Ps4Elf(sys.argv[1]).build_image()
for spec in sys.argv[2:]:
    rva, size = (int(x, 0) for x in spec.split(":"))
    body = bytes(img[rva:rva + size])
    print(f"{{0x{rva:x}, 0x{size:x}, 0x{fnv1a64(body):016x}ull}}, // first bytes {body[:16].hex()}")
