# Cellular signal diagnostics

`getcellularinfo` sends one SMS with Carrier, Network, Signal, SIM status,
IMEI, ICCID, Lat and Lon. The response uses plain names and includes no raw
modem commands. Location is requested when data is connected; unavailable
coordinates appear as Unknown, never zero or a cached old position.
The location is a cellular estimate, not a GPS fix. Full command documentation
is in `all_cmds.md` and `sms_commands_client.html`.

## Findings from the supplied logs

At uptime 03:04 the SIM is READY but registration is 2 (searching), signal is
unavailable (-1), and the data connection is inactive. At uptime 05:42 the modem
reports AT&T, registration 5 (roaming), signal -63 dBm, and an active data
context with IP address 100.65.3.220. This shows that the modem subsequently
registered and obtained data connectivity. Registration 5 is a successful
registration on a roaming network.

The SMS screenshot also reports CSQ=24,99 and RSL=-65. That conversion is
consistent with the code. The later -63 value corresponds to CSQ RSSI code 25.
These observations demonstrate changing signal readings on this unit; it is
not stuck at -77 in these logs. They do not identify the cause of the client's
earlier fixed readings on other units.

## New INFO-level logs

Every signal poll reports the raw response and its interpretation:

```text
Signal response: +CSQ: 25,99
Signal: -63 dBm (raw RSSI=25, BER=99)
Registration: Registered roaming (5)
```

RSSI code 99 produces an explicit unknown-signal message; a failed signal query
logs its result. Malformed or invalid values are logged. BER 99 means BER is
unknown and does not invalidate a valid RSSI code.

About every 30 seconds, the normal modem loop also logs read-only diagnostics:

```text
LTE registration response: 0,5
Serving cell response: LTE CAT-M1,Online,...
```

The LTE registration response retains the CEREG field order (notification mode,
then registration status). The serving-cell response retains CPSI field order:
for CAT-M/NB-IoT, system/operation mode, MCC-MNC, TAC, serving/physical cell IDs,
band, channel, bandwidths, RSRQ, RSRP, RSSI, and RSSNR. Failed or unsupported
queries log that diagnostics are unavailable. Poll timing depends on other main
loop/modem operations; diagnostic query timeouts can delay the loop. These logs
are available even while unregistered, when SMS replies cannot be delivered.

## Signal interpretation

`sim7080::loop()` queries `AT+CSQ` each iteration. Accepted RSSI codes 0–31 are
converted using `2 * code - 113`. Raw code 18 maps to -77 dBm; there is no -77
constant or clamp. `networkQuality=-1` is the unavailable sentinel, not a
measured -1 dBm signal. It does not control registration. The compact SMS uses
`Signal=Unknown` instead of displaying the sentinel.

Previously corrected issues remain: CSQ RSSI 99 no longer becomes 0 dBm, and
CPIN prefix parsing errors no longer overwrite signal strength. The actual
model is queried with AT+GMM rather than supplied as a fixed string.

For an affected unit, compare repeated raw CSQ responses and interpreted dBm
logs with the serving-cell measurements and operator. If CSQ stays at 18 while
CPSI measurements change, preserve the logs and modem revision for further
investigation. Do not directly equate RSRP with the CSQ-derived RSL.

The supplied `sim708ATCommandManual.pdf` documents CSQ in section 3.2.16,
CPSI in section 5.2.19, and EPS registration in section 6.2.7.
No project build or hardware test was run for these changes.
