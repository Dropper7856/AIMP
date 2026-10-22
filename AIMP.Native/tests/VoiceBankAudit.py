"""Reproduce and audit the native VoiceSync bank; never writes game files.

python AIMP.Native/tests/VoiceBankAudit.py --sound-dir PATH [--write-header]
Copies shipped AudioInput source + complete native 3D/game-aux node parameters.
"""
import argparse
import hashlib
import re
import struct
from pathlib import Path

BANK, SOURCE, SOUND, ATTENUATION, ACTION, EVENT = (0xE571E435, 0x0F12CB6E, 0x5774922A, 0x2C86D6E9, 0xA9EF6E1D, 0xAC7D4E6B)
U32 = lambda b, o=0: struct.unpack_from('<I', b, o)[0]

def chunks(data):
    p = 0
    while p < len(data):
        assert p + 8 <= len(data)
        tag, n = data[p:p+4], U32(data,p+4)
        assert p+8+n <= len(data)
        yield tag, data[p+8:p+8+n]
        p += 8+n

def objects(data):
    content = dict(chunks(data))[b'HIRC']; p = 4; result = {}
    for _ in range(U32(content)):
        kind, size = content[p], U32(content,p+1)
        body = content[p+5:p+5+size]
        assert len(body) == size and len(body) >= 4
        key = U32(body)
        assert key not in result or result[key][0] != kind # INIT has separate namespaces for types 18/20
        result[key] = (kind,body)
        p += 5+size
    assert p == len(content)
    return result

def node_layout(body, source_end):
    p = source_end
    fx = body[p+1]; p += 2
    if fx: p += 1 + 7*fx
    bus_at, parent_at = p, p+4; p += 10
    n = body[p]; p += 1+5*n
    n = body[p]; p += 1+9*n
    position_at = p
    assert body[p:p+3] == b'\x01\x00\x01'
    assert U32(body,p+3) == 1
    aux_at = p + 13
    return bus_at,parent_at,position_at,aux_at

def patch(data, offset, value):
    result = bytearray(data); struct.pack_into('<I', result,offset,value); return bytes(result)

def audit_audio_input_dependencies(bank):
    """Validate the runtime generator lookup, not just HIRC byte boundaries.

    AI.exe c62320/c5adc0 store a generator's source ID, then c773a0 resolves
    that ID in the CAkFxCustom index before AudioInput::Init can run. A bank
    can load and PostEvent can return an ID even when this dependency is absent.
    """
    inventory = objects(bank)
    kind, sound = inventory[SOUND]
    assert kind == 2 and U32(sound,4) == 0x00C80002
    source_id = U32(sound,12)
    assert source_id in inventory, 'AudioInput source has no CAkFxCustom definition'
    kind, generator = inventory[source_id]
    assert kind == 19, 'AudioInput source must resolve to HIRC type 19 (CAkFxCustom)'
    assert U32(generator,4) == U32(sound,4), 'AudioInput plug-in IDs differ'
    assert len(generator) == 19 and U32(generator,8) == 4
    assert generator[12:] == b'\0'*7, 'Unexpected AudioInput parameters or bindings'
    assert inventory[ATTENUATION][0] == 14
    assert U32(sound,node_layout(sound,33)[2]+7) == ATTENUATION
    kind, action = inventory[ACTION]
    assert kind == 3 and U32(action,6) == SOUND and U32(action,14) == BANK
    kind, event = inventory[EVENT]
    assert kind == 4 and U32(event,4) == 1 and U32(event,8) == ACTION

def compose(sound_dir):
    names = ['UI.BNK','M04_WELCOMETOSEVASTOPOL.BNK','M22_ALL_FATHER_CORE.BNK','HUMAN.BNK','INIT.BNK']
    raw = {n:(sound_dir/n).read_bytes() for n in names}
    inventory = {n:objects(b) for n,b in raw.items()}
    for b in raw.values(): assert U32(dict(chunks(b))[b'BKHD']) == 88
    ui, source_donor, aux_donor, human, init = (inventory[n] for n in names)
    movie = ui[0x26AB29F1][1]
    assert movie[4:8] == bytes.fromhex('0200c800') and U32(movie,8) == 0
    assert len(movie) == 78 and U32(movie,29) == 0 # generator parameter block
    # The Sound descriptor is only a reference. Its separately authored
    # CAkFxCustom object owns the AudioInput factory/parameters used at playback.
    generator_kind, generator_donor = ui[U32(movie,12)]
    assert generator_kind == 19 and U32(generator_donor,4) == U32(movie,4)
    generator = patch(generator_donor,0,SOURCE)
    native_node = aux_donor[0x0346044A][1]
    assert len(native_node) == 88
    old_bus, old_parent, old_pos, old_aux = node_layout(native_node,29)
    assert U32(native_node,old_parent) == 0 and native_node[old_aux:old_aux+4] == b'\x00\x01\x00\x00'
    node = movie[:33]+native_node[29:]
    node = patch(node,0,SOUND); node = patch(node,12,SOURCE)
    bus_at,parent_at,pos_at,aux_at = node_layout(node,33)
    # Use the world bus from a complete parentless retail 3D sound, not the
    # movie/UI bus. The native game-aux node tail is otherwise preserved.
    world = source_donor[0x1748DD77][1]
    native_bus = U32(world,node_layout(world,29)[0])
    assert native_bus == 0x8C0DCD35 and init[native_bus][0] == 8
    node = patch(node,bus_at,native_bus)
    node = patch(node,pos_at+7,ATTENUATION)
    attenuation = patch(human[0x02E54F50][1],0,ATTENUATION)
    action = patch(patch(patch(ui[0x03549516][1],0,ACTION),6,SOUND),14,BANK)
    event = patch(patch(ui[0x0C057FE7][1],0,EVENT),8,ACTION)
    header = patch(dict(chunks(raw['UI.BNK']))[b'BKHD'],4,BANK)
    hirc = struct.pack('<I',5)+b''.join(bytes([kind])+struct.pack('<I',len(body))+body for kind,body in [(19,generator),(14,attenuation),(2,node),(3,action),(4,event)])
    bank = b'BKHD'+struct.pack('<I',len(header))+header+b'HIRC'+struct.pack('<I',len(hirc))+hirc
    assert len(bank) == 351
    rebuilt = objects(bank)
    assert set(rebuilt) == {SOURCE,ATTENUATION,SOUND,ACTION,EVENT}
    audit_audio_input_dependencies(bank)
    assert U32(node,parent_at) == 0 and node[aux_at+1] == 1
    # No other node settings edited: remapping is limited to documented IDs/bus.
    expected = movie[:33]+native_node[29:]
    for at,value in [(0,SOUND),(12,SOURCE),(bus_at,native_bus),(pos_at+7,ATTENUATION)]: expected = patch(expected,at,value)
    assert node == expected
    provenance = {n:hashlib.sha256(b).hexdigest() for n,b in raw.items()}
    return bank,provenance

def header_text(bank):
    lines = ['#pragma once','#include <array>','#include <cstdint>','namespace aimp::voice_sync {',
        '// Wwise v88: shipped AudioInput CAkFxCustom + source + native 3D/game-aux node.',
        '// Native HUMAN attenuation, world bus, independent IDs; no movie event fallback.',
        '// Reproduce and audit with AIMP.Native/tests/VoiceBankAudit.py using the installed SOUND banks.',
        f'inline constexpr std::uint32_t kVoiceBankId = 0x{BANK:08X}U;',
        f'inline constexpr std::uint32_t kVoiceEventId = 0x{EVENT:08X}U;',
        f'inline constexpr std::uint32_t kVoiceSourceId = 0x{SOURCE:08X}U;',
        f'alignas(32) inline const std::array<std::uint8_t, {len(bank)}> kVoiceBank = {{']
    lines += ['    '+','.join(f'0x{x:02X}U' for x in bank[i:i+16])+',' for i in range(0,len(bank),16)]
    return '\n'.join(lines+['};','}',''])

if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--sound-dir',type=Path,required=True)
    parser.add_argument('--write-header',action='store_true')
    args = parser.parse_args()
    bank,provenance = compose(args.sound_dir)
    header = Path(__file__).resolve().parents[1]/'include/aimp/VoiceBank.h'
    if args.write_header: header.write_text(header_text(bank),encoding='utf8')
    text = header.read_text().split('kVoiceBank =',1)[1]
    embedded = bytes(int(x,16) for x in re.findall(r'0x([0-9A-Fa-f]{2})U',text))
    assert embedded == bank, 'Embedded bank differs from the shipped-asset recipe'
    print('PASS AudioInput CAkFxCustom dependency, native source, complete 3D node, game aux sends, stock attenuation, world bus and HIRC references')
    print('bank SHA256',hashlib.sha256(bank).hexdigest())
    for name,digest in provenance.items(): print(name,digest)
