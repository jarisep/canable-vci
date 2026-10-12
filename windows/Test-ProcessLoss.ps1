# SPDX-License-Identifier: Apache-2.0
$ErrorActionPreference='Stop';$ProgressPreference='SilentlyContinue'
$root=Split-Path -Parent $PSScriptRoot;$service="$root\dist\CanableVciService.exe";$test="$root\build\stage3\api_test.exe";$dll="$root\dist\CanableJ2534.dll"
if(Get-Service CodexCanableVci -ErrorAction SilentlyContinue | Where-Object Status -eq Running){throw 'Stop the installed service before this console-only test.'}
$server=$null;$client=$null
try {
 $server=Start-Process $service -ArgumentList '--console' -WindowStyle Hidden -PassThru
 Start-Sleep -Milliseconds 500
 & $test $dll --owner-crash
 if($LASTEXITCODE -ne 0){throw 'Crash owner failed'}
 Start-Sleep -Milliseconds 500
 & $test $dll --identify
 if($LASTEXITCODE -ne 0){throw 'Fresh owner after crash failed'}
 $ready="$root\logs\service-loss-ready.txt"
 Remove-Item $ready -Force -ErrorAction SilentlyContinue
 $client=Start-Process $test -ArgumentList @($dll,'--service-loss') -WindowStyle Hidden -PassThru -RedirectStandardOutput "$root\logs\service-loss-test.log" -RedirectStandardError "$root\logs\service-loss-test-error.log"
 # Capture the process handle before exit so PowerShell can retrieve its code.
 $null=$client.Handle
 $deadline=(Get-Date).AddSeconds(10)
 while(-not (Test-Path $ready) -and (Get-Date) -lt $deadline){if($client.HasExited){throw 'Loss client exited before readiness'};Start-Sleep -Milliseconds 100}
 if(-not (Test-Path $ready)){throw 'Loss client readiness timed out'}
 Stop-Process -Id $server.Id -Force
 # This intentionally kills only the console process created above. The device
 # lease takes up to five seconds to stop its old channel and release ownership.
 Start-Sleep -Seconds 6
 $server=Start-Process $service -ArgumentList '--console' -WindowStyle Hidden -PassThru
 $client.WaitForExit(12000)|Out-Null
 if(-not $client.HasExited){throw 'Restart client did not finish'}
 $client.WaitForExit()
 Get-Content "$root\logs\service-loss-test.log"
 Get-Content "$root\logs\service-loss-test-error.log"
 if($null -eq $client.ExitCode -or $client.ExitCode -ne 0){throw "Restart client failed: exit code $($client.ExitCode)"}
 Write-Output "Console loss harness passed; client exit code $($client.ExitCode)."
} finally {
 if($client -and -not $client.HasExited){Stop-Process -Id $client.Id -Force}
 if($server -and -not $server.HasExited){Stop-Process -Id $server.Id -Force}
}
