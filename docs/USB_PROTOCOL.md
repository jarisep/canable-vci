# CANable ISO-TP USB protocol v1

Stage 1 contract, 2026-10-11. The [machine-readable contract](../protocol/usb-isotp-v1.json) specifies exact field identifiers and sizes. This dedicated protocol is not implemented by upstream CANnectivity.

## Transport and ownership

The Windows service opens one vendor-specific WinUSB interface with one bulk OUT and IN endpoint. Query endpoint addresses from descriptors. Interface GUID is `{5D4E7C68-9BC1-4E2B-9D68-5C9AB801D9F2}`, class FF, subclass 49, protocol 1. Include the GUID in MS OS descriptors. Verify VID/PID and driver binding instead of assuming an old gs_usb instance is compatible.

The initial diagnostic interface exclusively owns CAN; preserve upstream gs_usb as a separate build. Simultaneous interfaces require later endpoint/memory/ownership work. Windows sends whole messages, rather than scheduling CAN frames.

USB requests and 64-byte packets are not protocol message boundaries. Assemble a 32-byte header, validate length, then consume exactly its body. Fragmented headers/bodies and coalesced messages are valid. Do not interleave message bytes in one direction.

Invalid magic or excessive length resets protocol ownership before allocation. Unsupported versions return BAD_VERSION if the remaining header is usable; unknown opcodes return UNSUPPORTED without changing configuration. Partial messages expire with the lease; later bytes cannot resume an old TX.

## Header and responses

All integers are little endian without padding. Format `<4sBBHIIIIQ`, 32 bytes:

| Offset | Field | Meaning |
| ---: | --- | --- |
| 0 | magic[4] | ASCII CISO |
| 4 | version u8 | 1 |
| 5 | kind u8 | COMMAND=1, RESPONSE=2, EVENT=3 |
| 6 | opcode u16 | JSON operation ID |
| 8 | body_bytes u32 | Excludes header; maximum 4160 |
| 12 | session_id u32 | HELLO=0; otherwise negotiated ID |
| 16 | channel_generation u32 | Device/session commands=0; otherwise host-selected increasing generation |
| 20 | request_id u32 | Commands >0; echoed responses; independent RX events=0 |
| 24 | device_time_us u64 | Commands=0; device monotonic time in responses/events |

Identifiers are neither pointers nor raw J2534 handles. Request IDs and generations never wrap within a session. Renew the session on exhaustion. Never deliver old-generation events to a new channel.

Every response begins with uint32 result, OK=0. Errors are protocol codes, not raw J2534/Zephyr errors. Echo opcode/request/channel; error bodies contain only result. Successful extra fields follow JSON reply_format; otherwise body is four bytes.

## Commands

| Command | Behavior |
| --- | --- |
| HELLO | Session/channel=0, host nonce, lease. Return nonce, boot ID, new session, actual lease, capability revision. Do not replace an active owner. |
| GET_CAPS | Actual features, buffers, route/transfer/periodic limits, timeouts, timestamp quality. Unsupported features cannot claim success. |
| GET_INFO | MCU UID[12], bounded firmware/build/board strings. Host derives an own stable uint32 serial. No ignition emulation. |
| HEARTBEAT / SESSION_CLOSE | Renew lease, or stop all owned work safely. |
| CHANNEL_OPEN / CLOSE | Protocol 7/0x8005, initially Flags=0 and 500000; one generation. Stop jobs/TX/RX before close acknowledgment. |
| SET_TIMING | N_As/N_Bs/N_Cr ms, WFT, RX FC BS/STmin. Change only while idle; reject unsupported runtime values. |
| ROUTE_ADD / REMOVE | Exact RX and TX/FC IDs, independent RX/TX extended addresses, addressing flags, FC BS/STmin and padding. One bound RX route; change only while idle. |
| TX_SUBMIT | Route, J2534 flags, length, deadline, complete payload <=4095. Assemble before CAN TX. |
| TX_CANCEL | Abort accepted request; acknowledge after new frames stop and callbacks quiesce. Already transmitted frames cannot be undone. |
| CLEAR | TX/RX/PERIODIC/ROUTES mask, barrier acknowledgment and last_event_seq. |
| PERIODIC_ADD / REMOVE | Device monotonic scheduler; initial target two jobs, payload <=64. |
| EVENT_ACK | Cumulative sequence after copying events into the service queue; frees retained RX storage, independent of client ReadMsgs. |
| GET_STATUS | Bus state, error counts, overflow/dropped RX, remaining lease; no invented ignition state. |

ROUTE_ADD implements exact FLOW_CONTROL_FILTER mapping; do not advertise partial masks or multiple routes before implementation. TX extended/29-bit flags must match the route. FRAME_PAD=0x40 selects zero-padding for that TX; FC has its own route padding setting. Reject reserved/inconsistent values. Periodic interval range is 5000–65535000 µs (5–65535 ms); oversized payloads fail capacity checks.

TX deadline_ms limits total job age from device acceptance. Zero means no separate overall deadline, while ISO-TP timers still apply. Do not automatically cancel jobs because J2534 WriteMsgs Timeout=0 queued them. Transfer timers are distinct from UDS P2/P2* response timing.

## Events, ordering, and barriers

Each event has an increasing session-wide sequence. TX_DONE/RX_START/RX_COMPLETE include route, transfer, RxStatus, CAN ID, extended address, and payload length. TRANSFER_ERROR adds stable reason and raw Zephyr error. USB CAN IDs are little endian; host J2534 Data uses four-byte big endian.

- TX_DONE occurs only after successful CAN transmission. Normal transfer ID and header request equal TX_SUBMIT request. No payload; host generates an ID/address-only indication.
- RX_START follows accepted valid FF and precedes full RX. Its length is the expected ISO-TP payload. No SF start event.
- RX_COMPLETE carries exactly one full payload. RX transfer IDs are device-generated, not guessed from the last TX. SF emits only this event.
- TRANSFER_ERROR terminates a failed TX/RX once; cancellation has its own reason. No later TX_DONE for a failed/cancelled request. Do not publish partial RX as complete.
- BUS_STATE/OVERFLOW are bounded events; GET_STATUS retains counters even when host cannot consume events.

Preserve CAN/ISO-TP observation order. Serialize TX_SUBMIT OK before that request's terminal event, even for immediate CAN completion. Reception does not wait for USB acceptance delivery: the route is already bound and RX storage reserved.

Every accepted TX produces one terminal event. Periodic event header request is its creation request; transfer ID is a separate per-job instance counter. Distinguish these from ordinary TX requests.

CHANNEL_CLOSE, ROUTE_REMOVE, TX_CANCEL, and CLEAR successful replies contain last_event_seq. Produce affected pre-barrier events before acknowledgment. Finish an already-started USB IN message before freeing its resources. Host may suppress obsolete events while acknowledging them. RX/ROUTES clearing must not destroy unrelated TX unless that TX uses a removed route.

## Memory, backpressure, and loss

Advertise actual GET_CAPS bounds. Target one TX, one RX, one route. Receive USB payload directly into reserved TX storage without a separate 4160-byte body copy. Retain complete RX net_buf chains until EVENT_ACK and stream them over USB without another full RX copy.

BS=0 requires a pool covering the whole message: the old 4x512 pool is insufficient. Stage 2 starts with >=4096 RX bytes plus SF/FF capacity and enough next-frame processing resources. Include net_buf headers, USB buffers, stacks, and event metadata in a new measurement; the earlier 28256-byte RAM probe is not the final service measurement.

Target eight small control-event slots and one retained complete RX. Reserve terminal capacity before accepting TX. BUSY/NO_RESOURCE must not destroy active work. Exhausted RX sends appropriate FC overflow on FF; dropped SF increments a counter. Report overflow and abort safely if a required terminal cannot fit; never pretend success.

The service copies full RX before EVENT_ACK. Host slowness may delay the next request but must not change FC timing for an ongoing transfer. ISO-TP timers run independently of USB workers. Periodic jobs skip busy slots and increment a counter rather than accumulating unbounded queued copies.

Default lease 5000 ms, heartbeat 1000 ms; allowed lease 500–30000 ms. Reset, unplug, close, or expiry stops periodic/new CAN work, quiesces callbacks, and frees RX. Leave no active channel. Only HEARTBEAT renews lease; partial messages do not.

Require increasing request IDs; reject earlier/repeated IDs with BAD_REQUEST_ORDER. Never automatically replay a TX after uncertain acceptance. Recover with a new HELLO, generation, and routes rather than silently resuming old work.

## Bounded Zephyr adaptation

Locked revisions are in [west.yml](../west.yml) and the [build helper](../tools/build_firmware.py). Sources: [CANnectivity](https://github.com/CANnectivity/cannectivity), [Zephyr ISO-TP](https://docs.zephyrproject.org/latest/services/connectivity/canbus/isotp.html).

| Requirement | Upstream status / adaptation |
| --- | --- |
| Normal/extended TX/RX | isotp_send, isotp_bind, isotp_recv_net; separate RX/TX addresses. |
| RX_START/timestamp | Public receive API lacks accepted-FF event; add bounded metadata hook, not a parallel ISO-TP state machine. |
| BS>0 RX | Upstream recv_net may return a block plus remaining length. Stage 2 reserves and assembles the full chain while retaining FC block boundaries, avoiding unused fragments per small block. |
| Per-TX padding / RX metadata | Upstream settings are global. Add per-transfer option and DLC metadata; assembled payload cannot prove 0x10 padding errors. |
| Actual CAN TX error | Audited send_can_tx_cb ignores error. Propagate failure; wait for final callbacks before success. |
| Cancel/clear | RX unbind exists, public TX cancellation does not. Add safe abort/quiescence rather than forgetting request IDs. |
| N_* / WFT | Fixed Kconfig in probe; implement context values or report unsupported. Do not confuse BS/STmin with timeouts. |
| Timestamp quality | Report controller/callback/worker origin; use 64-bit monotonic time and boot ID; host uint32 conversion. Independent physical timing remains deferred. |

Inject TX failures, absent FC, wrong sequence, cancel races, BS=0/8, 4095-byte RX, and fragmented USB in software tests. CANable alone suffices for real USB tests; a second CAN device is not needed to specify this protocol.

The [firmware build documentation](BUILD_AND_RECOVERY.md) reports
actual bounds and fixed timeout support. Host IN must be drained concurrently
with command OUT: an unsolicited event can otherwise block the device's output
worker while the host is still transmitting a long command. The dedicated ISO-TP
worker remains independent; USB backpressure does not schedule consecutive CAN
frames. Lease expiry resets old parser ownership before accepting fresh bytes.
Before ownership, reject a non-HELLO header immediately rather than waiting for
its body, and scan past old payload bytes to the next valid header. An old OUT
packet can arrive from the USB queue after its owner's lease has expired.
Likewise, the host drains partial old IN tails and stale responses before the
nonce-matched HELLO; strict stream validation resumes after ownership.
