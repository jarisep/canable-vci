# Source provenance

The ISO-TP implementation and context declarations derive from Zephyr commit
`920074c07cf4b503f70670cf424e4add4325d05a`, under Apache-2.0. The pinned upstream files retain their copyright and license notices.
Private audit snapshots are not distributed.

| Local file | Upstream source |
| --- | --- |
| `isotp.c` | `subsys/canbus/isotp/isotp.c` |
| `isotp_internal.h` | `subsys/canbus/isotp/isotp_internal.h` |
| `../include/ciso_isotp.h` | `include/zephyr/canbus/isotp.h` |
| `mcan-stop.patch` | Patch against `drivers/can/can_mcan.c` |

Changes include a dedicated ISO-TP workqueue; complete RX allocation independent
of block size; accepted-FF and failure metadata hooks; per-transfer zero padding;
CAN callback error propagation; pending-frame accounting; cancellation and worker
quiescence; retained RX chain ownership; and malformed FC handling. The ciso-0.2.1 correction shares the persistent
RX hardware filter with Flow Control reception and preserves FC during CLEAR_RX. The M_CAN
patch cancels pending hardware TX requests before entering INIT, since INIT alone
does not remove them. Cancellation timeout is 20 ms and failure is propagated.

USB setup derives from CANnectivity commit
`93eb616f21e270200846ffc20d128c57fdbe5dab`, `app/src/usb.c`, and Zephyr's
`subsys/usb/device_next/class/loopback.c`, under Apache-2.0. Copyright notices
remain in `../src/usb.c`. This is a new dedicated CISO class using the same Zephyr
USB stack and WinUSB descriptor approach; upstream gs_usb does not implement
whole-message ISO-TP and is disabled in this build. The CANnectivity module is
pinned for provenance and later separate gs_usb builds.

The Apache-2.0 license is in `LICENSE`. Zephyr, STM32 HAL, CMSIS, and toolchain
dependencies retain their own notices in their source distributions. Firmware
does not include Elmue's transport or firmware code.
