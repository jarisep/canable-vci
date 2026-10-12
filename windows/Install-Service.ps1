# SPDX-License-Identifier: Apache-2.0
param([Parameter(Mandatory=$true)][ValidatePattern("^[0-9A-Fa-f]{24}$")][string]$Serial)
$ErrorActionPreference='Stop';$ProgressPreference='SilentlyContinue'
$root=Split-Path -Parent $PSScriptRoot
$exe=Join-Path $root 'dist\CanableVciService.exe'
if(-not (Test-Path $exe)){throw 'Build the service first.'}
New-Item -ItemType Directory -Force (Join-Path $root 'logs')|Out-Null
$expected='"'+$exe+'" --service'
$sid=[Security.Principal.WindowsIdentity]::GetCurrent().User.Value
"[Service]`r`nPipeName=\\.\pipe\CodexCanableVci`r`nAllowedUserSid=$sid`r`n[Device]`r`nSerial=$Serial`r`n" | Set-Content -Encoding ASCII (Join-Path $root 'dist\CanableVciService.ini')
"[Service]`r`nPipeName=\\.\pipe\CodexCanableVci`r`n" | Set-Content -Encoding ASCII (Join-Path $root 'dist\CanableJ2534.ini')
$existing=Get-CimInstance Win32_Service -Filter "Name='CodexCanableVci'"
if($existing){if($existing.PathName -ne $expected){throw 'An unrelated service uses the reserved name; refusing to replace it.'}}
else {New-Service -Name CodexCanableVci -DisplayName 'CANable ISO-TP VCI development service' -BinaryPathName $expected -StartupType Manual | Out-Null}
Start-Service CodexCanableVci
$service=Get-Service CodexCanableVci
$service.WaitForStatus('Running',[TimeSpan]::FromSeconds(10))
for($attempt=0;$attempt -lt 3;$attempt++){
 & $exe --status
 if($LASTEXITCODE -eq 0){break}
 Start-Sleep -Milliseconds 300
}
if($LASTEXITCODE -ne 0){throw 'Service pipe verification failed.'}
[pscustomobject]@{Name=$service.Name;Status=$service.Status.ToString();BinaryPath=$expected;AllowedUserSid=$sid;Startup='Manual';Account='LocalSystem'}|ConvertTo-Json|Set-Content -Encoding UTF8 (Join-Path $root 'logs\service-install.json')
