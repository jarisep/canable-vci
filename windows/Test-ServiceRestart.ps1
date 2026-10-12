# SPDX-License-Identifier: Apache-2.0
$ErrorActionPreference='Stop';$ProgressPreference='SilentlyContinue'
$root=Split-Path -Parent $PSScriptRoot
$client=$null
try {
 $ready=Join-Path $root 'logs\service-loss-ready.txt'
 Remove-Item $ready -Force -ErrorAction SilentlyContinue
 $client=Start-Process (Join-Path $root 'build\stage3\api_test.exe') -ArgumentList @((Join-Path $root 'dist\CanableJ2534.dll'),'--service-loss') -WindowStyle Hidden -PassThru -RedirectStandardOutput (Join-Path $root 'logs\scm-restart-test.log') -RedirectStandardError (Join-Path $root 'logs\scm-restart-test-error.log')
 $null=$client.Handle
 $deadline=(Get-Date).AddSeconds(10)
 while(-not (Test-Path $ready) -and (Get-Date) -lt $deadline){if($client.HasExited){throw 'Client exited before readiness'};Start-Sleep -Milliseconds 100}
 if(-not (Test-Path $ready)){throw 'Client readiness timed out'}
 Restart-Service CodexCanableVci
 (Get-Service CodexCanableVci).WaitForStatus('Running',[TimeSpan]::FromSeconds(10))
 if(-not $client.WaitForExit(20000)){throw 'Restart client timed out'}
 $client.WaitForExit()
 Get-Content (Join-Path $root 'logs\scm-restart-test.log')
 Get-Content (Join-Path $root 'logs\scm-restart-test-error.log')
 if($null -eq $client.ExitCode -or $client.ExitCode -ne 0){throw "Restart client failed: $($client.ExitCode)"}
 Write-Output 'Installed SCM service restart harness passed.'
} finally {
 if($client -and -not $client.HasExited){Stop-Process -Id $client.Id -Force}
 if((Get-Service CodexCanableVci).Status -ne 'Running'){Start-Service CodexCanableVci}
}
