# Optional Stellantis VCI Mediator bridge

`CanablePcmBridge.dll` is an optional x86 compatibility layer for the
Stellantis/DiagBox setup used during this project's testing. The primary product
is the CANable J2534 adapter. Applications that load `CanableJ2534.dll` directly
do not need this bridge.

## Where it fits

```text
Stellantis / DiagBox application
    -> VCI Mediator
    -> CanablePcmBridge.dll
    -> original PCMToJ2534.dll
    -> CanableJ2534.dll
    -> CANable Windows service
    -> WinUSB -> CANable CAN / ISO-TP firmware
```

The original `PCMToJ2534.dll` remains installed and performs the native
PCM-to-J2534 translation. The bridge exports the same 33 PCM entry points,
forwards the native communication operations, and adapts a small set of
CANable-specific identification and ignition calls. It does not replace the
ISO-TP implementation or schedule CAN frames.

## Why it was added

The Stellantis/DiagBox test setup uses Mediator's PCM component for access to
J2534 interfaces. That path needed CANable-specific module naming and ignition
reporting. A J2534 provider registration alone did not provide all of that
application-facing behavior. The bridge supplies the missing adaptation while
continuing to use the original native component.

The virtual OBD pin-selection aliases belong to the J2534 service, not this
bridge. See the [wiring and pin-selection explanation](../README.md#obd-wiring-and-pin-selection-compatibility).

## Adapted behavior

| Area | Bridge behavior |
| --- | --- |
| Adapter selection | Calls the native `PCM_SetMVCI` / `PCM_SetMVCIExp`, then enables CANable handling only after successful selection of `CANable ISO-TP` with the applicable `PASSTHRU0404` type. |
| Module naming | Gives the selected CANable module the name `CANable ISO-TP`. Provider registration retains Vendor `CANable project`. |
| Ignition, `TypeVoltage=3` | Validates the native module/USB connection through the native voltage call, then reads the independent ignition state from the CANable service. Returns 13500 mV for software ON and 0 for software OFF. |
| Battery, `TypeVoltage=4` | Continues through the native component and J2534 provider. The current adapter's battery value is emulated separately from ignition. |
| Other adapters and voltage types | Continue through the native component. |
| Communication operations | Native forwarding covers logical links, communication parameters, primitives, callbacks, events and other unmodified PCM operations. |
| Cleanup | Successful `PCM_Unprepare` clears CANable-specific selection state. |

Ignition reporting is software emulation. It does not measure or switch vehicle
ignition, and an ON value does not establish CAN communication with an ECU.
Disconnected USB, an invalid module or a failed service query returns an error
instead of reporting a successful ignition state.

## Supported native component

The bridge was developed against the x86 PCM API in `PCMToJ2534` **13.2.4.0**,
with Stellantis VCI Mediator **3.5.2** in the test environment. Activation requires
this exact native DLL SHA-256:

```text
13f5964cd117a82bd604a64c2ce042346f2001673d32b8b38cdb810d8ad11f51
```

The activation script rejects a different binary for ABI review. The loader also
requires all 33 expected exports. Support for another vendor version is not
established by matching a filename or copying this DLL into its installation.

The native location is currently:

```text
C:\Program Files (x86)\Stellantis_VCIMediator\drv\PCMToJ2534.dll
```

Both the bridge loader and management script refer to that installation location.
A different location requires reviewing both paths and rebuilding the bridge.
The proprietary native component is not included in this repository or release.

## Build and activate

The [Windows release package](https://github.com/jarisep/canable-vci/releases/latest)
includes `dist/mediator/CanablePcmBridge.dll`. To build it separately:

```powershell
.\windows\Build-MediatorBridge.ps1 -Zig 'C:\tools\zig\zig.exe'
```

First install the CANable service and register its J2534 provider as described
in [WINDOWS.md](WINDOWS.md). Close DiagBox and Mediator before changing their
component selectors. From elevated PowerShell, inspect and activate:

```powershell
.\windows\Manage-MediatorBridge.ps1 -Mode Status
.\windows\Manage-MediatorBridge.ps1 -Mode Activate
```

Activation backs up the affected INI files, records their hashes and changes
these selectors to the bridge:

| File | Section / key | Selected value |
| --- | --- | --- |
| `drv/VCIManager.ini` | `COMMON / PCMToJ2534LibPath` | Full path to the bridge DLL |
| `WSCore.ini` | `PCMToJ2534 / Path` | Bridge directory |
| `WSCore.ini` | `PCMToJ2534 / Composant` | `CanablePcmBridge` |

The original native DLL is not replaced. The management script verifies the
expected starting selectors and refuses unrelated configuration changes.
The restoration record is stored at
`backups/canable-mediator-bridge-state.json`, with timestamped file backups.

## Check operation and restore

With diagnostic applications closed, use the identification/ignition harnesses:

```powershell
.\windows\Test-MediatorScan.ps1
.\windows\Test-Mediator.ps1
```

The scan checks native discovery/description. The module harness checks identity,
connection and ignition ON/OFF independently of battery emulation; its `finally`
block restores software ignition ON. These checks do not send diagnostic CAN
frames. They do not establish compatibility with every application's adapter UI
or every vehicle session. See [validation scope](VALIDATION.md).

Bridge messages are written to `logs/mediator-bridge.log`. A failed native load,
missing export or failed ignition query is recorded there.

To undo the bridge selection, close diagnostic applications and run:

```powershell
.\windows\Manage-MediatorBridge.ps1 -Mode Restore
```

Restoration verifies that the active INI files still match the recorded hashes,
then restores their original contents byte for byte. It refuses to overwrite
files changed outside this integration. Restoring the bridge selectors does not
remove the CANable J2534 provider or uninstall the service; those are separate
operations.
