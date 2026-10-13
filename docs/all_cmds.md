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

Replies to the sender in **one SMS** with the main cellular information.
Case-insensitive; no arguments. No modem commands or registration codes appear
in the reply. Latitude and longitude are always included.

```text
getcellularinfo
```

Example:

```text
Cellular
Carrier=AT&T
Network=Roaming
Signal=-63 dBm
SIM=Ready
IMEI=863866074042672
ICCID=89882280666242663460
Lat=42.056050
Lon=-91.654126
```

- `Carrier`: mobile operator (up to 16 characters).
- `Network`: Connected, Roaming, Searching, Offline, Denied or Unknown.
  Connected and Roaming both mean registered successfully.
- `Signal`: received signal estimate in dBm; a less negative value is stronger.
  Unknown means no valid reading is available.
- `SIM`: Ready, Locked (PIN/PUK required) or Unknown.
- `IMEI`: modem identity; `ICCID`: SIM identity. These help identify a unit.
- `Lat` / `Lon`: cellular location estimate in decimal degrees, to six decimal
  places. Unknown means the modem could not obtain the location; the firmware
  does not substitute zero or a previous position. This is a network-based
  estimate, not a GPS fix.

The reply fits within 160 characters. Registration, signal, SIM status, operator
and identities are refreshed. When a data connection is active, the firmware
also requests a location; this can take up to 30 seconds. Failed identity
queries may report Error or Unknown. The command does not change network
settings.

Signal and readable registration details are logged. Additional modem details
remain available in the firmware logs for troubleshooting, rather than in the
user's SMS. See [cellular signal diagnostics](Cellular_signal_diagnostics.md).

On BL54L15, calibration, relay state, alert settings and heartbeat settings are
persisted in external flash, with readback verification after saves. Internal
settings storage is unused and is not automatically migrated. See
[external flash storage](External_flash_storage.md).

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

### Fuse-detection diagnostic logs

On the BL54L15 carrier, INFO logs report input AC availability and an approximate
frequency when stable optocoupler pulses are measurable:

```text
fuse_detection: AC available, freq ~60.00 Hz (pulse 120.00 Hz)
```

The optocoupler detects both half-cycles; pulse frequency is divided by two for
the mains estimate. A steady active input still means AC available, but logs
frequency unavailable because the RC filter can suppress measurable edges.
Frequency is diagnostic only; fuse/utility alert decisions remain based on
AC presence and socket metering. Logs appear on state changes or roughly every
10 seconds when the main loop reaches its input check.
