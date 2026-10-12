# SPDX-License-Identifier: Apache-2.0
param([ValidateSet('Activate','Restore','Status')][string]$Mode='Status')
$ErrorActionPreference='Stop';$ProgressPreference='SilentlyContinue'
$workspace=Split-Path -Parent $PSScriptRoot
$root='C:\Program Files (x86)\Stellantis_VCIMediator'
$bridge=Join-Path $workspace 'dist\mediator\CanablePcmBridge.dll'
$snapshot=Join-Path $workspace 'backups\canable-mediator-bridge-state.json'
Add-Type -TypeDefinition @'
using System;
using System.Text;
using System.Runtime.InteropServices;
public static class CanableMediatorIni {
 [DllImport("kernel32.dll",CharSet=CharSet.Unicode,SetLastError=true)]
 public static extern bool WritePrivateProfileString(string section,string key,string value,string path);
 [DllImport("kernel32.dll",CharSet=CharSet.Unicode)]
 public static extern uint GetPrivateProfileString(string section,string key,string fallback,StringBuilder value,uint size,string path);
}
'@
function Read-Setting($file,$section,$name){
 $buffer=New-Object Text.StringBuilder 1024
 [void][CanableMediatorIni]::GetPrivateProfileString($section,$name,'[ABSENT]',$buffer,1024,$file)
 return $buffer.ToString()
}
$settings=@(
 [pscustomobject]@{File=(Join-Path $root 'drv\VCIManager.ini');Section='COMMON';Key='PCMToJ2534LibPath';Value=$bridge;ExpectedBefore='[ABSENT]'},
 [pscustomobject]@{File=(Join-Path $root 'WSCore.ini');Section='PCMToJ2534';Key='Path';Value=((Split-Path $bridge -Parent)+'\');ExpectedBefore='.\drv\'},
 [pscustomobject]@{File=(Join-Path $root 'WSCore.ini');Section='PCMToJ2534';Key='Composant';Value='CanablePcmBridge';ExpectedBefore='PCMToJ2534'}
)
if($Mode -eq 'Status'){
 $settings|ForEach-Object {[ordered]@{File=$_.File;Section=$_.Section;Key=$_.Key;Value=(Read-Setting $_.File $_.Section $_.Key)}}|ConvertTo-Json
 return
}
if(Get-Process VCIMediator -ErrorAction SilentlyContinue){throw 'Close Mediator before changing its component paths.'}
if($Mode -eq 'Restore'){
 if(-not(Test-Path $snapshot)){throw 'Mediator bridge restoration snapshot is missing.'}
 $state=Get-Content -Raw $snapshot|ConvertFrom-Json
 if($state.Mode -eq 'restored'){Write-Output 'Mediator bridge settings are already restored.';return}
 foreach($item in $state.Files){if((Get-FileHash $item.File -Algorithm SHA256).Hash.ToLowerInvariant() -ne $item.AfterHash){throw 'Mediator INI changed outside this integration; refusing to overwrite it.'}}
 foreach($item in $state.Files){
  Copy-Item $item.Backup $item.File -Force
  if((Get-FileHash $item.File -Algorithm SHA256).Hash.ToLowerInvariant() -ne $item.BeforeHash){throw 'Mediator INI restoration hash differs.'}
 }
 $state.Mode='restored';$state|ConvertTo-Json -Depth 7|Set-Content -Encoding UTF8 $snapshot
 Write-Output 'Original Mediator component INI files restored byte-for-byte.';return
}
if(-not(Test-Path $bridge)){throw 'Build the own CANable Mediator bridge first.'}
$native=Join-Path $root 'drv\PCMToJ2534.dll'
if((Get-FileHash $native -Algorithm SHA256).Hash.ToLowerInvariant() -ne '13f5964cd117a82bd604a64c2ce042346f2001673d32b8b38cdb810d8ad11f51'){throw 'Native PCMToJ2534 ABI has changed; review the bridge first.'}
if(Test-Path $snapshot){
 $previous=Get-Content -Raw $snapshot|ConvertFrom-Json
 if($previous.Mode -eq 'active'){
  foreach($item in $previous.Files){if((Get-FileHash $item.File -Algorithm SHA256).Hash.ToLowerInvariant() -ne $item.AfterHash){throw 'Active Mediator bridge configuration differs.'}}
  foreach($item in $settings){if((Read-Setting $item.File $item.Section $item.Key) -ne $item.Value){throw 'Active component selector differs from the own CANable bridge.'}}
  Write-Output 'Own CANable Mediator bridge paths are already active.';return
 }
}
foreach($item in $settings){if((Read-Setting $item.File $item.Section $item.Key) -ne $item.ExpectedBefore){throw "Unexpected existing component path: $($item.Key)"}}
if((Read-Setting (Join-Path $root 'WSCore.ini') 'PCMToJ2534' 'Composant') -ne 'PCMToJ2534'){throw 'Unexpected native component name.'}
$folder=Join-Path $workspace ('backups\mediator-bridge-'+(Get-Date -Format 'yyyyMMdd-HHmmss-fff'))
New-Item -ItemType Directory -Force $folder|Out-Null
$files=@($settings.File|Select-Object -Unique|ForEach-Object {
 $file=$_
 $backup=Join-Path $folder ([IO.Path]::GetFileName($file))
 Copy-Item $file $backup
 [pscustomobject]@{File=$file;Backup=$backup;BeforeHash=(Get-FileHash $backup -Algorithm SHA256).Hash.ToLowerInvariant();AfterHash=$null}
})
$state=[ordered]@{Mode='prepared';CapturedUtc=[DateTime]::UtcNow.ToString('o');Files=$files;Settings=$settings;BridgeSha256=(Get-FileHash $bridge -Algorithm SHA256).Hash.ToLowerInvariant()}
$state|ConvertTo-Json -Depth 7|Set-Content -Encoding UTF8 $snapshot
try{
 foreach($item in $settings){
  if(-not[CanableMediatorIni]::WritePrivateProfileString($item.Section,$item.Key,$item.Value,$item.File)){throw 'Mediator component path update failed.'}
  if((Read-Setting $item.File $item.Section $item.Key) -ne $item.Value){throw 'Mediator component path verification failed.'}
 }
 foreach($item in $files){$item.AfterHash=(Get-FileHash $item.File -Algorithm SHA256).Hash.ToLowerInvariant()}
 $state.Mode='active';$state|ConvertTo-Json -Depth 7|Set-Content -Encoding UTF8 $snapshot
}catch{foreach($item in $files){Copy-Item $item.Backup $item.File -Force};throw}
Write-Output 'Own CANable bridge selected for VCIManager discovery and the PCMToJ2534 RPC component. Native DLLs were not replaced.'
