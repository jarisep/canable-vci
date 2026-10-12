# SPDX-License-Identifier: Apache-2.0
param([string]$Zig='zig')
$ErrorActionPreference='Stop';$ProgressPreference='SilentlyContinue'
if(Get-Process VCIMediator -ErrorAction SilentlyContinue){throw 'Close Mediator before replacing its CANable bridge.'}
$workspace=Split-Path -Parent $PSScriptRoot
$build=Join-Path $workspace 'build\stage3'
$dist=Join-Path $workspace 'dist\mediator'
New-Item -ItemType Directory -Force $build,$dist|Out-Null
$env:ZIG_GLOBAL_CACHE_DIR=Join-Path $build 'zig-global-cache'
$env:ZIG_LOCAL_CACHE_DIR=Join-Path $build 'zig-local-cache'
$common=@('c++','-std=c++17','-O2','-Wall','-Wextra','-Wno-nullability-completeness','-D_WIN32_WINNT=0x0a00','-DUNICODE','-D_UNICODE','-I',(Join-Path $PSScriptRoot 'include'))
& $Zig @common -target x86-windows-gnu -shared (Join-Path $PSScriptRoot 'src\pcm_bridge.cpp') (Join-Path $PSScriptRoot 'src\pcm_bridge.def') -o (Join-Path $dist 'CanablePcmBridge.dll')
if($LASTEXITCODE -ne 0){throw 'CANable Mediator bridge build failed'}
Write-Output 'Own CANable Mediator bridge built; J2534 DLL and installed service were not replaced.'
