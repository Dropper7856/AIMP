"""Offline regression checks for the native bank's playback dependencies."""
import re
import struct
import unittest
from pathlib import Path

from VoiceBankAudit import (ACTION, SOURCE, SOUND, audit_audio_input_dependencies,
                            chunks, objects, patch)


def embedded_bank():
    header = Path(__file__).resolve().parents[1] / 'include/aimp/VoiceBank.h'
    text = header.read_text().split('kVoiceBank =', 1)[1]
    return bytes(int(x, 16) for x in re.findall(r'0x([0-9A-Fa-f]{2})U', text))


def rewrite_objects(bank, transform):
    records = transform(objects(bank))
    hirc = struct.pack('<I', len(records)) + b''.join(
        bytes([kind]) + struct.pack('<I', len(body)) + body
        for kind, body in records.values())
    return b''.join(tag + struct.pack('<I', len(body)) + body
                    for tag, body in chunks(bank) if tag != b'HIRC') + \
        b'HIRC' + struct.pack('<I', len(hirc)) + hirc


class VoiceBankDependenciesTests(unittest.TestCase):
    def test_complete_native_bank(self):
        bank = embedded_bank()
        audit_audio_input_dependencies(bank)
        self.assertEqual(len(objects(bank)), 5)

    def test_v242_bank_loadable_but_missing_generator(self):
        # Reproduce the exact v242 regression: valid HIRC lengths/references for
        # Event/Action/Sound, but no object for the remapped generator source ID.
        bank = rewrite_objects(embedded_bank(),
                               lambda records: {k: v for k, v in records.items() if k != SOURCE})
        self.assertEqual(len(bank), 327)
        self.assertEqual(len(objects(bank)), 4)
        with self.assertRaisesRegex(AssertionError, 'no CAkFxCustom'):
            audit_audio_input_dependencies(bank)

    def test_generator_must_be_custom_fx(self):
        def change(records):
            records[SOURCE] = (18, records[SOURCE][1])
            return records
        with self.assertRaisesRegex(AssertionError, 'type 19'):
            audit_audio_input_dependencies(rewrite_objects(embedded_bank(), change))

    def test_generator_plugin_must_match_source(self):
        def change(records):
            kind, body = records[SOURCE]
            records[SOURCE] = (kind, patch(body, 4, 0x00640002))
            return records
        with self.assertRaisesRegex(AssertionError, 'plug-in IDs differ'):
            audit_audio_input_dependencies(rewrite_objects(embedded_bank(), change))

    def test_source_reference_must_resolve(self):
        def change(records):
            kind, body = records[SOUND]
            records[SOUND] = (kind, patch(body, 12, 0x164701DC))
            return records
        with self.assertRaisesRegex(AssertionError, 'no CAkFxCustom'):
            audit_audio_input_dependencies(rewrite_objects(embedded_bank(), change))

    def test_play_action_must_reference_own_bank(self):
        def change(records):
            kind, body = records[ACTION]
            records[ACTION] = (kind, patch(body, 14, 0x5C770DB7))
            return records
        with self.assertRaises(AssertionError):
            audit_audio_input_dependencies(rewrite_objects(embedded_bank(), change))


if __name__ == '__main__':
    unittest.main()
