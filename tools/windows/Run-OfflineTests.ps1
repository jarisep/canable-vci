# SPDX-License-Identifier: Apache-2.0
# Host protocol checks only; no CANable or vehicle is required.
$ErrorActionPreference='Stop'
$root=(Resolve-Path (Join-Path $PSScriptRoot '..\..')).Path
$python=Join-Path $env:LOCALAPPDATA 'Programs\Python\Python312\python.exe'
if (-not (Test-Path $python)) { throw 'Python 3.12 was not found at the verified tester path.' }
& $python (Join-Path $root 'tools\run_offline_tests.py') |
    Tee-Object -FilePath (Join-Path $root 'logs\windows-offline-tests.log')
if ($LASTEXITCODE -ne 0) { throw 'Offline protocol tests failed.' }
& $python (Join-Path $root 'tools\canable_vci.py') devices --output (Join-Path $root 'logs\windows-device-paths.json')
if ($LASTEXITCODE -ne 0) { throw 'SetupAPI device enumeration failed.' }
