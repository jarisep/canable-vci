#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
"""Cross-build the Windows userspace package; never installs or accesses USB."""
import argparse
import os
from pathlib import Path
import shutil
import subprocess

ROOT = Path(__file__).resolve().parents[1]

def main():
    parser=argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--zig',required=True)
    parser.add_argument('--output',type=Path,required=True)
    args=parser.parse_args()
    output=args.output.resolve()
    dist=output/'dist';tests=output/'build/stage3'
    for path in [dist/'mediator',tests,output/'logs']:path.mkdir(parents=True,exist_ok=True)
    env=os.environ.copy()
    env.setdefault('ZIG_GLOBAL_CACHE_DIR',str(output/'cache/global'))
    env.setdefault('ZIG_LOCAL_CACHE_DIR',str(output/'cache/local'))
    common=[args.zig,'c++','-std=c++17','-O2','-Wall','-Wextra',
            '-Wno-nullability-completeness','-D_WIN32_WINNT=0x0a00','-DUNICODE',
            '-D_UNICODE','-I','windows/include']
    def build(target,sources,destination,extra=()):
        subprocess.run(common+['-target',target]+list(extra)+sources+['-o',str(destination)],cwd=ROOT,env=env,check=True)
    build('x86-windows-gnu',['windows/src/dll.cpp','windows/src/j2534.def'],dist/'CanableJ2534.dll',['-shared'])
    build('x86_64-windows-gnu',['windows/src/service.cpp','windows/src/engine.cpp'],dist/'CanableVciService.exe',['-lsetupapi','-lwinusb','-lbcrypt','-ladvapi32'])
    build('x86-windows-gnu',['windows/src/pcm_bridge.cpp','windows/src/pcm_bridge.def'],dist/'mediator/CanablePcmBridge.dll',['-shared'])
    for name in ['api_test','mediator_test','mediator_scan_test','pcm_forwarding_test']:
        build('x86-windows-gnu',[f'windows/tests/{name}.cpp'],tests/f'{name}.exe')
    build('x86_64-windows-gnu',['windows/tests/async_log_test.cpp'],tests/'async_log_test.exe')
    shutil.copyfile(dist/'CanableJ2534.dll',tests/'CanableJ2534.dll')
    (tests/'CanableJ2534.ini').write_text('[Service]\nPipeName=\\\\.\\pipe\\CodexCanableVciTest\n')
    print('Windows binaries and simulation tests built; no device or service modified.')
if __name__=='__main__':main()
