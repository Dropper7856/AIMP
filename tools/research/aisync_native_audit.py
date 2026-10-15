"""Reproduce the AISync machine-code boundary checks without running AI.exe.

This verifies bytes/ABI/layout evidence, not runtime navigation or animation.
Usage: python tools/research/aisync_native_audit.py [AI.exe]
"""
from pathlib import Path
import hashlib
import json
import re
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
    ("Victim graph finish complete hook prefix", 0x255A60, "56 8B F1 8B 8E B0 00 00 00"),
    ("Victim graph finish plain RET", 0x255B24, "C3"),
    ("Victim graph finish calls source finish", 0x255A7A, "E8 21 FF FF FF"),
    ("Stop constructor immediately requests completion", 0x2612CF, "80 CA 02 88 56 08"),
    ("Paired update victim classification/ESI/EAX", 0x263E4A,
     "8B 46 44 85 C0 74 58 8B 80 1C 03 00 00 85 C0 74 06 80 78 48 00 75 48"),
    ("Paired update NPC-only controller guard", 0x263E71, "83 79 74 03"),
    ("Paired update retail player stage dispatcher", 0x263EA9, "8B 86 E0 00 00 00"),
    ("Prepared source native transition ECX=this/call producer", 0x2638D0, "56 8B F1 E8 98 ED FF FF"),
    ("Prepared source native stage 2 and plain RET", 0x2638EA, "C7 86 E0 00 00 00 02 00 00 00 5E C3"),
    ("Prepared source alternate phase guard", 0x263900, "56 8B F1 8B 86 18 01 00 00 24 07 3C 01"),
    ("Prepared source alternate native producer/stage 5/RET", 0x26390F, "E8 5C ED FF FF C7 86 E0 00 00 00 05 00 00 00 5E C3"),
    ("Retail move producer gait/reset outside request", 0x284C28, "89 B8 94 01 00 00 C7 80 90 01 00 00 00 00 00 00"),
    ("Second retail move producer same gait/reset contract", 0x284041, "89 83 94 01 00 00 C7 83 90 01 00 00 00 00 00 00"),
    ("Scheduler transition early RET 4", 0x287412, "C2 04 00"),
    ("Scheduler transition final RET 4", 0x287687, "C2 04 00"),
    ("Scheduler idle callback plain RET", 0x287A19, "C3"),
    ("Paired graph player E5 branch", 0x264AA9, "80 BE E5 00 00 00 00 74 14"),
    ("Idle class dispatch can bypass shared Idle hook", 0x25FCB6,
     "8B 41 64 8B 40 04 83 F8 02 74 19 83 F8 07 74 14 83 F8 03 74 0F 83 F8 04 74 0A 83 F8 06 74 05 83 F8 09 75 62"),
    ("Alternate Idle has same native class dispatch", 0x25FD92,
     "8B 41 64 8B 40 04 83 F8 02 74 19 83 F8 07 74 14 83 F8 03 74 0F 83 F8 04 74 0A 83 F8 06 74 05 83 F8 09 75 5B"),
    ("Idle dispatch destroys previous cycle before constructor", 0x25FCF7, "8B 06 8B 10 6A 01 8B CE FF D2"),
    ("Alternate Idle dispatch also destroys previous cycle", 0x25FDD3, "8B 06 8B 10 6A 01 8B CE FF D2"),
    ("Interactive victim Start_Death failure before D8 finish", 0x2659AD, "80 BE DA 00 00 00 00 74 17 80 4E 08 06"),
    ("Cancel clears scheduler retention flag regardless of status", 0x283DCB, "83 7E 0C 00 C6 46 18 00"),
    ("Terminal scheduler list retains requests with +18 set", 0x283478, "80 7E 18 00 74 3F"),
]
failures = []
target_sha256 = hashlib.sha256(data).hexdigest()
if target_sha256 != "00d6bb846ac5a78916baf363eb148aa255a23bf216a271cfd23a0ece3c368364":
    failures.append({"name": "Certified AI.exe identity", "actual": target_sha256})
# Audit the literals that actually gate installation, not only the independent
# RE evidence above. A duplicated byte in a live guard previously escaped this
# audit because its separately transcribed evidence was correct.
source = Path(__file__).resolve().parents[2] / "AIMP.Native" / "src" / "AISync.cpp"
runtime_guards = re.findall(
    r"MatchRetailBytes\(\s*(0x[0-9a-fA-F]+)U?\s*,\s*\{([^}]+)\}\s*\)",
    source.read_text(encoding="utf-8-sig"),
)
if not runtime_guards:
    raise ValueError("No runtime AISync certification guards were found")
for rva_text, literals in runtime_guards:
    expected = bytes(int(value.strip().rstrip("Uu"), 0)
                     for value in literals.split(",") if value.strip())
    checks.append(("Runtime AISync guard", int(rva_text, 16), expected.hex()))
hook_prefixes = re.findall(
    r'Hook\(hooks,\s*"([^"]+)",\s*(0x[0-9a-fA-F]+)U?\s*,\s*'
    r'reinterpret_cast<void\*>\(\s*&\w+\s*\)\s*,\s*\{([^}]+)\}',
    source.read_text(encoding="utf-8-sig"),
)
for name, rva_text, literals in hook_prefixes:
    expected = bytes(int(value.strip().rstrip("Uu"), 0)
                     for value in literals.split(",") if value.strip())
    checks.append(("Runtime hook prefix: " + name, int(rva_text, 16), expected.hex()))
for name, rva, hex_bytes in checks:
    expected = bytes.fromhex(hex_bytes)
    actual = at(rva, len(expected))
    if actual != expected:
        failures.append({"name": name, "rva": hex(rva), "actual": actual.hex(), "expected": expected.hex()})
for rva, target in [(0xEAE5D4, 0x264A10), (0xEAE694, 0x2658E0)]:
    actual = struct.unpack("<I", at(rva, 4))[0]
    if actual != image_base + target:
        failures.append({"vtableRva": hex(rva), "actual": hex(actual)})
# The new whole-instruction trampolines copy a PUSH absolute SEH handler.
# Confirm these operands are actually PE HIGHLOW relocations, so their loaded
# values remain valid when the game is rebased.
reloc_rva, reloc_size = struct.unpack_from("<II", data, pe + 24 + 96 + 5 * 8)
relocations = set()
cursor = 0
while cursor < reloc_size:
    page, block_size = struct.unpack("<II", at(reloc_rva + cursor, 8))
    if block_size < 8 or block_size % 2 or cursor + block_size > reloc_size:
        raise ValueError("Invalid PE base relocation block")
    for entry in struct.unpack("<" + "H" * ((block_size - 8) // 2),
                               at(reloc_rva + cursor + 8, block_size - 8)):
        if entry >> 12 == 3:
            relocations.add(page + (entry & 0xFFF))
    cursor += block_size
for rva in (0x25FC83, 0x25FD63):
    if rva not in relocations:
        failures.append({"name": "Paired Idle SEH HIGHLOW operand", "rva": hex(rva)})
print(json.dumps({"binary": str(path), "sha256": target_sha256,
                  "checks": len(checks) + 5, "failures": failures,
                  "runtimeGuards": len(runtime_guards),
                  "hookPrefixes": len(hook_prefixes),
                  "validation": "static only; Host/Client runtime acceptance remains required"}, indent=2))
sys.exit(bool(failures))
