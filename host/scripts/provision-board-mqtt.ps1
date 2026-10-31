param(
    [string]$Port = 'COM21',
    [Parameter(Mandatory=$true)][string]$Ssid,
    [Parameter(Mandatory=$true)][string]$Password,
    [string]$BrokerHost = '192.168.1.7',
    [ValidateRange(1,65535)][int]$BrokerPort = 1883,
    [string]$Site = 'site_1',
    [string]$Device = 'ed84-001'
    ,[string]$StaticIp
    ,[string]$Gateway
    ,[string]$Netmask = '255.255.255.0'
    ,[string]$MqttUser
    ,[string]$MqttPassword
)

$ErrorActionPreference = 'Stop'
$py = @'
import argparse, os, time, serial
p=argparse.ArgumentParser(); p.add_argument('--port',required=True); p.add_argument('--ssid',required=True); p.add_argument('--password',required=True)
p.add_argument('--broker',required=True); p.add_argument('--broker-port',required=True); p.add_argument('--site',required=True); p.add_argument('--device',required=True); p.add_argument('--static-ip'); p.add_argument('--gateway'); p.add_argument('--netmask',default='255.255.255.0')
a=p.parse_args()
user=os.environ.get('EDGEDOCTOR_PROVISION_MQTT_USER','')
password=os.environ.get('EDGEDOCTOR_PROVISION_MQTT_PASSWORD','')
def quoted(value):
    if any(ord(c)<32 for c in value) or '"' in value or value.endswith('\\'):
        raise SystemExit('Credential cannot be represented by the board shell; use a credential file transfer.')
    return '"'+value+'"'
auth_command='edmqtt auth '+quoted(user)+' '+quoted(password) if user else None
if password and not user: raise SystemExit('MqttUser is required with MqttPassword')
if auth_command and len(auth_command.encode())>=80:
    raise SystemExit('Credential command exceeds the board shell 79-byte limit; use a credential file transfer.')
s=serial.Serial(a.port,115200,timeout=1); time.sleep(1)
def cmd(x, wait=1.0):
    s.write((x+'\r\n').encode()); time.sleep(wait); return s.read(12000).decode('utf-8','replace')
# `wifi join` only associates for this power cycle; edwifi save writes the
# credential file the BSP's Wi-Fi manager replays at boot.
print(cmd('edwifi save '+a.ssid+' '+a.password, 2.0).replace(a.password, '[REDACTED]'))
print(cmd('wifi join '+a.ssid+' '+a.password, 8.0).replace(a.password, '[REDACTED]'))
if a.static_ip and a.gateway: print(cmd('ifconfig w0 '+a.static_ip+' '+a.gateway+' '+a.netmask))
print(cmd('edmqtt stop', 3.0))
response=cmd('edmqtt config '+a.site+' '+a.device+' '+a.broker+' '+a.broker_port)
print(response)
if 'edmqtt: saved' not in response: raise SystemExit('MQTT configuration was not saved; ensure the worker has stopped and retry')
if auth_command:
    response=cmd(auth_command)
    if 'credentials saved' not in response: raise SystemExit('MQTT credentials could not be saved')
    print('MQTT credentials saved')
print(cmd('edmqtt start'))
print(cmd('ifconfig'))
print(cmd('edmqtt info'))
s.close()
'@
$tmp = Join-Path $env:TEMP 'edgedoctor-provision-board.py'
Set-Content -LiteralPath $tmp -Value $py -Encoding UTF8
$previousMqttUser = $env:EDGEDOCTOR_PROVISION_MQTT_USER
$previousMqttPassword = $env:EDGEDOCTOR_PROVISION_MQTT_PASSWORD
$env:EDGEDOCTOR_PROVISION_MQTT_USER = $MqttUser
$env:EDGEDOCTOR_PROVISION_MQTT_PASSWORD = $MqttPassword
try { $args = @('--port',$Port,'--ssid',$Ssid,'--password',$Password,'--broker',$BrokerHost,'--broker-port',$BrokerPort,'--site',$Site,'--device',$Device); if ($StaticIp -and $Gateway) { $args += @('--static-ip',$StaticIp,'--gateway',$Gateway,'--netmask',$Netmask) }; & python $tmp @args; if ($LASTEXITCODE) { exit $LASTEXITCODE } }
finally {
    $env:EDGEDOCTOR_PROVISION_MQTT_USER = $previousMqttUser
    $env:EDGEDOCTOR_PROVISION_MQTT_PASSWORD = $previousMqttPassword
    Remove-Item -LiteralPath $tmp -Force -ErrorAction SilentlyContinue
}
