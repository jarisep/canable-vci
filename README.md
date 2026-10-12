# CANable J2534

Use a CANable adapter as a **J2534 PassThru interface on Windows**.
The project provides an x86 J2534 DLL, an x64 Windows service and CANable
firmware that handles ISO-TP directly on the device.

```text
J2534 application
    -> CanableJ2534.dll (x86)
    -> local named pipe
    -> CanableVciService.exe (x64)
    -> Windows WinUSB
    -> CANable firmware: CAN + ISO-TP
```

Windows uses its built-in WinUSB driver. The project's Windows package supplies
the userspace J2534 adapter and service.

## Features

- J2534 04.04 interface, including ISO15765 and ISO15765_PS.
- ISO-TP segmentation, reassembly, Flow Control and transfer timing in firmware.
- Standard and 29-bit CAN identifiers, with extended addressing support.
- Payloads up to 4095 bytes, message filters and bounded periodic jobs.
- USB ownership, transfer cancellation and disconnect recovery.
- Virtual pin selection and independently emulated ignition/battery values.
- Bounded asynchronous logging in the Windows service.

## Get started

Download the [latest release](https://github.com/jarisep/canable-vci/releases/latest).
It includes a firmware package, a Windows package and SHA-256 checksums.

1. Identify your CANable and preserve its current firmware before flashing.
2. Follow the [firmware build and BOOT/DFU recovery guide](docs/BUILD_AND_RECOVERY.md).
3. Extract the Windows package and follow the [Windows installation guide](docs/WINDOWS.md).
4. Register the **CANable ISO-TP** J2534 provider and select it in your application.

The source includes build scripts, installation/restoration tools and tests.

## Current hardware and limits

The tested target is **MKS CANable 2.0 Pro, STM32G431C8**, with 64 KiB flash and
32 KiB RAM. The current firmware uses Classical CAN at 500 kbit/s, one channel,
one exact ISO-TP route, one active TX and one RX.

Ignition and battery values are software emulation. There is no voltage sensor
or programming voltage hardware.

The project has been tested with **Stellantis diagnostic software and DiagBox**,
including USB operation, ECU discovery and parameter reads. This is an
experimental adapter; broader vehicle interoperability and physical
sub-millisecond timing still need verification. ECU programming is not a verified
use case. See [validation and tested scope](docs/VALIDATION.md).

## OBD wiring and pin-selection compatibility

The development setup used OBD pins **3/8** for the vehicle CAN bus accessed
during Stellantis/DiagBox testing. This test setup is the reason for that cable
pinout. CANable is not tied to the pair: its CAN_H and CAN_L terminals connect
to whichever vehicle bus the cable is wired to.

Some J2534 applications expect a VCI with a pin multiplexer and request pin
switching through `SET_CONFIG(J1962_PINS)`. CANable has no pin multiplexer.
To avoid application errors for these requests, the driver accepts **3/8**
(`0x0308`) and **6/14** (`0x060E`) as virtual selections and returns the last
selection through `GET_CONFIG`. Both refer to the same CANable CAN interface;
there is no physical switching. The 6/14 compatibility alias was added for this
software expectation.

### Using a different cable pinout

1. Disconnect USB and the vehicle connection before changing the cable.
2. Wire CAN_H and CAN_L to the target vehicle's documented CAN pair. A conventional
   OBD CAN cable uses CAN_H on pin **6** and CAN_L on pin **14**; see the
   [OBD CAN pinout example](https://www.peak-system.com/products/accessories/cables/pcan-cable-obd-2/).
3. Reconnect and use the appropriate virtual selection in the application.
   Moving the cable from 3/8 to 6/14 does not require a firmware change.

The current virtual default is `0x060E`. To change that default, update `pins` in
[engine.hpp](windows/include/engine.hpp) and channel initialization in
[engine.cpp](windows/src/engine.cpp). That file's `J1962_PINS` validation controls
which virtual pairs are accepted. Other pairs currently return an error.
The service's `physical_pins=3/8` status/log text describes the development setup;
update those labels when changing its cable. They do not detect or control wiring.

## Tests and protocol

The current public build passes 33 ISO-TP simulations, three USB simulations,
13 portable checks and 482 simulated J2534 assertions. Fixtures use anonymized
application data. Do not replay recorded sessions into a vehicle.

```sh
python3 -m unittest discover -s tests -v
python3 tools/make_usb_vectors.py --check
python3 tools/make_j2534_cases.py --check
```

- [J2534 implementation contract](docs/J2534_IMPLEMENTATION.md)
- [CANable USB protocol](docs/USB_PROTOCOL.md)
- [Optional CI workflow template](docs/ci/offline-checks.yml)

## Bonus: VCI Mediator support

The optional bridge was added for the Stellantis/DiagBox test setup, which uses
VCI Mediator. It adapts CANable identification and ignition reporting for a
compatible, separately installed Mediator. The main interface remains J2534. See the
[Mediator integration instructions](docs/WINDOWS.md#optional-mediator-bridge).

## License

Project-authored code and modifications use **Apache-2.0**, matching the Zephyr
ISO-TP base. Original notices are retained; dependencies keep their own licenses.
See [LICENSE](LICENSE), [NOTICE](NOTICE) and [firmware provenance](firmware/vendor/PROVENANCE.md).
