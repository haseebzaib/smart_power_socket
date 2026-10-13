# Cellular signal diagnostics

Use `getcellularinfo` to collect fresh modem responses, including CSQ and CPSI,
from an SPS reporting a constant `RSL=-77`. The full command and fields are
listed in `all_cmds.md` and `sms_commands_client.html`.

## Firmware findings

`sim7080::loop()` queries `AT+CSQ` each iteration. For accepted RSSI codes 0–31,
`get_network_quality()` converts the raw code using `2 * code - 113`.
`device_status::make_snapshot()` copies that result into the RSL report.
There is no −77 constant or clamp in this path. A raw RSSI code of 18 maps to
−77 dBm. The AT engine clears its response buffer for each command, so the
code does not intentionally cache a fixed CSQ response.

The client examples demonstrate varying readings on the SPAs, but do not
establish why SPS readings are fixed. A modem returning CSQ 18 repeatedly,
radio/modem firmware behavior, or response/timing issues could account for
it. The cause requires raw readings from an affected unit; this review cannot
confirm it from the reported RSL value alone.

Two separate firmware error-handling issues were corrected:

- CSQ RSSI 99 (unknown) previously became 0 dBm. Unknown, malformed,
  out-of-range, or failed readings now invalidate signal and retain −1 as the
  legacy status/alert sentinel. The diagnostic command reports `RSL=Unknown`
  rather than treating −1 as a measurement.
- A CPIN prefix parsing error previously wrote −1 into signal strength instead
  of updating SIM status. It now sets the PIN status to Error without altering
  the CSQ result.

The modem model was previously a fixed `SIM7080_LTD` string. It is now queried
with `AT+GMM`, so diagnostics identify the actual modem. Normal valid CSQ
conversion was retained; no signal values or carrier-selection settings were
forced to change.

## Reading an affected unit

1. Request `getcellularinfo` repeatedly and compare the raw CSQ index, converted
   RSL, COPS operator/technology, and CPSI serving-cell report.
2. If CSQ changes, RSL should change by 2 dB per code step. If the raw code stays
   18, −77 is the conversion of the modem response rather than an application
   constant. This does not by itself prove the modem's estimate is accurate.
3. Compare CPSI signal fields across reports. For CAT-M/NB-IoT, the manual lists
   system/operation mode, MCC-MNC, TAC, serving/physical cell IDs, band, channel,
   bandwidths, RSRQ, RSRP, RSSI, and RSSNR. These raw fields are preserved;
   do not equate RSRP directly with the CSQ-derived RSL.
4. If CSQ remains fixed while CPSI measurements change, record Model and ModemFW
   alongside the readings for modem/vendor investigation. COPS/CPSI also help
   establish whether the serving network or radio mode changed.
5. If possible, capture direct `AT+CSQ` and `AT+CPSI?` responses from the same
   affected hardware and compare them with the SMS diagnostics. No affected
   hardware or direct AT captures were available during this review.

The command performs sequential queries, so its fields are not sampled at one
instant. Location is cached from the normal modem loop. Unknown/unsupported
responses are marked rather than replaced with plausible signal values.

## Reference

The project includes `sim708ATCommandManual.pdf` (SIM7070/SIM7080/SIM7090 AT
Command Manual V1.07): CSQ section 3.2.16, CNMP/CMNB sections 5.2.16–17,
CPSI section 5.2.19, and EPS registration section 6.2.7.
No project build or hardware test was run for these changes.
