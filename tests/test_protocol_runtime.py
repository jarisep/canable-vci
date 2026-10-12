# SPDX-License-Identifier: Apache-2.0
"""Host stream framing checks; no USB devices are opened."""
from pathlib import Path
import json
import struct
import sys
import unittest
import queue
import threading
sys.path.insert(0,str(Path(__file__).resolve().parents[1]/'tools'))
from ciso_protocol import Client, Decoder, Message
class ProtocolRuntimeTests(unittest.TestCase):
    def test_fragmented_and_coalesced_messages(self):
        messages=[Message(1,1,0,0,1,0,bytes(16)),Message(3,258,4,7,0,99,bytes(4119))]
        raw=b''.join(m.encode() for m in messages)
        for width in (1,7,32,64,10000):
            decoder=Decoder();found=[]
            for at in range(0,len(raw),width):found.extend(decoder.feed(raw[at:at+width]))
            self.assertEqual(found,messages)
    def test_rejects_oversize_before_allocating_payload(self):
        raw=struct.pack('<4sBBHIIIIQ',b'CISO',1,1,48,4161,1,1,1,0)
        decoder=Decoder()
        with self.assertRaises(ValueError):decoder.feed(raw)
        self.assertEqual(decoder.buffer,b'')
    def test_rejects_magic_version_kind(self):
        for magic,version,kind in [(b'FAIL',1,1),(b'CISO',2,1),(b'CISO',1,0)]:
            with self.assertRaises(ValueError):Decoder().feed(struct.pack('<4sBBHIIIIQ',magic,version,kind,1,0,0,0,1,0))
    def test_encode_limit(self):
        with self.assertRaises(ValueError):Message(1,48,1,1,1,0,bytes(4161)).encode()
    def test_reader_drains_events_while_command_is_still_being_written(self):
        class Duplex:
            def __init__(self):
                self.queue=queue.Queue();self.drained=threading.Event();self.decoder=Decoder();self.closed=False;self.first=True
            def write(self,data):
                if self.first:
                    self.first=False
                    self.queue.put(Message(3,260,1,1,0,0,struct.pack('<I',1)).encode())
                    if not self.drained.wait(0.5):raise RuntimeError('IN event blocked OUT')
                for msg in self.decoder.feed(data):
                    self.queue.put(Message(2,msg.opcode,msg.session,msg.generation,msg.request,0,bytes(4)).encode())
            def read(self,timeout_ms):
                try:data=self.queue.get(timeout=timeout_ms/1000)
                except queue.Empty:return b''
                self.drained.set();return data
            def close(self):self.closed=True
        transport=Duplex();client=Client(transport);client.session=1;client.decoder.resync=False
        try:
            client.command(48,bytes(4111),fragment=61)
            self.assertEqual(len(client.events),1)
        finally:client.disconnect()
        self.assertTrue(transport.closed)
        self.assertFalse(client.reader.is_alive())
    def test_background_read_error_is_reported_without_replaying_command(self):
        class Broken:
            def __init__(self):self.writes=0
            def write(self,data):self.writes+=1
            def read(self,timeout_ms):raise OSError('injected USB loss')
            def close(self):pass
        transport=Broken();client=Client(transport)
        try:
            with self.assertRaisesRegex(OSError,'injected USB loss'):client.command(2)
            self.assertEqual(transport.writes,1)
        finally:client.disconnect()
    def test_new_owner_skips_partial_old_event_and_stale_response(self):
        class PreviousOwner:
            def __init__(self):
                self.queue=queue.Queue();self.decoder=Decoder()
                self.queue.put(bytes(16)+Message(3,260,3,1,0,1,bytes(16)).encode()+
                               Message(2,2,3,0,1,1,bytes(4)).encode())
            def write(self,data):
                for msg in self.decoder.feed(data):
                    nonce,lease,_=struct.unpack('<QII',msg.body)
                    body=struct.pack('<IQQIII',0,nonce,77,7,lease,1)
                    self.queue.put(Message(2,1,0,0,msg.request,1,body).encode())
            def read(self,timeout_ms):
                try:return self.queue.get(timeout=timeout_ms/1000)
                except queue.Empty:return b''
            def close(self):pass
        client=Client(PreviousOwner())
        try:
            self.assertEqual(client.hello()['session_id'],7)
            self.assertEqual(client.events,[])
            self.assertFalse(client.decoder.resync)
        finally:client.disconnect()
if __name__=='__main__':unittest.main()
