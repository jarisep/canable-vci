# SPDX-License-Identifier: Apache-2.0
"""CANable whole-message protocol. No hardware access occurs on import."""
from dataclasses import dataclass
import struct
import secrets
import time
import queue
import threading
HEADER = struct.Struct('<4sBBHIIIIQ')
MAX_BODY = 4160
GUID = '{5D4E7C68-9BC1-4E2B-9D68-5C9AB801D9F2}'
RESULTS = ['OK','BAD_VERSION','BAD_LENGTH','BAD_SESSION','BAD_CHANNEL','UNSUPPORTED',
           'INVALID_VALUE','BUSY','NO_RESOURCE','NOT_CONNECTED','INTERNAL_ERROR','BAD_REQUEST_ORDER']
@dataclass
class Message:
    kind: int
    opcode: int
    session: int
    generation: int
    request: int
    device_time_us: int
    body: bytes
    def encode(self):
        if len(self.body) > MAX_BODY:
            raise ValueError('Body exceeds protocol limit')
        return HEADER.pack(b'CISO',1,self.kind,self.opcode,len(self.body),self.session,
                           self.generation,self.request,self.device_time_us) + self.body
class Decoder:
    def __init__(self,resync=False):
        self.buffer=bytearray()
        self.resync=resync
    def feed(self, data):
        self.buffer.extend(data)
        result=[]
        while len(self.buffer)>=HEADER.size:
            magic,version,kind,op,n,session,gen,req,stamp=HEADER.unpack_from(self.buffer)
            if magic!=b'CISO' or version!=1 or kind not in (1,2,3) or n>MAX_BODY:
                if self.resync:
                    # Only before ownership handshake: the previous reader may
                    # have consumed a header but left its body in the IN pipe.
                    start=self.buffer.find(b'CISO',1)
                    if start<0:
                        del self.buffer[:-3]
                        break
                    del self.buffer[:start]
                    continue
                self.buffer.clear()
                raise ValueError('Invalid protocol header; reconnect required')
            if len(self.buffer)<HEADER.size+n:
                break
            result.append(Message(kind,op,session,gen,req,stamp,bytes(self.buffer[32:32+n])))
            del self.buffer[:32+n]
        return result
class Client:
    def __init__(self, transport):
        self.transport=transport
        self.decoder=Decoder(resync=True)
        self.session=0
        self.request=secrets.randbelow(0x7fffffff)
        self.events=[]
        self.inbox=queue.Queue(maxsize=64)
        self.stop=threading.Event()
        self.reader_error=None
        self.reader=threading.Thread(target=self._read,name='ciso-usb-reader',daemon=True)
        self.lease_ms=5000
        self.last_heartbeat=time.monotonic()
        self.reader.start()
    def _read(self):
        # Drain IN while the owner writes OUT, including fragmented commands.
        try:
            while not self.stop.is_set():
                for msg in self.decoder.feed(self.transport.read(timeout_ms=100)):
                    try:self.inbox.put_nowait(msg)
                    except queue.Full:raise RuntimeError('USB receive queue full; reconnect without replay')
        except Exception as exc:
            if not self.stop.is_set():self.reader_error=exc
    def _next(self, timeout):
        if self.reader_error:raise self.reader_error
        try:return self.inbox.get(timeout=timeout)
        except queue.Empty:
            if self.reader_error:raise self.reader_error
            return None
    def _event(self,msg):
        if len(self.events)>=128:raise RuntimeError('Development event history full; reconnect without replay')
        self.events.append(msg)
    def command(self, opcode, body=b'', generation=0, timeout=2, fragment=0, check=True):
        if self.request==0xffffffff:raise RuntimeError('Request IDs exhausted; establish a new session')
        self.request+=1
        request=self.request
        frame=Message(1,opcode,self.session,generation,request,0,body).encode()
        step=fragment or len(frame)
        for at in range(0,len(frame),step):
            self.transport.write(frame[at:at+step])
        deadline=time.monotonic()+timeout
        while time.monotonic()<deadline:
            msg=self._next(min(0.1,max(0,deadline-time.monotonic())))
            if msg is None:continue
            if msg.kind==3:
                if self.session:self._event(msg)
                continue
            if msg.kind!=2 or msg.request!=request or msg.opcode!=opcode:
                if opcode==1 and not self.session:continue
                raise RuntimeError('Unexpected response; reconnect without replaying TX')
            if len(msg.body)<4:
                raise RuntimeError('Truncated response')
            code=struct.unpack_from('<I',msg.body)[0]
            if check and code:
                label=RESULTS[code] if code<len(RESULTS) else str(code)
                raise RuntimeError(f'Opcode {opcode}: {label}')
            return msg
        raise TimeoutError(f'No response for opcode {opcode}; TX must not be replayed automatically')
    def hello(self, lease_ms=5000):
        nonce=secrets.randbits(64)
        msg=self.command(1,struct.pack('<QII',nonce,lease_ms,0),fragment=7)
        if len(msg.body)!=32:
            raise RuntimeError('Invalid HELLO response size')
        result,echo,boot,self.session,self.lease_ms,revision=struct.unpack('<IQQIII',msg.body)
        if echo!=nonce or not self.session:
            raise RuntimeError('HELLO identity mismatch')
        self.decoder.resync=False
        self.last_heartbeat=time.monotonic()
        return {'boot_id':boot,'session_id':self.session,'lease_ms':self.lease_ms,'caps_revision':revision}
    def capabilities(self):
        msg=self.command(2)
        if len(msg.body)!=76:
            raise RuntimeError('Invalid capability response size')
        values=struct.unpack('<IQ16I',msg.body)
        names=['max_payload','physical_buses','channels','routes','tx_transfers','rx_transfers',
               'periodic_slots','periodic_max_payload','control_event_slots','max_body','bitrate',
               'n_as_ms','n_bs_ms','n_cr_ms','timestamp_quality','wft_max']
        return {'features':values[1],**dict(zip(names,values[2:]))}
    def poll(self, timeout_ms=100):
        msg=self._next(timeout_ms/1000)
        while msg is not None:
            if msg.kind!=3:
                raise RuntimeError('Unexpected response outside a command')
            self._event(msg)
            msg=self._next(0)
        if time.monotonic()-self.last_heartbeat>self.lease_ms/3000:
            self.command(3)
            self.last_heartbeat=time.monotonic()
    def acknowledge(self):
        if self.events:
            seq=struct.unpack_from('<I',self.events[-1].body)[0]
            self.command(80,struct.pack('<I',seq))
    def close(self):
        try:
            if self.session:
                self.command(4)
                self.session=0
        finally:self.disconnect()
    def disconnect(self):
        self.stop.set()
        self.reader.join(timeout=2)
        if self.reader.is_alive():raise RuntimeError('USB reader did not stop; handle retained')
        self.transport.close()
