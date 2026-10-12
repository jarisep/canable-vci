# SPDX-License-Identifier: Apache-2.0
param([string]$Zig='zig')
$ErrorActionPreference='Stop'
$root=$PSScriptRoot
$workspace=Split-Path -Parent $root
$dist=Join-Path $workspace 'dist'
$build=Join-Path $workspace 'build\stage3'
New-Item -ItemType Directory -Force $dist,$build,(Join-Path $workspace 'logs') | Out-Null
$env:ZIG_GLOBAL_CACHE_DIR=Join-Path $build 'zig-global-cache'
$env:ZIG_LOCAL_CACHE_DIR=Join-Path $build 'zig-local-cache'
$common=@('c++','-std=c++17','-O2','-Wall','-Wextra','-Wno-nullability-completeness','-D_WIN32_WINNT=0x0a00','-DUNICODE','-D_UNICODE','-I',(Join-Path $root 'include'))
& $Zig @common -target x86-windows-gnu -shared (Join-Path $root 'src\dll.cpp') (Join-Path $root 'src\j2534.def') -o (Join-Path $dist 'CanableJ2534.dll')
if($LASTEXITCODE -ne 0){throw 'x86 J2534 DLL build failed'}
& $Zig @common -target x86_64-windows-gnu (Join-Path $root 'src\service.cpp') (Join-Path $root 'src\engine.cpp') -lsetupapi -lwinusb -lbcrypt -ladvapi32 -o (Join-Path $dist 'CanableVciService.exe')
if($LASTEXITCODE -ne 0){throw 'x64 service build failed'}
New-Item -ItemType Directory -Force (Join-Path $dist 'mediator')|Out-Null
& $Zig @common -target x86-windows-gnu -shared (Join-Path $root 'src\pcm_bridge.cpp') (Join-Path $root 'src\pcm_bridge.def') -o (Join-Path $dist 'mediator\CanablePcmBridge.dll')
if($LASTEXITCODE -ne 0){throw 'x86 CANable Mediator bridge build failed'}
& $Zig @common -target x86-windows-gnu (Join-Path $root 'tests\api_test.cpp') -o (Join-Path $build 'api_test.exe')
if($LASTEXITCODE -ne 0){throw 'x86 API test build failed'}
& $Zig @common -target x86-windows-gnu (Join-Path $root 'tests\mediator_test.cpp') -o (Join-Path $build 'mediator_test.exe')
if($LASTEXITCODE -ne 0){throw 'x86 Mediator test build failed'}
& $Zig @common -target x86-windows-gnu (Join-Path $root 'tests\mediator_scan_test.cpp') -o (Join-Path $build 'mediator_scan_test.exe')
if($LASTEXITCODE -ne 0){throw 'x86 Mediator discovery test build failed'}
& $Zig @common -target x86-windows-gnu (Join-Path $root 'tests\pcm_forwarding_test.cpp') -o (Join-Path $build 'pcm_forwarding_test.exe')
if($LASTEXITCODE -ne 0){throw 'x86 native PCM forwarding test build failed'}
Copy-Item (Join-Path $dist 'CanableJ2534.dll') (Join-Path $build 'CanableJ2534.dll') -Force
"[Service]`r`nPipeName=\\.\pipe\CodexCanableVciTest`r`n" | Set-Content -Encoding ASCII (Join-Path $build 'CanableJ2534.ini')
& $Zig @common -target x86_64-windows-gnu (Join-Path $root 'tests\async_log_test.cpp') -o (Join-Path $build 'async_log_test.exe')
if($LASTEXITCODE -ne 0){throw 'Asynchronous logger test build failed'}
Write-Output 'Windows service and x86 DLL built successfully.'
