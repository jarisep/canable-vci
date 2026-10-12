# SPDX-License-Identifier: Apache-2.0
$ErrorActionPreference='Stop';$ProgressPreference='SilentlyContinue'
$root=Split-Path -Parent $PSScriptRoot
if((Get-Service CodexCanableVci).Status -ne 'Running'){throw 'Start CodexCanableVci first.'}
Remove-Item (Join-Path $root 'logs\usb-loss-ready.txt') -Force -ErrorAction SilentlyContinue
# This waits for human unplug/replug. It never transmits a CAN message.
& (Join-Path $root 'build\stage3\api_test.exe') (Join-Path $root 'dist\CanableJ2534.dll') --usb-loss | Tee-Object -FilePath (Join-Path $root 'logs\stage3-usb-loss.log')
if($LASTEXITCODE -ne 0){throw 'Physical USB loss/recovery test failed.'}
