# SPDX-License-Identifier: Apache-2.0
# Read-only inventory. The firmware advertises WINUSB through MS OS 2.0.
$ErrorActionPreference='Stop'
$root='C:\codex-canable-vci'
$devices=Get-PnpDevice -PresentOnly | Where-Object { $_.InstanceId -like 'USB\VID_1209&PID_0001*' }
$result=@($devices | ForEach-Object {
    $service=Get-PnpDeviceProperty -InstanceId $_.InstanceId -KeyName 'DEVPKEY_Device_Service' -ErrorAction SilentlyContinue
    [pscustomobject]@{InstanceId=$_.InstanceId;Status=$_.Status;Name=$_.FriendlyName;DriverService=$service.Data}
})
ConvertTo-Json -InputObject $result -Depth 4 | Set-Content -Encoding UTF8 (Join-Path $root 'logs\usb-driver-inventory.json')
$result | Format-List
