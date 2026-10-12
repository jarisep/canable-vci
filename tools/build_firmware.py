#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
"""Build pinned firmware or native tests in an existing isolated west workspace."""
import argparse
import hashlib
import os
from pathlib import Path
import subprocess

ROOT=Path(__file__).resolve().parents[1]
REVISIONS={
    'zephyr':'920074c07cf4b503f70670cf424e4add4325d05a',
    'modules/hal/cmsis_6':'1c1840af7a7e757d6e2fec3ddb0e5ce0dfcc93c8',
    'modules/hal/stm32':'c9550199186236d610092f4005f8c9bae2a6bfcf',
}
MCAN_ORIGINAL='fb1cb92cf9df28eecd1c4ba661758cb89c09433f24c9a0bccea4de8ffc6e1111'
MCAN_PATCHED='84f6bc0b1cf7f9f97ed0241f67137c6f86d84d2b93edfbe54b845d3e1db608f3'
def main():
    p=argparse.ArgumentParser(description=__doc__)
    p.add_argument('--workspace',type=Path,required=True)
    p.add_argument('--target',choices=['firmware','isotp-tests','usb-tests'],default='firmware')
    p.add_argument('--build-dir',type=Path,required=True)
    p.add_argument('--pristine',action='store_true')
    args=p.parse_args()
    workspace=args.workspace.resolve();build=args.build_dir.resolve()
    for path,expected in REVISIONS.items():
        actual=subprocess.check_output(['git','-C',str(workspace/path),'rev-parse','HEAD'],text=True).strip()
        if actual!=expected:raise SystemExit(f'Wrong source revision for {path}: {actual}')
    cannectivity=workspace/'modules/lib/cannectivity'
    if cannectivity.exists():
        actual=subprocess.check_output(['git','-C',str(cannectivity),'rev-parse','HEAD'],text=True).strip()
        if actual!='93eb616f21e270200846ffc20d128c57fdbe5dab':raise SystemExit('Wrong CANnectivity revision')
    zephyr=workspace/'zephyr';driver=zephyr/'drivers/can/can_mcan.c'
    digest=hashlib.sha256(driver.read_bytes()).hexdigest()
    if digest==MCAN_ORIGINAL:
        # This is intentionally an isolated checkout, not an unrelated Zephyr tree.
        subprocess.run(['git','apply',str(ROOT/'firmware/vendor/mcan-stop.patch')],cwd=zephyr,check=True)
    elif digest!=MCAN_PATCHED:raise SystemExit('M_CAN source has unexpected changes; refusing to patch')
    if hashlib.sha256(driver.read_bytes()).hexdigest()!=MCAN_PATCHED:raise SystemExit('M_CAN patch verification failed')
    env=os.environ.copy();env['ZEPHYR_BASE']=str(zephyr)
    native=args.target!='firmware'
    if native:env['ZEPHYR_TOOLCHAIN_VARIANT']='host'
    source=ROOT/({'firmware':'firmware','isotp-tests':'tests/firmware','usb-tests':'tests/usb'}[args.target])
    board='native_sim/native/64' if native else 'mks_canable_v20/stm32g431xx'
    command=['west','build','-b',board,'-d',str(build),str(source)]
    if args.pristine:command.extend(['--pristine','always'])
    command.append('--')
    # Explicit module list makes resource measurements independent of extra projects.
    modules=[workspace/'modules/hal/cmsis_6',workspace/'modules/hal/stm32']
    command.append('-DZEPHYR_MODULES='+';'.join(map(str,modules)))
    if args.target=='usb-tests':command.append('-DDTC_OVERLAY_FILE='+str(source/'boards/native_sim.overlay'))
    subprocess.run(command,cwd=workspace,env=env,check=True)
    if native:
        subprocess.run([str(build/'zephyr/zephyr.exe')],check=True)
    else:
        image=build/'zephyr/zephyr.bin'
        if image.stat().st_size>65536:raise SystemExit('Image exceeds verified 64 KiB flash capacity')
        print(f'Firmware bytes: {image.stat().st_size}; SHA256: {hashlib.sha256(image.read_bytes()).hexdigest()}')
if __name__=='__main__':main()
