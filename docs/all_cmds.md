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
- `Device`: load/device connected state per outlet, derived from current or apparent power.
- `Outlet`: relay state per outlet, where `1` means relay ON.
- `RSL`: modem signal in dBm.
- `Batt`: battery voltage.
- `HBD`: heartbeat days setting.
- `UpDays`: firmware uptime in days.

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

Command is reserved in firmware and currently only logs receipt.

```text
SetHeartBeatDays
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

Sent per outlet when a load/device disconnects or reconnects while utility is ON.

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

Sent per outlet when a connected load has poor power factor.

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
