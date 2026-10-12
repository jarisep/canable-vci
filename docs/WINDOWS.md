# Windows userspace package

The Windows release supplies an x86 `CanableJ2534.dll`, x64
`CanableVciService.exe`, and optional x86 `CanablePcmBridge.dll` for an existing
VCI Mediator installation. Microsoft WinUSB is supplied by Windows; no vendor
kernel-driver binary is bundled. The package does not include diagnostic vendor
applications, native Mediator DLLs or authorization/profile modifications.

## Build or extract

Extract the release ZIP into `C:\codex-canable-vci` so that `windows/`, `dist/`,
`build/stage3/`, `tools/` and `logs/` are direct children. The scripts also support
another chosen workspace directory. Python 3 is only needed for the USB CLI.

For a source build, install the official portable Zig 0.16.0 compiler and run:

```powershell
.\windows\build.ps1 -Zig 'C:\tools\zig\zig.exe'
```

This builds the J2534 DLL, service, optional bridge and test programs. Run the
hardware-free simulation before installation:

```powershell
.\windows\Run-Tests.ps1
```

The simulation uses `FakeClient` and a separate pipe. Public test data is
anonymized. Do not select `-RealUsb` while connected to a vehicle; that mode is a
bench test suite, not a vehicle diagnostic session replay.

## Select your device and install

Connect CANable with BOOT removed and a cold power cycle. Use Device Manager or:

```powershell
python .\tools\canable_vci.py devices
```

Choose the application's 24-hex-digit USB serial (not its differently formatted
ROM DFU serial). Configure that serial explicitly. From elevated PowerShell:

```powershell
.\windows\Install-Service.ps1 -Serial '<YOUR_APPLICATION_USB_SERIAL>'
.\windows\Manage-Provider.ps1 -Mode Activate
.\dist\CanableVciService.exe --status
```

The service runs as LocalSystem with Manual startup. Its named pipe is limited
to LocalSystem and the installing user's SID. The x86 J2534 provider appears as
**CANable ISO-TP**, Vendor **CANable project**. Provider activation snapshots its
own registry state and refuses to overwrite an unrelated existing key.

The application must reopen its J2534 connection after a service restart.
The development cable used OBD 3/8. Pin-selection requests are accepted virtually
to accommodate applications that expect a VCI pin multiplexer; CANable has no
physical mux. The cable determines the connected bus. See the
[wiring and alias explanation](../README.md#obd-wiring-and-pin-selection-compatibility)
for the reason and how to use another pinout. Software ignition does not switch
vehicle ignition. USB loss still returns a device error independently of voltage
emulation.

For identification without CAN traffic, close diagnostic applications, then:

```powershell
.\build\stage3\api_test.exe .\dist\CanableJ2534.dll --identify
```

Do not run direct USB tools concurrently with a J2534 owner. The identification
harness opens no diagnostic channel. It verifies expected rejection of unsupported
programming-voltage operations.

## Optional Mediator bridge

The [dedicated bridge guide](MEDIATOR_BRIDGE.md) explains the PCM/J2534 call path,
CANable-specific adaptations, supported native binary, settings and restoration.

Only use the bridge with a compatible, separately installed VCI Mediator. It
adapts CANable naming and independent ignition and forwards other providers to
the original native DLL. That proprietary DLL is not redistributed. The bridge
pins its supported ABI/binary; an arbitrary different vendor version is not
implicitly supported.

Close Mediator and inspect state before activation:

```powershell
.\windows\Manage-MediatorBridge.ps1 -Mode Status
.\windows\Manage-MediatorBridge.ps1 -Mode Activate
```

This guarded integration takes a restoration snapshot and changes only its
recorded component selectors. Registration changes alone do not establish that
every diagnostic application's adapter-selection screen is compatible.

## Update and restore

Close diagnostic applications and verify `usb_owned=0`, `channel=0` before
replacing a service executable. Copy the current EXE, INI files and logs into a
new backup directory and record their hashes. Stop only `CodexCanableVci`, replace
its EXE, start it and check `--status`. Restore the previous backed-up EXE if
verification fails. Do not overwrite a previous backup.

To undo optional integration, use the existing snapshots:

```powershell
.\windows\Manage-MediatorBridge.ps1 -Mode Restore
.\windows\Manage-Provider.ps1 -Mode Restore
.\windows\Uninstall-Service.ps1
```

DLL/provider and tester settings must not be changed merely to update service
logging. The async logger retains one 1 MiB `.old` file, reports queue drops and
drains on normal shutdown; forced process termination can lose queued records.
