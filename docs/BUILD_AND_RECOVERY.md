# Firmware build and recovery

Target `mks_canable_v20/stm32g431xx`, with 64 KiB flash and 32 KiB RAM. Do not
assume the ROM DFU descriptor's advertised 128 KiB is the actual device capacity.
Identify the connected board and verify its MCU/capacity before writing.
The tested ciso-0.2.1 image uses 52,296 flash bytes and 24,440 RAM bytes.

## Build pinned sources

Dependencies are pinned in `west.yml`. The helper verifies exact Git revisions
and the M_CAN driver hash before applying its bounded hardware TX cancellation
patch in an isolated checkout. Python 3.12, west 1.5.0, CMake 4.4.4, Ninja and
Arm GCC 14.2.1 were used in the recorded build. Install an Arm toolchain with
newlib support separately.

Place this repository at `<workspace>/canable-vci`, then:

```sh
cd <workspace>
python3 -m venv .venv
. .venv/bin/activate
python -m pip install west==1.5.0 cmake==4.4.4 ninja
west init -l canable-vci
west update
python -m pip install -r zephyr/scripts/requirements-base.txt
export ZEPHYR_TOOLCHAIN_VARIANT=gnuarmemb
export GNUARMEMB_TOOLCHAIN_PATH=<arm-toolchain-root>
python canable-vci/tools/build_firmware.py --workspace . --build-dir build/ciso
```

ISO-TP and USB tests run with `--target isotp-tests` and `--target usb-tests`
in separate build directories. Native tests use the host C compiler and
`native_sim/native/64`. They do not measure physical CAN timing.

## Preserve and flash the actual unit

Disconnect vehicle CAN before maintenance. Fit only the jumper marked BOOT
while USB is disconnected, then reconnect. Identify the ROM DFU serial with
`dfu-util -d 0483:df11 -l`. Substitute that exact serial below; verify the full
backup length and hash before continuing.

```sh
dfu-util -d 0483:df11 -S <DFU_SERIAL> -a 0 -s 0x08000000:65536 -U pre-flash-64k.bin
sha256sum pre-flash-64k.bin
dfu-util -d 0483:df11 -S <DFU_SERIAL> -a 0 -s 0x08000000 -D canable-ciso-0.2.1.bin
```

Do not request `:leave`. While still in DFU, upload exactly the image length
(`wc -c canable-ciso-0.2.1.bin`) and compare it byte for byte:

```sh
dfu-util -d 0483:df11 -S <DFU_SERIAL> -a 0 -s 0x08000000:<IMAGE_BYTES> -U new-image-readback.bin
cmp canable-ciso-0.2.1.bin new-image-readback.bin
```

Use fresh upload filenames; dfu-util refuses to overwrite them. After successful
comparison, disconnect USB, remove BOOT, wait at least three seconds and reconnect.
The application should enumerate as `1209:0001` and report `ciso-0.2.1`:

```sh
python3 tools/canable_vci.py probe
python3 tools/canable_vci.py usb-test
python3 tools/canable_vci.py usb-recovery
```

These commands transmit zero CAN frames. On Linux, grant access to the exact
USB unit using a serial-specific udev rule; on Windows use the advertised WinUSB
binding. VID/PID `1209:0001` is a private development identity, not a product
allocation. Do not change drivers or permissions for unrelated USB devices.

## Restore

With BOOT fitted, restore the verified actual `pre-flash-64k.bin` using the same
DFU serial and internal-flash address, upload all 65,536 bytes to a fresh filename,
and compare. Then disconnect, remove BOOT, wait at least three seconds and reconnect.
Never substitute a different unit's unverified backup. Option bytes, protection,
OTP and boot-selection settings are not changed by the documented commands.

## Short power-cycle behavior

The Pro board has a documented shared PB8 CAN_RX/BOOT0 hardware issue. A short
power interruption can select ROM DFU before the application runs. The known
option-byte workaround ignores BOOT0 and therefore also disables jumper recovery.
This project retains the jumper and does not apply that workaround. Use a cold
power cycle of at least three seconds. See the
[board investigation](https://github.com/makerbase-mks/CANable-MKS/issues/6).
