# CANable VCI

An experimental CANable ISO-TP adapter for Windows J2534 applications and
VCI Mediator integration. This is a standalone project, maintained alongside
[ET2500](https://github.com/jarisep/et2500).

```text
J2534 application / VCI Mediator
    -> x86 CanableJ2534.dll
    -> local named pipe
    -> x64 CanableVciService.exe
    -> built-in Windows WinUSB
    -> CANable ciso-0.2.1 firmware (Zephyr ISO-TP)
    -> Classical CAN, fixed physical OBD pins 3/8
```

The Windows package provides the project's own J2534 DLL, service and optional
Mediator bridge. It uses Microsoft's built-in WinUSB; there is no separate kernel
driver to install or redistribute. Firmware advertises the interface through
Microsoft OS descriptors.

## Supported development target

- MKS CANable 2.0 Pro, STM32G431C8, verified 64 KiB flash / 32 KiB RAM.
- Classical CAN at 500 kbit/s; payloads up to 4095 bytes.
- One channel, one exact ISO-TP route, one active TX and one RX.
- Standard/29-bit CAN IDs and ISO-TP extended addressing.
- Physical vehicle cable fixed to OBD 3/8. Virtual 6/14 is a compatibility alias;
  selecting it does not move physical CAN wires.
- x86 J2534 04.04 DLL, x64 Windows service, optional x86 Mediator bridge.
- Independent software ignition and battery emulation; no voltage sensor or
  programming-voltage hardware.

## Current status

The ciso-0.2.1 correction shares the persistent receiver's hardware filter with
Flow Control. It fixes a multi-frame TX failure reproduced with M_CAN first-match
filter delivery. Windows logging uses a bounded queue and a dedicated writer.

Recorded project validation includes 33 ISO-TP simulations, three USB simulations,
13 public portable checks (22 in the original private workspace), 482 simulated J2534 assertions and nine
reference cases. The flashed unit passed 12 real USB protocol cases and two
lease-recovery cases. The updated Windows service passed 25 real USB identification
checks. Vehicle ECU discovery and parameter reads have been observed, but physical
post-correction multi-frame interoperability and sub-millisecond timing remain
open. These results do not establish suitability for ECU programming.

Public fixtures preserve addressing, message lengths, flags and event order while
replacing identifying application data. Raw vehicle captures, vendor software,
DiagBox installers, activation/profile experiments, credentials and device backups
are not distributed. Public fixture checks validate the anonymized transport
contract; they do not reproduce the withheld capture's exact data bytes.

## Get started

- [Firmware build and BOOT/DFU recovery](docs/BUILD_AND_RECOVERY.md)
- [Windows package installation and restoration](docs/WINDOWS.md)
- [USB wire protocol](docs/USB_PROTOCOL.md)
- [J2534 implementation contract](docs/J2534_IMPLEMENTATION.md)
- [Release validation and tested scope](docs/VALIDATION.md)
- [License and dependency notices](NOTICE)

Release assets contain the CANable firmware and Windows userspace package.
Build from source when you need changes. Do not replay recorded diagnostic
sessions into a vehicle. Hardware tests require explicit bounded commands;
the portable checks and default simulation tests do not access a device.

Run the portable checks:

```sh
python3 -m unittest discover -s tests -v
python3 tools/make_usb_vectors.py --check
python3 tools/make_j2534_cases.py --check
g++ -std=c++17 -O2 -pthread -I windows/include windows/tests/async_log_test.cpp -o /tmp/canable-log-test
/tmp/canable-log-test
```

An optional [GitHub Actions workflow example](docs/ci/offline-checks.yml) runs
these portable checks and the pinned firmware simulations. It is provided as
a template; this initial publication does not install a repository workflow.

## License

Project-authored source, documentation and modifications are **Apache-2.0**.
The modified Zephyr ISO-TP files retain their original notices and Apache-2.0
license. STM32 HAL, CMSIS and Windows runtime components retain their own licenses;
see [NOTICE](NOTICE), [licenses/](licenses/) and
[firmware provenance](firmware/vendor/PROVENANCE.md). No license is granted for
third-party diagnostic applications, their native DLLs, or vendor trademarks.
