"""Reproduce the AISync v188 machine-code boundary checks without running AI.exe.

This verifies bytes/ABI/layout evidence, not runtime navigation or animation.
Usage: python tools/research/aisync_native_audit.py [AI.exe]
"""
from pathlib import Path
import hashlib
import json
import struct
import sys

path = Path(sys.argv[1]) if len(sys.argv) > 1 else Path("AI.exe")
data = path.read_bytes()
pe = struct.unpack_from("<I", data, 0x3C)[0]
assert data[pe:pe + 4] == b"PE\0\0"
machine, section_count = struct.unpack_from("<HH", data, pe + 4)
optional_size = struct.unpack_from("<H", data, pe + 20)[0]
assert machine == 0x14C and struct.unpack_from("<H", data, pe + 24)[0] == 0x10B
image_base = struct.unpack_from("<I", data, pe + 24 + 28)[0]
sections = []
for index in range(section_count):
    offset = pe + 24 + optional_size + index * 40
    virtual_size, rva, raw_size, raw = struct.unpack_from("<IIII", data, offset + 8)
    sections.append((rva, raw_size, raw, virtual_size))


def at(rva, size):
    for start, raw_size, raw, _ in sections:
        if start <= rva and rva + size <= start + raw_size:
            return data[raw + rva - start:raw + rva - start + size]
    raise ValueError(f"RVA {rva:#x} is outside file-backed PE sections")


checks = [
    ("Move factory ECX=scheduler, output Ref", 0x283BF0, "51 56 8B 74 24 0C 56"),
    ("Move factory RET 4", 0x283C08, "C2 04 00"),
    ("Move request kind=1", 0x282D45, "89 50 10"),
    ("Move request vtable", 0x282D59, "C7 00 70 1A 2B 01"),
    ("Move goal argument", 0x2777B0, "8B 44 24 04"),
    ("Move goal updates planner natively", 0x2777CC, "E9 AF 88 DC FF"),
    ("Goal update native state gate", 0x2C7550, "83 79 68 03"),
    ("Goal update RET 4", 0x2C7597, "C2 04 00"),
    ("Submit native scheduler", 0x283CE0, "64 A1 00 00 00 00 6A FF"),
    ("Submit plain RET", 0x283D7A, "C3"),
    ("Cancel native scheduler", 0x283DB0, "64 A1 00 00 00 00 6A FF"),
    ("Release Ref uses ECX", 0xD770, "56 8B 71 04"),
    ("Release Ref plain RET", 0xD7AD, "C3"),
    ("Stop 3B transition entry", 0x2625E0, "6A FF 68 C1 3F 12 01"),
    ("Stop 3B transition RET 4 (not parameterless)", 0x262661, "C2 04 00"),
    ("Stop constructor state=3B", 0x2612B1, "C6 46 1A 3B"),
    ("Stop constructor RET 4", 0x261350, "C2 04 00"),
    ("Paired source event complete prefix", 0x264A10, "56 57 68 84 E9 2A 01"),
    ("Paired victim event complete prefix", 0x2658E0, "83 EC 1C 56 57"),
    ("Paired source event RET 4", 0x264AC8, "C2 04 00"),
    ("Paired victim event RET 4", 0x265A25, "C2 04 00"),
    ("Paired source finish plain RET", 0x255A5B, "C3"),
    ("Paired source finish complete prefix", 0x2559A0, "83 EC 10 56 8B F1"),
    ("Native finish sets source+E6", 0x255A21, "C6 86 E6 00 00 00 01"),
    ("Native victim finish sets victim+D8", 0x255A69, "C6 86 D8 00 00 00 01"),
]
failures = []
for name, rva, hex_bytes in checks:
    expected = bytes.fromhex(hex_bytes)
    actual = at(rva, len(expected))
    if actual != expected:
        failures.append({"name": name, "rva": hex(rva), "actual": actual.hex(), "expected": expected.hex()})
for rva, target in [(0xEAE5D4, 0x264A10), (0xEAE694, 0x2658E0)]:
    actual = struct.unpack("<I", at(rva, 4))[0]
    if actual != image_base + target:
        failures.append({"vtableRva": hex(rva), "actual": hex(actual)})
print(json.dumps({"binary": str(path), "sha256": hashlib.sha256(data).hexdigest(),
                  "checks": len(checks) + 2, "failures": failures,
                  "validation": "static only; Host/Client runtime acceptance remains required"}, indent=2))
sys.exit(bool(failures))
