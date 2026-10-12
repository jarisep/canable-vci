#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
"""Produce proposed USB wire examples, not a capture or a vehicle replay tool."""
import argparse
import json
from pathlib import Path
import struct

ROOT = Path(__file__).resolve().parents[1]


def build():
    schema = json.loads((ROOT / 'protocol/usb-isotp-v1.json').read_text())
    refs = json.loads((ROOT / 'tests/fixtures/proace-reference.json').read_text())
    cases = {c['id']: c for c in refs['cases']}
    commands = {c['name']: c for c in schema['commands']}
    events = {c['name']: c for c in schema['events']}
    vectors = []

    def add(name, kind, op, values, tail=b'', session=0x11223344, channel=1, request=0, time=0, source=None):
        definition = commands[op] if kind == 1 else events[op]
        body = struct.pack(definition['format'], *values) + tail
        header = struct.pack(schema['header_struct'], b'CISO', 1, kind, definition['opcode'], len(body),
                             session, channel, request, time)
        vectors.append({'id': name, 'kind': kind, 'operation': op,
                        'header': {'session_id': session, 'channel_generation': channel,
                                   'request_id': request, 'device_time_us': time},
                        'body_fields': dict(zip(definition['fields'], values)), 'tail_hex': tail.hex(),
                        'wire_hex': (header + body).hex(), 'source_reference': source})

    add('hello', 1, 'HELLO', [0x0102030405060708, 5000, 0], session=0, channel=0, request=1)
    add('open_500k', 1, 'CHANNEL_OPEN', [0x8005, 0, 500000], request=2)
    add('bsi_route', 1, 'ROUTE_ADD', [1, 0x652, 0x752, 0, 0, 0, 0, 0, 0], request=3)
    add('bsi_session_request', 1, 'TX_SUBMIT', [1, 0, 2, 0, 1000], bytes.fromhex('1003'), request=4,
        source={'case': 'bsi_single_frame_session', 'line': 14, 'extract': 'Data[4:]'})
    add('bsi_tx_done', 3, 'TX_DONE', [1, 1, 4, 9, 0x752, 0, 0, 0], request=4, time=10000)
    multi = cases['bsi_multiframe_identification']['records'][-1]
    data = bytes.fromhex(multi['data_hex'])[4:]
    add('bsi_rx_start', 3, 'RX_START', [2, 1, 1, 2, 0x652, 0, 0, len(data)], time=20000)
    add('bsi_rx_complete', 3, 'RX_COMPLETE', [3, 1, 1, multi['rx_status'], 0x652, 0, 0, len(data)], data,
        time=35000, source={'case': 'bsi_multiframe_identification', 'line': multi['line'], 'extract': 'Data[4:]'})
    add('ack_rx', 1, 'EVENT_ACK', [3], channel=0, request=5)
    extended = cases['extended_address_complete_response']['records'][0]
    ext_data = bytes.fromhex(extended['data_hex'])
    add('extended_session_request', 1, 'TX_SUBMIT', [2, 0x80, len(ext_data) - 5, 0, 1000], ext_data[5:], request=6,
        source={'case': 'extended_address_complete_response', 'line': extended['line'],
                'extract': 'Data[5:]; CAN ID and extended address belong to route 2'})
    add('flow_control_timeout', 3, 'TRANSFER_ERROR', [4, 1, 7, 0, 0x752, 0, 0, 0, 2, -2], request=7, time=1000000)
    return {'schema_version': 1, 'origin': 'proposed_synthetic_usb_vectors', 'auto_replay_to_vehicle': False,
            'notes': 'Wire examples for independent codecs. Device timestamps are invented. RX_COMPLETE padding status is copied from Bosch, not measured on CANable. Cases are independent examples, not one executable command sequence.',
            'vectors': vectors}


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--check', action='store_true')
    args = parser.parse_args()
    path = ROOT / 'tests/fixtures/usb-v1-vectors.json'
    rendered = json.dumps(build(), ensure_ascii=False, indent=2) + '\n'
    if args.check:
        if not path.exists() or path.read_text() != rendered:
            raise SystemExit('USB examples differ from the contract')
    else:
        path.parent.mkdir(parents=True, exist_ok=True)
        path.write_text(rendered)
    print(f'{"verified" if args.check else "wrote"} {path.relative_to(ROOT)}')


if __name__ == '__main__':
    main()
