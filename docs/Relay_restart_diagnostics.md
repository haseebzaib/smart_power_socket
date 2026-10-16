# Unexpected restart during DeviceOn / DeviceOff

The supplied SMS history includes `SPS is starting` after a relay command.
That heading is sent by the application's startup path, but the screenshot
does not establish the reset mechanism or implicate SPI flash. The user has
already reproduced the issue with loads disconnected; investigation remains
focused on the firmware path.

DeviceOn and DeviceOff change selected GPIO outputs, save the full relay mask,
then send an SMS acknowledgement. Neither calls an MCU reboot function. The
Reboot commands cycle socket outputs; they do not intentionally reboot the MCU.

## Changes

- Relay outputs keep their input buffers connected. Nordic's output-only GPIO
  configuration disconnects the input buffer, while the existing state-reporting
  code reads the input register. Readback now has an enabled input path without
  changing relay polarity or initial output levels.
- Already-correct outputs skip GPIO writes. A mask matching the last loaded or
  successfully verified saved mask skips flash access altogether.
- A write or readback failure invalidates the saved-mask cache, so the next
  command cannot incorrectly skip persistence using an uncertain old value.
- GPIO/read/persistence errors are logged. DeviceOn/DeviceOff return GPIO or
  save errors rather than unconditional OK; reboot commands report save errors.
- Settings write/readback stages, relay switching and SMS reply stages are
  logged. The log buffer is increased to 4 KiB to reduce dropped burst logs.
- Every boot reports the hardware reset-cause bits and then clears the latched
  cause, so later boots do not accumulate the previous reason.
- The main stack stays at 16 KiB. Logging-thread stack is explicitly 2 KiB and
  interrupt stack is 4 KiB (the inspected older build used 768 bytes and 2 KiB
  respectively). Hardware stack protection is explicitly enabled. Zephyr thread
  analyzer reports thread and interrupt stack usage through INFO logs every
  60 seconds, independently of the main-loop modem operations. This enables
  stack initialization/monitoring and runtime statistics; the analyzer itself
  has a 2 KiB stack. These diagnostics add RAM and runtime overhead and do not
  prove that a prior reset was caused by stack exhaustion.

These changes fix state tracking/error handling and improve isolation. They do
not establish that the reported MCU restart has been fixed.

## Reading a capture

Keep RTT connected while sending DeviceOff, then DeviceOn. Capture the lines
before any restart and the next boot's `Boot reset cause` line. A typical path
for a changed relay mask is:

```text
DeviceOff: relay switching begin
Relay 1 switch begin: OFF (GPIO 0 -> 1)
Relay 1 switch complete: ret=0
DeviceOff: relay switching complete ret=0
Relay persistence begin: mask=0x0e
Settings write begin: sps/relay
Settings readback begin: sps/relay
Settings persisted and verified: sps/relay (8 bytes)
DeviceOff: persistence complete ret=0; SMS reply begin
DeviceOff: SMS reply complete sent=1
```

Now repeat the exact same valid command. The logs should show both
`GPIO write skipped` and `flash access skipped`, then an SMS reply. If the
restart persists on that path, the command's relay write and flash-save stages
were bypassed; inspect the remaining command/SMS path and reset diagnostics.
If it only happens for a changed mask, the stage logs narrow where execution
last progressed. Deferred RTT logs can be lost on reset, so the last visible
line is a clue rather than proof of the faulting instruction.

Reset logs include a hexadecimal bitmask and decoded pin, software, watchdog,
CPU-lockup and debug flags. The nRF driver does not classify every reset source:
a zero mask means no classified cause, and cannot prove a brownout. Nonzero
software reset also does not establish which code caused it. Preserve any
Zephyr fault/register dump printed before the next boot.

Existing GPIO semantics remain 0=logical ON, 1=logical OFF. No partition,
external-flash address, relay pin or MCU reboot command was changed here.
No project build or hardware reproduction was run for this investigation.

## Meter initialization retries

`configure ret: 5` is `HLW811X_NO_RESPONSE` in the bundled meter driver: a
configuration operation received no expected response. It does not indicate
stack exhaustion. Individual configuration failures already trigger retries
with a 30-second delay, using that outlet's saved calibration ratio.

The retry runs in the main loop, not a separate timer/thread. Modem startup,
AT timeouts, location queries or SMS operations can delay it beyond 30 seconds.
Retry-start, failure/next-delay and recovery messages now make progress visible:

```text
HLW811x meter 1 configure retry begin (previous error=5)
HLW811x meter 1 configure retry succeeded; measurements resume next loop
```

Successful retry clears the error and normal readings resume on the following
loop. A failed retry schedules the next attempt 30 seconds after that attempt
finishes. This retry path handles configuration failures; transient measurement
errors currently clear that sample and retry reading on a later loop, without
automatically reconfiguring an otherwise configured meter.
