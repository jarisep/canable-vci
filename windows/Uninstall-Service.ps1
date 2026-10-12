# SPDX-License-Identifier: Apache-2.0
$ErrorActionPreference='Stop'
$root=Split-Path -Parent $PSScriptRoot
$expected='"'+(Join-Path $root 'dist\CanableVciService.exe')+'" --service'
$service=Get-CimInstance Win32_Service -Filter "Name='CodexCanableVci'"
if(-not $service){return}
if($service.PathName -ne $expected){throw 'Service path differs; refusing to remove an unrelated service.'}
Stop-Service CodexCanableVci
& sc.exe delete CodexCanableVci
if($LASTEXITCODE -ne 0){throw 'Service removal failed.'}
