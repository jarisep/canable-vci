# Initial public release validation

Prepared 2026-10-12. The repository is a curated, standalone publication of the
working CANable project. It contains no raw diagnostic captures, original vendor
binaries, private profiles, credentials, host-specific serials or flash backups.

## Public source checks

- 13 portable stream/ABI/USB-contract checks pass.
- USB vectors and anonymized J2534 fixture header regenerate exactly.
- Five logger regression groups pass on Linux and Windows.
- 33 ISO-TP tests pass with first-match hardware-filter simulation.
- Three virtual USB tests pass.
- The public firmware build uses 52,296 flash bytes and 24,440 RAM bytes. It is
  byte-identical to the tested ciso-0.2.1 image, SHA-256
  `0266533715739dd824dc29747eb28afb1eb6c5f7d916627d0f45aedb598e07ea`.
- The final public Windows build passes 482 simulated J2534 assertions and all
  nine transport/lifecycle reference cases using anonymized fixture payloads.
- Source content, local documentation links, license attribution and release
  package hashes are checked before publication.

Public portability changes replace the development unit's embedded serial with
installation configuration, use a synthetic simulation UID, and make native
identification tests accept a configured unit and the supported firmware family.
The test harness honors fake-transport backpressure and uses a simulation-only
processing barrier before asserting the independent application RX queue's bound.
This avoids a scheduling-dependent overflow test. The barrier is rejected by the
production backend along with other simulation operations.

## Recorded hardware scope

The ciso-0.2.1 image was flashed to a serial-verified unit after a full backup and
read back byte for byte. Cold startup, 12 real USB protocol cases and two lease
recovery cases passed without CAN transmissions. The updated installed Windows
service passed 25 real USB identification assertions through the J2534 DLL.
These hardware checks used the private deployment's configured serial; the public
Windows build's generic configuration/synthetic test changes were validated in
simulation, without replacing that deployed service.

Earlier vehicle work achieved ECU discovery and parameter reads and exposed
multi-frame TX failures. The RX/FC filter correction reproduces and fixes the
corresponding software-simulation defect. Post-correction vehicle interoperability,
remaining delayed-response errors and physical sub-millisecond STmin accuracy
remain unverified. No vehicle diagnostic capture was replayed by publication tests.

Release packages contain unsigned development userspace binaries, matching public
sources, and license/attribution material. Microsoft WinUSB is obtained from the
operating system, not this package.
