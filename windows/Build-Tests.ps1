# SPDX-License-Identifier: Apache-2.0
param([string]$Zig='zig')
$ErrorActionPreference='Stop';$ProgressPreference='SilentlyContinue'
$workspace=Split-Path -Parent $PSScriptRoot
$build=Join-Path $workspace 'build\stage3'
New-Item -ItemType Directory -Force $build|Out-Null
$env:ZIG_GLOBAL_CACHE_DIR=Join-Path $build 'zig-global-cache'
$env:ZIG_LOCAL_CACHE_DIR=Join-Path $build 'zig-local-cache'
$common=@('c++','-std=c++17','-O2','-Wall','-Wextra','-Wno-nullability-completeness','-D_WIN32_WINNT=0x0a00','-DUNICODE','-D_UNICODE','-I',(Join-Path $PSScriptRoot 'include'))
foreach($name in @('api_test','mediator_test','mediator_scan_test','pcm_forwarding_test')){
 & $Zig @common -target x86-windows-gnu (Join-Path $PSScriptRoot "tests\$name.cpp") -o (Join-Path $build "$name.exe")
 if($LASTEXITCODE -ne 0){throw "$name build failed"}
}
& $Zig @common -target x86_64-windows-gnu (Join-Path $PSScriptRoot 'tests\async_log_test.cpp') -o (Join-Path $build 'async_log_test.exe')
if($LASTEXITCODE -ne 0){throw 'Asynchronous logger test build failed'}
Write-Output 'Windows x86 test executables rebuilt; installed service and DLL were not replaced.'
