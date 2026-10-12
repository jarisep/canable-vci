# SPDX-License-Identifier: Apache-2.0
"""Public contract tests; no devices, network, or private captures."""
import ctypes
import json
from pathlib import Path
import re
import struct
import sys
import unittest
ROOT=Path(__file__).resolve().parents[1]
sys.path.insert(0,str(ROOT/'tools'))
import make_usb_vectors as usb_vectors

def read_json(path):
    return json.loads((ROOT/path).read_text())

class AbiTests(unittest.TestCase):
    def test_windows_abi_widths(self):
        class Message(ctypes.Structure):
            _fields_ = [(name, ctypes.c_uint32) for name in ('ProtocolID', 'RxStatus', 'TxFlags', 'Timestamp', 'DataSize', 'ExtraDataIndex')] + [('Data', ctypes.c_ubyte * 4128)]

        class X86PointerList(ctypes.Structure):
            _fields_ = [('count', ctypes.c_uint32), ('pointer', ctypes.c_uint32)]

        class Parameter(ctypes.Structure):
            _fields_ = [('id', ctypes.c_uint32), ('value', ctypes.c_uint32), ('supported', ctypes.c_uint32)]

        self.assertEqual(ctypes.sizeof(Message), 4152)
        self.assertEqual(Message.Data.offset, 24)
        self.assertEqual(ctypes.sizeof(X86PointerList), 8)
        self.assertEqual(X86PointerList.pointer.offset, 4)
        self.assertEqual(ctypes.sizeof(Parameter), 12)



class UsbContractTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.schema = read_json('protocol/usb-isotp-v1.json')
        cls.vectors = read_json('tests/fixtures/usb-v1-vectors.json')

    def test_layout_and_unique_opcodes(self):
        s = self.schema
        self.assertEqual(struct.calcsize(s['header_struct']), 32)
        self.assertEqual([f['offset'] for f in s['header_fields']], [0, 4, 5, 6, 8, 12, 16, 20, 24])
        ops = s['commands'] + s['events']
        self.assertEqual(len({op['opcode'] for op in ops}), len(ops))
        for op in ops:
            fmt = op['format']
            self.assertEqual(len(struct.unpack(fmt, bytes(struct.calcsize(fmt)))), len(op['fields']))
            if 'reply_format' in op:
                self.assertEqual(len(struct.unpack(op['reply_format'], bytes(struct.calcsize(op['reply_format'])))), len(op['reply_fields']))
            self.assertLessEqual(struct.calcsize(fmt) + op.get('tail_max', 0), s['max_body_bytes'])

    def test_vectors_match_documented_byte_order(self):
        self.assertEqual(usb_vectors.build(), self.vectors)
        hello = bytes.fromhex(self.vectors['vectors'][0]['wire_hex'])
        # Independent fixed bytes: CISO, version=1, command=1, opcode=1, body=16.
        self.assertEqual(hello[:12].hex(), '4349534f0101010010000000')
        self.assertEqual(hello[32:40].hex(), '0807060504030201')
        self.assertEqual(hello[40:44], (5000).to_bytes(4, 'little'))

    def test_vector_lengths_and_payloads(self):
        defs = {op['name']: op for op in self.schema['commands'] + self.schema['events']}
        for v in self.vectors['vectors']:
            with self.subTest(vector=v['id']):
                wire = bytes.fromhex(v['wire_hex'])
                magic, version, kind, op, bodylen, session, channel, request, timestamp = struct.unpack_from(self.schema['header_struct'], wire)
                self.assertEqual((magic, version, kind), (b'CISO', 1, v['kind']))
                definition = defs[v['operation']]
                self.assertEqual(op, definition['opcode'])
                self.assertEqual(bodylen, len(wire) - 32)
                self.assertLessEqual(bodylen, self.schema['max_body_bytes'])
                fixed = struct.calcsize(definition['format'])
                values = dict(zip(definition['fields'], struct.unpack_from(definition['format'], wire, 32)))
                self.assertEqual(values, v['body_fields'])
                tail = wire[32 + fixed:]
                self.assertEqual(tail.hex(), v['tail_hex'])
                if 'tail_length_field' in definition:
                    self.assertEqual(len(tail), values[definition['tail_length_field']])
                    self.assertLessEqual(len(tail), definition['tail_max'])

    def test_wire_payloads_are_from_named_reference(self):
        refs = {c['id']: c for c in read_json('tests/fixtures/proace-reference.json')['cases']}
        for v in self.vectors['vectors']:
            source = v['source_reference']
            if not source:
                continue
            record = next(r for r in refs[source['case']]['records'] if r['line'] == source['line'])
            offset = 5 if 'Data[5:]' in source['extract'] else 4
            self.assertEqual(bytes.fromhex(v['tail_hex']), bytes.fromhex(record['data_hex'])[offset:])

    def test_windows_emulation_does_not_become_hardware_command(self):
        names = [op['name'] for op in self.schema['commands']]
        self.assertFalse(any(re.search(r'IGNITION|VOLTAGE|PIN', name) for name in names))
        self.assertIn('forced_on_ignition', self.schema['windows_only_state'])
        self.assertEqual(self.schema['target_limits']['logical_channels'], 1)
