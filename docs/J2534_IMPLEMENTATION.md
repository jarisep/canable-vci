# J2534 implementation contract

Version 1, stage 1, 2026-10-11. Requirements for the C++ DLL and Windows service. **Observed** means captured calls; **static** means current loader/source/binary evidence; **contract** means a design decision. Not every required call was captured with a vehicle.

## ABI and loading

Initial DLL: PE32/x86, C exports, `__stdcall`/`WINAPI`, 04.04 signatures. Export all 14 standard PassThru functions with plain names. The previous proxy's .def provides names and x86 stack sizes; decorated `_PassThru...@N` aliases may serve development tools. No C++ exception crosses the C ABI.

Windows ULONG/unsigned long remains 32-bit on x64. Linux unsigned long is unsuitable for portable wire structures. Neither pointers nor raw PASSTHRU_MSG structures cross USB or process boundaries.

| Structure | x86 layout |
| --- | --- |
| PASSTHRU_MSG | 4152 bytes: six uint32 fields at 0/4/8/12/16/20; Data[4128] at 24 |
| SCONFIG | 8 bytes: Parameter, Value |
| SCONFIG_LIST | 8 bytes: uint32 count, pointer at 4 |
| SPARAM, observed device query | 12 bytes: uint32 Parameter, Value, Supported |
| SPARAM_LIST | 8 bytes: count, pointer at 4 |

Add sizeof/offsetof compile checks. Do not advertise x64 without its own build/tests and pointer-list layouts.

## Functions

| Function | Required behavior | Evidence |
| --- | --- | --- |
| Open / Close | Own a service session and device; return DeviceID. Close channels, periodic jobs, and waiters. | Observed identification Open/Close before diagnostic session. |
| Connect / Disconnect | Accept 0x8005, 500000, Flags=0; map ISO15765=7 to the same core, returning the caller's ProtocolID. | 124 observed 0x8005 channels; protocol 7 is a contract. |
| ReadMsgs | FIFO events; return actual NumMsgs even with ERR_TIMEOUT. Call timeout does not destroy RX. | 94 RX rows from calls returning 9, including extended TX indications. |
| WriteMsgs | Validate messages, transfer payloads to firmware. Timeout=0 queues; USB acceptance is not physical completion. Follow 04.04 NumMsgs semantics. | Normal/extended and multi-frame TX observed. |
| StartMsgFilter / StopMsgFilter | FLOW_CONTROL_FILTER maps exact RX ID/address to TX/FC ID/address, bound before TX. Advertise one-route limit. | Exports required; exact masks/count absent. |
| StartPeriodicMsg / StopPeriodicMsg | Device scheduling, channel ownership, bounded resources, safe stop. Initial target two jobs, payload <=64. | Exports required; usage count unlogged. |
| Ioctl | Matrix below; log vendor/unknown numbers and safely decoded fields. | Loader, proxy, voltage analysis. |
| ReadVersion | API 04.04, own DLL/firmware versions, bounded ASCII output. | Mediator PCM_GetVersion identification. |
| GetLastError | Bounded human-readable error; background workers must not overwrite another caller's error. | Required export. |
| SetProgrammingVoltage | Hardware unsupported. May accept PIN_OFF cleanup; activation returns ERR_NOT_SUPPORTED. | Export required; hardware absent. |

PassThruGetAdapterInfo is not a required new export: current PCMToJ2534 does not load it. Do not write guessed content to an unknown output structure.

The service owns USB/state. Named-pipe messages contain session/request/opcode and bounded copied fields. Restart wakes blocked calls with device errors, invalidates handles, and requires Open again. Synchronize concurrent Read/Write/Ioctl/Disconnect; disconnect must release waiting readers.

## IOCTL and configuration

| Identifier | Requirement |
| --- | --- |
| GET_CONFIG=1 / SET_CONFIG=2 | Channel SCONFIG_LIST; validate counts/pointers; unsupported parameters return errors. |
| J1962_PINS=0x8001 parameter | Accept 0308 and alias 060E, return last selection. Proposed default 060E is unobserved. Physical pins always 3/8. Reject other pairs. |
| READ_VBATT=3 | DeviceID, NULL input, uint32 millivolt output; independent emulated vbatt, default 13500. |
| Vendor IOCTL 0x800E | NULL input, uint32 mV; forced_on=13500, manual_off=0. Own Mediator bridge reads the same independent service state without a foreign Vendor identity. |
| READ_PROG_VOLTAGE=0x0E | Separate standard query; no ignition control or physical programming voltage support. |
| CLEAR_TX_BUFFER=7 / CLEAR_RX_BUFFER=8 | Clear Windows queues through firmware barriers; events at/before barrier sequence cannot repopulate cleared contents. |
| CLEAR_PERIODIC_MSGS=9 / CLEAR_MSG_FILTERS=0x0A | Safely remove jobs/routes; release handles after acknowledgment. |
| GET_DEVICE_INFO=0x800C | Observed NULL input, output SPARAM_LIST with one 12-byte SPARAM, Parameter=1. Own stable uint32 serial, Supported=1; do not copy Bosch identity. |
| GET_PROTOCOL_INFO=0x800D | Not observed. Implement only verified ABI/parameters; report actual limits; log before extending. |
| Other protocols/IOCTLs | Appropriate unsupported/invalid error. Do not advertise K-line, SW-CAN, or DoIP. |

SCONFIG DATA_RATE=1, LOOPBACK=3, ISO15765_BS=0x1E, STMIN=0x1F, WFT_MAX=0x25 are implemented or explicitly limited. STmin accepts 00–7F milliseconds or F1–F9 (100–900 µs). Reject reserved values; USB preserves the code.

STMIN_TX=0x23, BS_TX=0x22, vendor N_AS/N_BS/N_CR, N_BR_MIN/N_CS_MIN are incompletely captured. PCMToJ2534.ini contains provider-specific BS/STMIN/N_BR/N_CS values; the binary adapts STMIN_TX. Implement proven forms or return unsupported with absent capability. Storing a GET_CONFIG value is not timing support. USB SET_TIMING allows timeouts; reject differing fixed Kconfig values until runtime adaptation exists.

Log export, handles, protocol, Timeout, requested/returned NumMsgs, filter mask/pattern/FC, periodic interval, IOCTL, and decoded SCONFIG/SPARAM lists. Do not dump unknown pointers. This supplies missing initialization evidence without restoring Xentry VCI vehicle wiring.

## Messages and events

J2534 Data starts with a four-byte network-order CAN ID, then an extended address if enabled, then payload. USB separates identifiers/addresses from payload and excludes PCI bytes. Maximum DataSize is 4099 normal or 4100 extended, despite the 4128-byte ABI array. Reject oversized/truncated data.

| Event | J2534 representation |
| --- | --- |
| TX_SUBMIT OK | Buffer acceptance only; no RX message or TX-completion indication. |
| Physical ISO-TP TX success | RX queue, TX_MSG_TYPE(1) | TX_INDICATION(8)=0x09; ID and optional address only. |
| Accepted multi-frame First Frame | START_OF_MESSAGE=0x02; RX ID and optional address; once per transfer. |
| Complete SF/message | ID + optional address + complete payload; no separate SF RX_START. |
| Actual RX padding error | Add ISO15765_PADDING_ERROR=0x10 based on frame metadata. Mediator accepted many such trace replies. |
| Extended addressing | Add ISO15765_ADDR_TYPE=0x80; observed 89/82/90/80. |
| 29-bit ID | CAN_29BIT_ID=0x100 only with capability and tests; unobserved in Bosch trace. |

Decode TX FRAME_PAD=0x40, ADDR_TYPE=0x80, and CAN_29BIT_ID=0x100 separately. Do not silently ignore other flags. RX 0x10 is a padding error, not ordinary padding. [04.04 header reference](https://github.com/joeyoravec/j2534-logger/blob/master/ptshim32/j2534_v0404.h)

Timestamp is uint32 microseconds from the session's device monotonic basis modulo 2^32. Absolute Bosch timestamps are not expected new-device values. ExtraDataIndex was unlogged; set it to DataSize for messages without extra bytes. [PEAK 04.04 manual](https://www.peak-system.com/produktcd/Pdf/English/PCAN-PassThru-API_UserMan_eng.pdf). Verify in the actual DLL ABI and Mediator tests.

UDS negative replies such as 7F 10 12 are ordinary RX data, not J2534 function errors. Separate ISO-TP failure, absent ECU reply, and Windows read timeout. A partially satisfied ReadMsgs returns messages, NumMsgs>0, and ERR_TIMEOUT=9. Timeout=0 with an empty queue returns ERR_BUFFER_EMPTY=0x10.

Initial Windows RX bound: 64 complete messages / 256 KiB plus bounded metadata events. Report overflow; do not invisibly discard oldest messages. Clear/disconnect release waiters safely. New channel generations must not consume previous-generation events.

## Stage 3 acceptance

1. Mediator loads all 14 exports and runs identification Open/ReadVersion/GET_DEVICE_INFO/Close with correct own identity.
2. The new trace records 0x8005/500000, pins, actual filters/periodic/configuration; add observed extensions without hiding unsupported errors.
3. Nine Proace fixtures pass normalized event comparisons. Padding differences require real DLC/padding evidence; do not mask all status bits.
4. Test forced_on, independent vbatt, and unplug. The own CANable bridge returns independent TypeVoltage=3 state with an honest Vendor; examine other vendor-dependent paths.
5. ABI, timeout, partial-read, filter, periodic, clear, cancellation, and process-loss tests pass without a vehicle. Memory and hardware timing belong to firmware/vehicle verification.
