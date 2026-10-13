# SPS SMS Commands and Alerts

All command names are case-insensitive. Numeric arguments are not case-sensitive because they are digits.

## Status Commands

### `getStatus`

Requests the main status report, followed by the outlet report.

Example:

```text
getStatus
```

Main status response:

```text
Utility=ON
Device=0110
Outlet=1111
RSL=-101
Batt=4.44
Lat=42.027998
Lon=-91.631620
FW=001.00
HBD=0.000
UpDays=0.123
```

Outlet report response:

```text
Outlet=1111
1=119.640V,0.001A,-0.064W
2=119.390V,0.002A,-0.031W
3=119.340V,0.035A,4.262W
4=119.190V,0.001A,0.000W
```

Fields:

- `Utility`: `ON` when AC voltage is present on any metering channel.
- `Device`: monitored load/device power state per outlet.
- `Outlet`: relay state per outlet, where `1` means relay ON.
- `RSL`: modem signal in dBm.
- `Batt`: battery voltage.
- `HBD`: heartbeat interval in days.
- `UpDays`: firmware uptime in days.

### `getcellularinfo`

Requests fresh read-only cellular diagnostics and replies to the command sender.
The command is case-insensitive and takes no arguments:

```text
getcellularinfo
```

The report is sent as separate, numbered SMS messages headed
`CellularInfo part 1`, `CellularInfo part 2`, etc. Each message is at most 160
characters. Long fields can continue in the next message; read the parts in
number order. The number of parts depends on the modem's responses. If a send
fails, transmission stops; resend the command to request a new complete report.

Example reassembled report (illustrative values):

```text
CellularInfo
Model=SIM7080G
ModemFW=<modem firmware revision>
IMEI=<15-digit modem IMEI>
ICCID=<SIM ICCID>
IMSI=<subscriber identity>
Carrier=T-Mobile
SIM=READY
CREG=5
CEREG=0,5
CSQ=18,99
RSL=-77
COPS=0,0,"T-Mobile",7
CNMP=38
CMNB=1
APN=iot.1nce.net
CNACT=0,1,"10.0.0.1"
Lat=42.027998
Lon=-91.631620
CPSI=<raw serving-cell information>
```

Fields:

| Field | Meaning |
| --- | --- |
| `Model`, `ModemFW` | Actual modem model (`AT+GMM`) and firmware revision (`AT+CGMR`). |
| `IMEI`, `ICCID`, `IMSI` | Modem, SIM card, and subscriber identities. |
| `Carrier`, `COPS` | Operator name and raw operator selection/technology response. |
| `SIM` | SIM PIN/readiness status. |
| `CREG` | Registration status: 1 = home, 5 = roaming; other values indicate no confirmed registration. |
| `CEREG` | Raw EPS/LTE registration response; the second value is registration status. |
| `CSQ` | Raw RSSI index and BER code. `18,99` produces `RSL=-77`; BER 99 means unknown. |
| `RSL` | CSQ-derived signal estimate in dBm; `Unknown` if unavailable or CSQ RSSI is 99. |
| `CNMP`, `CMNB` | Raw preferred radio modes; 38 = LTE only, and CMNB 1 = CAT-M, 2 = NB-IoT, 3 = both. |
| `APN` | APN configured in the firmware. |
| `CNACT` | Fresh raw data-context activation states and IP addresses. |
| `Lat`, `Lon` | Last location cached by the normal modem loop; not a fresh GPS fix from this command. |
| `CPSI` | Raw serving-cell/system report: radio mode, network/cell/band, and LTE RSRQ/RSRP/RSSI/RSSNR when available. |

Unsupported, failed, empty, or oversized diagnostic responses show `Unavailable`;
existing identity/PIN/carrier queries may show `Error` or `Unknown`. Failed CSQ
queries show `CSQ=-1,-1` and `RSL=Unknown`; a valid RSSI code of 99 also means
signal unknown. Existing status/alert messages use `RSL=-1` as the unavailable
sentinel, not a measured signal strength.

The command does not switch carrier, radio mode, or activate a data connection.
It makes multiple AT queries, so delivery can take time if queries time out.
See [cellular signal diagnostics](Cellular_signal_diagnostics.md) for the −77 investigation.

### `GetOutletData`

Requests detailed metering data. Without an outlet number, the device sends one SMS per outlet.

Examples:

```text
GetOutletData
OutletData
GetOutletData 1
GetOutletData=4
```

Response:

```text
OutletData=1
V=119.640V
I=0.001A
P=-0.064W
S=0.120VA
E=123Wh
F=60.00Hz
PF=0.99
```

Fields:

- `V`: RMS voltage.
- `I`: RMS current.
- `P`: real/active power.
- `S`: apparent power.
- `E`: energy from the metering IC.
- `F`: AC frequency.
- `PF`: power factor.

## Relay Commands

Relay logic is active-low in hardware, but SMS commands use logical ON/OFF.

Relay state is saved to flash after these commands. On reboot, the device restores the saved relay state. If no saved state exists, all relays default ON.

### `DeviceOn`

Turns ON outlets selected by a 4-bit mask.

```text
DeviceOn=1001
```

This turns ON outlets 1 and 4.

Response:

```text
OK DeviceOn
```

### `DeviceOff`

Turns OFF outlets selected by a 4-bit mask.

```text
DeviceOff=0110
```

This turns OFF outlets 2 and 3.

Response:

```text
OK DeviceOff
```

### `DeviceReboot`

Reboots outlets selected by a 4-bit mask.

```text
DeviceReboot=0101
```

This reboots outlets 2 and 4.

If an outlet is already ON, reboot is OFF, wait, ON. If an outlet is OFF, reboot is ON, wait, OFF, wait, ON.

Response:

```text
OK DeviceReboot
```

### `Reboot`

Reboots one outlet.

```text
Reboot 1
Reboot=4
```

Response:

```text
OK Reboot
```

## Heartbeat Command

### `SetHeartBeatDays`

Sets the delay between periodic heartbeat messages. The value supports up to three decimal places
and is saved to flash. `0` disables periodic heartbeats.

```text
SetHeartbeatDays=30.43
```

Response:

```text
OK SetHeartbeatDays=30.430
```

The schedule uses monotonic device uptime and therefore restarts after a device reboot. If the
network is unavailable when a heartbeat is due, one heartbeat is sent when the network returns;
missed heartbeats are not sent as a burst.

The heartbeat uses the normal status and outlet-report payloads with an `SPS Heartbeat` heading.

## Device Power Detection Settings

The thresholds are universal: one setting applies to all four outlets. Values are in watts and
are saved to flash.

At startup, an outlet below `1.000 W` is initialized off and an outlet at or above `1.000 W` is
initialized on, without generating an alert. At runtime, an off outlet must rise above
`OnThreshold` to change on, and an on outlet must fall below `OffThreshold` to change off. Between
the two thresholds the previous state is retained.

Defaults:

```text
OnThreshold=1.2
OffThreshold=0.7
```

Responses:

```text
OK OnThreshold=1.200
OK OffThreshold=0.700
```

Read the currently active values:

```text
GetThresholds
GetOnThreshold
GetOffThreshold
```

Responses:

```text
OnThreshold=1.200
OffThreshold=0.700

OnThreshold=1.200
OffThreshold=0.700
```

`OffThreshold` must be lower than `OnThreshold`; invalid values return an error.

## Power Factor Alert Setting

Power-factor alerts are off by default. The setting is saved to flash.

```text
PowerFactorAlerts=On
PowerFactorAlerts=Off
```

Responses:

```text
OK PowerFactorAlerts=On
OK PowerFactorAlerts=Off
```

## Startup Message

After modem/network readiness, the device sends the same data as `getStatus`, with a startup heading first:

```text
SPS is starting
Utility=ON
Device=0110
Outlet=1111
RSL=-101
Batt=4.44
Lat=42.027998
Lon=-91.631620
FW=001.00
HBD=0.000
UpDays=0.001
```

It then sends the outlet report.

## Alerts

Alerts are sent to the configured alert number after network readiness.

### Utility Power Lost

Sent when utility AC is no longer detected.

```text
Alert=Utility power lost
Utility=OFF
Device=0110
Outlet=1111
RSL=-101
Batt=4.44
Lat=42.027998
Lon=-91.631620
FW=001.00
HBD=0.000
```

No individual outlet report is sent after utility lost.

### Utility Power Lost — Fuse Probably Blown

Sent when input AC is detected on `FUSE_DETECTION` (P1.12), but no metering
channel detects socket voltage (all below 50 V), for three consecutive
network-ready alert polls. The input is active-low and sampled over up to
40 ms to catch AC pulses. AC_IN must be before the fuse and metering after it.

```text
Alert=Utility power lost - fuse probably blown
Utility=OFF
Device=0110
Outlet=1111
RSL=-101
Batt=4.44
Lat=42.027998
Lon=-91.631620
FW=001.00
HBD=0.000
```

This extends the utility-loss alert; it does not add an incoming SMS command.
No individual outlet report follows it. The diagnosis is probable because a
metering fault or another downstream interruption can give the same readings.
If input AC is absent or the sensor is unavailable, the normal loss alert is
used. If input AC returns during an existing outage without socket voltage,
the fuse alert is sent after confirmation. Failed sends are retried; a successful
fuse alert is sent once until socket voltage returns. The existing restoration
alert and outlet report then follow as before.

### Utility Power Restored

Sent when utility AC returns.

```text
Alert=Utility power restored
Utility=ON
Device=0110
Outlet=1111
RSL=-101
Batt=4.44
Lat=42.027998
Lon=-91.631620
FW=001.00
HBD=0.000
```

After this alert, the device sends the outlet report.

### Device Power Lost/Restored

Sent per outlet when a load/device unexpectedly disconnects or reconnects while utility and that
outlet's relay are ON. Intentional `DeviceOff`, `DeviceOn`, and relay-off periods reset the
monitoring baseline and do not produce device power lost/restored alerts.

The monitor averages five active-power readings independently for each outlet. The universal on
and off thresholds provide hysteresis, and a state change must remain valid for three consecutive
averaged readings before an alert is sent. Negative active-power readings are treated and
reported as zero.

```text
Alert=Device power lost
Outlet=3
3=119.340V,0.000A,0.000W
```

```text
Alert=Device power restored
Outlet=3
3=119.340V,0.035A,4.262W
```

### Frequency Bad/Restored

Frequency is considered bad below `58.00Hz` or above `61.00Hz`.

```text
Alert=Frequency bad
Freq=57.80Hz
```

```text
Alert=Frequency restored
Freq=60.01Hz
```

### Power Factor Bad/Restored

Sent per outlet when a connected load has poor power factor and `PowerFactorAlerts` is enabled.

```text
Alert=Power factor bad
PF=0.72
Outlet=2
2=119.390V,0.500A,31.000W
```

```text
Alert=Power factor restored
PF=0.90
Outlet=2
2=119.390V,0.500A,53.000W
```

## Calibration

Calibration is per individual outlet/socket. Each outlet has a separate HLW811x IC path, so factory calibration should be run for outlet 1, then 2, then 3, then 4.

`K1_A` and `K1_B` are fixed by hardware:

```text
K1_A=3.0
K1_B=1.0
```

`K2` is saved per outlet in flash. Default `K2` is:

```text
K2=1.064
```

### Ask For Calibration Instructions

```text
RunCalibration
```

Response:

```text
RunCalibration
Use: RunCalibration 1 V=120.000
Optional: F=60.00 P=60.000
```

### Run Outlet Calibration

Use a trusted multimeter voltage as `V`.

```text
RunCalibration 1 V=120.000
RunCalibration 2 V=120.000
RunCalibration 3 V=120.000
RunCalibration 4 V=120.000
```

Optional factory sanity checks:

```text
RunCalibration 1 V=120.000 F=60.00 P=60.000
```

`F` checks measured frequency against the reference. `P` or `Load` checks active power roughly against the reference load and reports a warning if it is far away. K2 is still calculated from voltage only.

### K2 Formula

K2 is the voltage divider scaling relative to the datasheet nominal divider.

```text
new_K2 = old_K2 * hlw_voltage_reading / reference_voltage
```

Example:

```text
HLW reading = 128.7 V
Multimeter = 121.0 V
old_K2 = 1.0

new_K2 = 1.0 * 128.7 / 121.0 = 1.0636
```

Firmware stores K2 as fixed point internally and applies it immediately after saving.

Calibration success response:

```text
Calibration=OK
Outlet=1
Vref=120.000V
Vmeas=128.700V
K2=1.072500
```

Possible errors:

```text
ERR RunCalibration target
ERR RunCalibration outlet
ERR RunCalibration V
ERR RunCalibration F
ERR RunCalibration P
ERR RunCalibration low voltage
ERR RunCalibration freq
ERR RunCalibration K2
ERR RunCalibration save
ERR RunCalibration apply
```
