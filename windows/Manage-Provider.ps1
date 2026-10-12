# SPDX-License-Identifier: Apache-2.0
param([ValidateSet('Activate','Restore','Status')][string]$Mode='Status')
$ErrorActionPreference='Stop';$ProgressPreference='SilentlyContinue'
$root=Split-Path -Parent $PSScriptRoot
$path='HKLM:\SOFTWARE\WOW6432Node\PassThruSupport.04.04\CANable ISO-TP'
$dll=Join-Path $root 'dist\CanableJ2534.dll'
$snapshot=Join-Path $root 'backups\canable-native-provider-state.json'
$values=[ordered]@{Name='CANable ISO-TP';Vendor='CANable project';FunctionLibrary=$dll;J1850VPW=0;J1850PWM=0;ISO9141=0;ISO14230=0;CAN=0;ISO15765=1;ISO15765_PS=1;SCI_A_ENGINE=0;SCI_A_TRANS=0;SCI_B_ENGINE=0;SCI_B_TRANS=0}
function Verify-Owned {
 $key=Get-Item $path
 if(@($key.GetValueNames()).Count -ne $values.Count){throw 'Own provider contains unexpected values; refusing to overwrite it.'}
 foreach($name in $values.Keys){
  $kind=if($values[$name] -is [string]){'String'}else{'DWord'}
  if($key.GetValue($name) -ne $values[$name] -or $key.GetValueKind($name).ToString() -ne $kind){throw "Own provider mismatch: $name"}
 }
}
if($Mode -eq 'Status'){
 if(Test-Path $path){$key=Get-Item $path;[ordered]@{RegistryPath=$path;Name=$key.GetValue('Name');Vendor=$key.GetValue('Vendor');FunctionLibrary=$key.GetValue('FunctionLibrary');ISO15765=$key.GetValue('ISO15765');ISO15765_PS=$key.GetValue('ISO15765_PS')}|ConvertTo-Json}
 else{Write-Output 'CANable ISO-TP provider is not registered.'}
 return
}
if(Get-Process VCIMediator -ErrorAction SilentlyContinue){throw 'Close Mediator before changing its provider registration.'}
if($Mode -eq 'Activate'){
 if(-not(Test-Path $dll)){throw 'Build the x86 CANable DLL first.'}
 if(Test-Path $path){
  if(-not(Test-Path $snapshot)){throw 'An existing CANable key has no ownership snapshot; refusing to overwrite it.'}
  Verify-Owned
  Write-Output 'CANable ISO-TP provider is already registered.';return
 }
 New-Item -ItemType Directory -Force (Join-Path $root 'backups')|Out-Null
 $state=[ordered]@{Mode='prepared';CapturedUtc=[DateTime]::UtcNow.ToString('o');RegistryPath=$path;BeforeKeyExists=$false;Values=$values;DllSha256=(Get-FileHash $dll -Algorithm SHA256).Hash.ToLowerInvariant()}
 $state|ConvertTo-Json -Depth 6|Set-Content -Encoding UTF8 $snapshot
 try{
  New-Item -Path $path -Force|Out-Null
  foreach($name in $values.Keys){$kind=if($values[$name] -is [string]){'String'}else{'DWord'};New-ItemProperty -Path $path -Name $name -PropertyType $kind -Value $values[$name] -Force|Out-Null}
  Verify-Owned
  $state.Mode='active';$state|ConvertTo-Json -Depth 6|Set-Content -Encoding UTF8 $snapshot
 }catch{if(Test-Path $path){Remove-Item $path -Force};throw}
 Write-Output 'Own CANable ISO-TP profile registered. No Bosch provider values were modified.'
}else{
 if(-not(Test-Path $path)){Write-Output 'Own CANable profile is already removed.';return}
 if(-not(Test-Path $snapshot)){throw 'Own provider ownership snapshot is missing.'}
 $state=Get-Content -Raw $snapshot|ConvertFrom-Json
 if($state.RegistryPath -ne $path -or $state.BeforeKeyExists){throw 'Unexpected own-provider restoration snapshot.'}
 Verify-Owned
 Remove-Item $path -Force
 if(Test-Path $path){throw 'Own provider removal failed.'}
 $state.Mode='restored';$state|ConvertTo-Json -Depth 6|Set-Content -Encoding UTF8 $snapshot
 Write-Output 'Own CANable provider removed. Bosch and other providers were not modified.'
}
