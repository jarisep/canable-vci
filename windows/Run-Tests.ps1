# SPDX-License-Identifier: Apache-2.0
param([switch]$RealUsb)
$ErrorActionPreference='Stop';$ProgressPreference='SilentlyContinue'
$workspace=Split-Path -Parent $PSScriptRoot
$service=Join-Path $workspace 'dist\CanableVciService.exe'
$test=Join-Path $workspace 'build\stage3\api_test.exe'
$mode=if($RealUsb){'--real'}else{'--fake'}
$dll=if($RealUsb){Join-Path $workspace 'dist\CanableJ2534.dll'}else{Join-Path $workspace 'build\stage3\CanableJ2534.dll'}
$args=if($RealUsb){'--console'}else{'--simulate'}
if($RealUsb -and (Get-Service CodexCanableVci -ErrorAction SilentlyContinue | Where-Object Status -eq Running)){throw 'Stop the installed service before running the console USB test.'}
& (Join-Path $workspace 'build\stage3\async_log_test.exe')
if($LASTEXITCODE -ne 0){throw 'Asynchronous logger regression failed.'}
$process=Start-Process -FilePath $service -ArgumentList $args -PassThru -WindowStyle Hidden
try {
 Start-Sleep -Milliseconds 500
 if($process.HasExited){throw 'Test service exited before the client started.'}
 & $test $dll $mode | Tee-Object -FilePath (Join-Path $workspace "logs\stage3-api-$($mode.TrimStart('-')).log")
 if($LASTEXITCODE -ne 0){throw 'J2534 ABI/runtime tests failed.'}
} finally {
 # Terminate only the console process created by this test. This is also a
 # process-loss exercise; firmware must release any residual lease.
 if(-not $process.HasExited){Stop-Process -Id $process.Id -Force}
}
