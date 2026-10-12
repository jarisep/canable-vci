# SPDX-License-Identifier: Apache-2.0
$ErrorActionPreference='Stop'
$root=Split-Path -Parent $PSScriptRoot
if(Get-Process VCIMediator -ErrorAction SilentlyContinue){throw 'Close VCI Mediator before the guarded wrapper test.'}
$provider=Get-Item 'HKLM:\SOFTWARE\WOW6432Node\PassThruSupport.04.04\CANable ISO-TP'
if($provider.GetValue('Name') -ne 'CANable ISO-TP' -or $provider.GetValue('Vendor') -ne 'CANable project' -or $provider.GetValue('FunctionLibrary') -ne (Join-Path $root 'dist\CanableJ2534.dll')){throw 'Own CANable profile is not registered correctly.'}
try {
 & (Join-Path $root 'build\stage3\mediator_test.exe') | Tee-Object -FilePath (Join-Path $root 'logs\stage3-mediator-test.log')
 if($LASTEXITCODE -ne 0){throw 'Installed PCMToJ2534 wrapper test failed.'}
 & (Join-Path $root 'dist\CanableVciService.exe') --set-ignition off
 if($LASTEXITCODE -ne 0){throw 'Ignition override failed.'}
 & (Join-Path $root 'build\stage3\mediator_test.exe') --ignition-off | Tee-Object -FilePath (Join-Path $root 'logs\stage3-mediator-off-test.log')
 if($LASTEXITCODE -ne 0){throw 'Installed PCMToJ2534 OFF/battery test failed.'}
} finally {
 & (Join-Path $root 'dist\CanableVciService.exe') --set-ignition on
}
Write-Output 'Own CANable module identity and ignition ON/OFF pass. Registration remains active; Bosch was not modified.'
