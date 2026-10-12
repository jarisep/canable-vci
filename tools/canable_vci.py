#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
"""Stage 2 USB development CLI. Probe/test never transmits CAN messages."""
import argparse
import json
import os
import struct
import sys
import time
from ciso_protocol import Client, Message
from ciso_usb import open_transport, WindowsUSB

def event_dict(e):
    result={'opcode':e.opcode,'request':e.request,'generation':e.generation,'device_time_us':e.device_time_us}
    if e.opcode in (256,257,258,259) and len(e.body)>=24:
        seq,route,transfer,status,can_id,ext,reserved,n=struct.unpack_from('<IIIIIBBH',e.body)
        result.update(event_seq=seq,route=route,transfer=transfer,rx_status=f'0x{status:x}',can_id=f'0x{can_id:x}',extended_address=ext,payload_length=n)
        if e.opcode==258:result['payload_hex']=e.body[24:].hex()
        if e.opcode==259:result['reason'],result['isotp_error']=struct.unpack_from('<Ii',e.body,24)
    return result

def probe(client):
    hello=client.hello()
    info=client.command(5).body
    return {'hello':hello,'capabilities':client.capabilities(),'uid':info[4:16].hex(),
            'firmware':info[16:36].split(b'\0')[0].decode('ascii'),
            'revision':info[36:56].split(b'\0')[0].decode('ascii'),
            'board':info[56:72].split(b'\0')[0].decode('ascii')}

def usb_test(client):
    result=probe(client)
    cases=[]
    def expect(op,body=b'',generation=0,want=0,fragment=1):
        response=client.command(op,body,generation,fragment=fragment,check=False)
        actual=struct.unpack_from('<I',response.body)[0]
        if actual!=want:raise RuntimeError(f'USB test {op}: expected {want}, got {actual}')
        cases.append({'opcode':op,'result':actual,'body_bytes':len(body),'fragment':fragment})
    expect(2,fragment=1)
    expect(0x7777,want=5)
    expect(16,struct.pack('<III',0x8005,0,500000),1)
    expect(48,struct.pack('<IIHHI',0,0,4095,0,0)+bytes((i&255) for i in range(4095)),1,want=6,fragment=61)
    expect(48,struct.pack('<IIHHI',0,0,4095,0,0)+b'wrong',1,want=2)
    expect(18,struct.pack('<IIIHBBI',2000,1000,1000,10,0,0,0),1,want=5)
    expect(81)
    expect(17,generation=1)
    expect(16,struct.pack('<III',7,0,500000),1,want=4)
    expect(16,struct.pack('<III',7,0,500000),2)
    expect(17,generation=2)
    expect(3)
    client.close()
    result['tests']=cases;result['can_messages_transmitted']=0
    return result

def usb_recovery():
    """Exercise lease recovery, including an incomplete old OUT command."""
    cases=[]
    for partial in (False,True):
        client=Client(open_transport())
        try:
            old_session=client.hello(500)['session_id']
            client.command(16,struct.pack('<III',7,0,500000),1)
            if partial:
                frame=Message(1,48,client.session,1,client.request+1,0,bytes(100)).encode()
                client.transport.write(frame[:39])
        finally:
            # Intentionally omit SESSION_CLOSE: the lease must release ownership.
            client.disconnect()
        time.sleep(.75)
        client=Client(open_transport())
        try:
            new_session=client.hello(500)['session_id']
            if new_session==old_session:
                raise RuntimeError('Lease recovery reused the old session')
            status=struct.unpack('<7I',client.command(81).body)
            if status[1]!=0:
                raise RuntimeError(f'CAN channel remains open after lease expiry: {status}')
            client.capabilities()
            client.close()
            cases.append({'partial_command':partial,'old_session':old_session,
                          'new_session':new_session,'bus_state_after_lease':status[1],
                          'passed':True})
        finally:
            client.disconnect()
    return {'tests':cases,'can_messages_transmitted':0,
            'method':'release interface without closing session; wait past lease; reopen'}

def read_bsi(client,payload):
    result=probe(client)
    client.command(16,struct.pack('<III',0x8005,0,500000),1)
    client.command(32,struct.pack('<IIIIIBBBB',1,0x652,0x752,0,0x40,0,0,0,0),1)
    client.command(48,struct.pack('<IIHHI',1,0x40,len(payload),0,1500)+payload,1)
    deadline=time.monotonic()+5
    while time.monotonic()<deadline:
        client.poll()
        if any(e.opcode==258 or (e.opcode==259 and e.request) for e in client.events):break
    result['events']=[event_dict(e) for e in client.events]
    client.acknowledge();client.command(17,generation=1);client.close()
    if not any(e.opcode==258 for e in client.events):raise RuntimeError('No complete BSI response: '+json.dumps(result))
    return result

def main():
    parser=argparse.ArgumentParser(description=__doc__)
    parser.add_argument('command',choices=['probe','usb-test','usb-recovery','devices','read-bsi'])
    parser.add_argument('--payload',default='22f080',help='Explicit read-only UDS request for read-bsi (default 22 F0 80)')
    parser.add_argument('--output',help='Write JSON result to this file')
    args=parser.parse_args()
    if args.command=='devices':
        result={'winusb_paths':WindowsUSB.paths()} if os.name=='nt' else {'backend':'libusb','vid':'1209','pid':'0001'}
    elif args.command=='usb-recovery':
        result=usb_recovery()
    else:
        client=Client(open_transport())
        try:
            if args.command=='probe':result=probe(client);client.close()
            elif args.command=='usb-test':result=usb_test(client)
            else:
                payload=bytes.fromhex(args.payload)
                if payload[:1] not in (b'\x22',b'\x19'):
                    raise ValueError('read-bsi accepts UDS ReadDataByIdentifier (22) or ReadDTCInformation (19) only')
                if len(payload)<2 or len(payload)>64:raise ValueError('Invalid read payload length')
                result=read_bsi(client,payload)
        finally:client.disconnect()
    text=json.dumps(result,indent=2)
    if args.output:
        from pathlib import Path
        Path(args.output).write_text(text+'\n',encoding='utf-8')
    print(text)
if __name__=='__main__':
    try:main()
    except (OSError,RuntimeError,ValueError,TimeoutError) as exc:
        print(str(exc),file=sys.stderr);sys.exit(1)
