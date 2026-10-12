# SPDX-License-Identifier: Apache-2.0
"""Linux libusb and Windows WinUSB transports, using only Python's standard library."""
import ctypes as C
import ctypes.util
import os
from ciso_protocol import GUID
VID,PID=0x1209,0x0001

def open_transport():
    return WindowsUSB() if os.name=='nt' else LinuxUSB()

class LinuxUSB:
    def __init__(self):
        self.lib=C.CDLL(ctypes.util.find_library('usb-1.0') or 'libusb-1.0.so.0')
        l=self.lib
        l.libusb_init.argtypes=[C.POINTER(C.c_void_p)]; l.libusb_init.restype=C.c_int
        l.libusb_open_device_with_vid_pid.argtypes=[C.c_void_p,C.c_uint16,C.c_uint16]; l.libusb_open_device_with_vid_pid.restype=C.c_void_p
        l.libusb_claim_interface.argtypes=[C.c_void_p,C.c_int];l.libusb_claim_interface.restype=C.c_int
        l.libusb_get_device.argtypes=[C.c_void_p];l.libusb_get_device.restype=C.c_void_p
        l.libusb_get_active_config_descriptor.argtypes=[C.c_void_p,C.POINTER(C.c_void_p)];l.libusb_get_active_config_descriptor.restype=C.c_int
        l.libusb_free_config_descriptor.argtypes=[C.c_void_p]
        l.libusb_bulk_transfer.argtypes=[C.c_void_p,C.c_ubyte,C.c_void_p,C.c_int,C.POINTER(C.c_int),C.c_uint];l.libusb_bulk_transfer.restype=C.c_int
        l.libusb_release_interface.argtypes=[C.c_void_p,C.c_int];l.libusb_close.argtypes=[C.c_void_p];l.libusb_exit.argtypes=[C.c_void_p]
        self.ctx=C.c_void_p();self.handle=None;self.interface=None
        if l.libusb_init(C.byref(self.ctx)):
            raise RuntimeError('libusb initialization failed')
        self.handle=l.libusb_open_device_with_vid_pid(self.ctx,VID,PID)
        if not self.handle:
            self.close(); raise RuntimeError('CANable ISO-TP device not found or not accessible')
        class Endpoint(C.Structure):
            _fields_=[('length',C.c_ubyte),('type',C.c_ubyte),('address',C.c_ubyte),('attributes',C.c_ubyte),('mps',C.c_uint16),('interval',C.c_ubyte),('refresh',C.c_ubyte),('synch',C.c_ubyte),('extra',C.c_void_p),('extra_length',C.c_int)]
        class InterfaceDescriptor(C.Structure):
            _fields_=[('length',C.c_ubyte),('type',C.c_ubyte),('number',C.c_ubyte),('alternate',C.c_ubyte),('num_endpoints',C.c_ubyte),('class_code',C.c_ubyte),('subclass',C.c_ubyte),('protocol',C.c_ubyte),('string',C.c_ubyte),('endpoints',C.POINTER(Endpoint)),('extra',C.c_void_p),('extra_length',C.c_int)]
        class Interface(C.Structure):
            _fields_=[('alts',C.POINTER(InterfaceDescriptor)),('num_alts',C.c_int)]
        class Config(C.Structure):
            _fields_=[('length',C.c_ubyte),('type',C.c_ubyte),('total',C.c_uint16),('num_interfaces',C.c_ubyte),('value',C.c_ubyte),('string',C.c_ubyte),('attributes',C.c_ubyte),('power',C.c_ubyte),('interfaces',C.POINTER(Interface)),('extra',C.c_void_p),('extra_length',C.c_int)]
        raw=C.c_void_p()
        if l.libusb_get_active_config_descriptor(l.libusb_get_device(self.handle),C.byref(raw)):
            self.close();raise RuntimeError('Cannot query USB configuration')
        cfg=C.cast(raw,C.POINTER(Config)).contents
        self.in_ep=self.out_ep=None
        for i in range(cfg.num_interfaces):
            iface=cfg.interfaces[i]
            for j in range(iface.num_alts):
                alt=iface.alts[j]
                if (alt.class_code,alt.subclass,alt.protocol)!=(255,73,1):continue
                self.interface=alt.number
                for k in range(alt.num_endpoints):
                    ep=alt.endpoints[k]
                    if ep.attributes&3==2:
                        if ep.address&128:self.in_ep=ep.address
                        else:self.out_ep=ep.address
        l.libusb_free_config_descriptor(raw)
        if self.interface is None or self.in_ep is None or self.out_ep is None:
            self.close(); raise RuntimeError('Dedicated ISO-TP interface not found')
        if l.libusb_claim_interface(self.handle,self.interface):
            self.interface=None;self.close();raise RuntimeError('Cannot claim dedicated USB interface')
    def write(self,data):
        buf=C.create_string_buffer(data);done=C.c_int()
        err=self.lib.libusb_bulk_transfer(self.handle,self.out_ep,buf,len(data),C.byref(done),2000)
        if err or done.value!=len(data):raise RuntimeError(f'USB write failed ({err}, {done.value}/{len(data)}); do not replay TX')
    def read(self,timeout_ms=100):
        buf=C.create_string_buffer(64);done=C.c_int()
        err=self.lib.libusb_bulk_transfer(self.handle,self.in_ep,buf,64,C.byref(done),timeout_ms)
        if err not in (0,-7):raise RuntimeError(f'USB read failed ({err})')
        return buf.raw[:done.value]
    def close(self):
        if self.handle:
            if self.interface is not None:self.lib.libusb_release_interface(self.handle,self.interface)
            self.lib.libusb_close(self.handle);self.handle=None
        if self.ctx:self.lib.libusb_exit(self.ctx);self.ctx=None

class WindowsUSB:
    @staticmethod
    def paths():
        from ctypes import wintypes as W
        import uuid
        class Guid(C.Structure):_fields_=[('bytes',C.c_ubyte*16)]
        class Data(C.Structure):_fields_=[('size',W.DWORD),('guid',Guid),('flags',W.DWORD),('reserved',C.c_void_p)]
        api=C.WinDLL('setupapi',use_last_error=True)
        api.SetupDiGetClassDevsW.argtypes=[C.POINTER(Guid),W.LPCWSTR,W.HWND,W.DWORD];api.SetupDiGetClassDevsW.restype=W.HANDLE
        api.SetupDiEnumDeviceInterfaces.argtypes=[W.HANDLE,C.c_void_p,C.POINTER(Guid),W.DWORD,C.POINTER(Data)];api.SetupDiEnumDeviceInterfaces.restype=W.BOOL
        api.SetupDiGetDeviceInterfaceDetailW.argtypes=[W.HANDLE,C.POINTER(Data),C.c_void_p,W.DWORD,C.POINTER(W.DWORD),C.c_void_p];api.SetupDiGetDeviceInterfaceDetailW.restype=W.BOOL
        api.SetupDiDestroyDeviceInfoList.argtypes=[W.HANDLE]
        g=Guid();g.bytes[:]=uuid.UUID(GUID.strip('{}')).bytes_le
        devs=api.SetupDiGetClassDevsW(C.byref(g),None,None,0x12)
        if devs==C.c_void_p(-1).value:raise C.WinError(C.get_last_error())
        result=[]
        try:
            index=0
            while True:
                d=Data();d.size=C.sizeof(d)
                if not api.SetupDiEnumDeviceInterfaces(devs,None,C.byref(g),index,C.byref(d)):
                    if C.get_last_error()==259:break
                    raise C.WinError(C.get_last_error())
                needed=W.DWORD()
                api.SetupDiGetDeviceInterfaceDetailW(devs,C.byref(d),None,0,C.byref(needed),None)
                raw=C.create_string_buffer(needed.value)
                C.cast(raw,C.POINTER(W.DWORD))[0]=8 if C.sizeof(C.c_void_p)==8 else 6
                if not api.SetupDiGetDeviceInterfaceDetailW(devs,C.byref(d),raw,needed,C.byref(needed),None):raise C.WinError(C.get_last_error())
                result.append(C.wstring_at(C.addressof(raw)+4))
                index+=1
        finally:api.SetupDiDestroyDeviceInfoList(devs)
        return result
    def __init__(self):
        from ctypes import wintypes as W
        self.file=None;self.handle=C.c_void_p()
        self.kernel=C.WinDLL('kernel32',use_last_error=True);self.lib=C.WinDLL('winusb',use_last_error=True)
        paths=self.paths()
        if len(paths)!=1:raise RuntimeError(f'Expected one CANable ISO-TP interface, found {len(paths)}')
        self.kernel.CreateFileW.argtypes=[W.LPCWSTR,W.DWORD,W.DWORD,C.c_void_p,W.DWORD,W.DWORD,W.HANDLE];self.kernel.CreateFileW.restype=W.HANDLE
        self.kernel.CloseHandle.argtypes=[W.HANDLE]
        self.lib.WinUsb_Initialize.argtypes=[W.HANDLE,C.POINTER(C.c_void_p)];self.lib.WinUsb_Initialize.restype=W.BOOL
        self.lib.WinUsb_Free.argtypes=[C.c_void_p]
        class Interface(C.Structure):_pack_=1;_fields_=[('length',C.c_ubyte),('type',C.c_ubyte),('number',C.c_ubyte),('alternate',C.c_ubyte),('endpoints',C.c_ubyte),('class_code',C.c_ubyte),('subclass',C.c_ubyte),('protocol',C.c_ubyte),('string',C.c_ubyte)]
        class Pipe(C.Structure):_fields_=[('type',C.c_int),('id',C.c_ubyte),('mps',C.c_uint16),('interval',C.c_ubyte)]
        self.lib.WinUsb_QueryInterfaceSettings.argtypes=[C.c_void_p,C.c_ubyte,C.POINTER(Interface)];self.lib.WinUsb_QueryInterfaceSettings.restype=W.BOOL
        self.lib.WinUsb_QueryPipe.argtypes=[C.c_void_p,C.c_ubyte,C.c_ubyte,C.POINTER(Pipe)];self.lib.WinUsb_QueryPipe.restype=W.BOOL
        self.lib.WinUsb_SetPipePolicy.argtypes=[C.c_void_p,C.c_ubyte,W.ULONG,W.ULONG,C.c_void_p];self.lib.WinUsb_SetPipePolicy.restype=W.BOOL
        for name in ['WinUsb_ReadPipe','WinUsb_WritePipe']:
            f=getattr(self.lib,name);f.argtypes=[C.c_void_p,C.c_ubyte,C.c_void_p,W.ULONG,C.POINTER(W.ULONG),C.c_void_p];f.restype=W.BOOL
        self.file=self.kernel.CreateFileW(paths[0],0xc0000000,3,None,3,0x40000000,None)
        if self.file==C.c_void_p(-1).value:self.file=None;raise C.WinError(C.get_last_error())
        if not self.lib.WinUsb_Initialize(self.file,C.byref(self.handle)):
            self.close();raise C.WinError(C.get_last_error())
        settings=Interface()
        if not self.lib.WinUsb_QueryInterfaceSettings(self.handle,0,C.byref(settings)):
            self.close();raise C.WinError(C.get_last_error())
        if (settings.class_code,settings.subclass,settings.protocol)!=(255,73,1):
            self.close();raise RuntimeError('Wrong USB interface identity')
        self.in_ep=self.out_ep=None
        for i in range(settings.endpoints):
            pipe=Pipe()
            if not self.lib.WinUsb_QueryPipe(self.handle,0,i,C.byref(pipe)):
                self.close();raise C.WinError(C.get_last_error())
            if pipe.type==2:
                if pipe.id&128:self.in_ep=pipe.id
                else:self.out_ep=pipe.id
        if self.in_ep is None or self.out_ep is None:self.close();raise RuntimeError('Bulk endpoints missing')
    def policy(self,ep,timeout_ms):
        from ctypes import wintypes as W
        value=W.ULONG(timeout_ms)
        if not self.lib.WinUsb_SetPipePolicy(self.handle,ep,3,C.sizeof(value),C.byref(value)):raise C.WinError(C.get_last_error())
    def write(self,data):
        from ctypes import wintypes as W
        self.policy(self.out_ep,2000);buf=C.create_string_buffer(data);done=W.ULONG()
        if not self.lib.WinUsb_WritePipe(self.handle,self.out_ep,buf,len(data),C.byref(done),None) or done.value!=len(data):raise C.WinError(C.get_last_error())
    def read(self,timeout_ms=100):
        from ctypes import wintypes as W
        self.policy(self.in_ep,timeout_ms);buf=C.create_string_buffer(64);done=W.ULONG()
        if not self.lib.WinUsb_ReadPipe(self.handle,self.in_ep,buf,64,C.byref(done),None):
            if C.get_last_error()==121:return b''
            raise C.WinError(C.get_last_error())
        return buf.raw[:done.value]
    def close(self):
        if self.handle:self.lib.WinUsb_Free(self.handle);self.handle=C.c_void_p()
        if self.file:self.kernel.CloseHandle(self.file);self.file=None
