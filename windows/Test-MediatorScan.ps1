# SPDX-License-Identifier: Apache-2.0
$ErrorActionPreference='Stop';$ProgressPreference='SilentlyContinue'
$workspace=Split-Path -Parent $PSScriptRoot
$manager='C:\Program Files (x86)\Stellantis_VCIMediator\drv\VCIManager.dll'
$expected='8df7f8e29e9f2a2e26ba50eb42d9014a601377df2dd6d20556f8b28464438c73'
if((Get-FileHash $manager -Algorithm SHA256).Hash.ToLowerInvariant() -ne $expected){
 throw 'VCIManager ABI has changed. Review its private discovery exports before running this harness.'
}
$provider=Get-Item 'HKLM:\SOFTWARE\WOW6432Node\PassThruSupport.04.04\CANable ISO-TP'
if($provider.GetValue('FunctionLibrary') -ne (Join-Path $workspace 'dist\CanableJ2534.dll')){
 throw 'Own CANable provider is not registered. This read-only test does not switch providers.'
}
if($provider.GetValue('Name') -ne 'CANable ISO-TP' -or $provider.GetValue('Vendor') -ne 'CANable project'){throw 'CANable identity differs from its own profile.'}
if((Get-Service CodexCanableVci).Status -ne 'Running'){throw 'Start the installed CANable service first.'}
$harness=Join-Path $workspace 'build\stage3\mediator_scan_test.exe'
if(-not(Test-Path $harness)){throw 'Build the x86 discovery harness with Build-Tests.ps1 first.'}
$log=Join-Path $workspace 'logs\mediator-scan.log'
& $harness 2>&1 | Tee-Object -FilePath $log
if($LASTEXITCODE -ne 0){throw 'VCIManager discovery/description failed; see logs\mediator-scan.log.'}
Write-Output 'Discovery test finished. Provider, service and installed Mediator configuration were not changed.'
