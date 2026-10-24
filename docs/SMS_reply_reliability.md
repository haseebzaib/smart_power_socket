# Generic outgoing SMS retries and load detection

Every command reply, acknowledgement, error response, calibration result,
startup report, heartbeat and alert goes through the same modem-owned RAM
outbox. There is no command-specific getStatus retry queue.

## Queue and retry behavior

`send_sms()` copies the recipient and payload into the outbox. True means
accepted into RAM, not submitted to the modem or delivered to the recipient.
The queue holds 24 messages, each with a recipient up to 31 bytes and payload
up to 255 bytes. These cover all current application response buffers. The
existing modem text-message submission format is unchanged.

The main thread services the queue when network registration is home or
roaming and the existing cooldown permits transmission. Up to four messages
are attempted per service call. A failed message retains its
original payload and ID; further attempts wait 60 seconds. Scheduling is
round-robin across recipients and FIFO within each recipient, so an invalid
alert destination cannot permanently prevent another recipient from receiving
command replies. Only a successful
modem submission removes a message. No command handler is re-executed to retry
its acknowledgement, so relay changes, calibration and flash writes are not
repeated by outgoing-message retry handling.

Each part of a multi-message response is a separate queue item. If part 1 is
accepted and part 2 fails, only part 2 is retried. Status and utility-alert
pairs validate formatting and capacity for both parts before enqueueing either.
Incoming commands are polled only when at least four outbox slots are free,
matching the largest current response group (all-outlet details/calibration).
The command executes before background producers can use those slots. If a
future command produces more than four messages, increase
`maxCommandReplyMessages` accordingly.

When capacity is insufficient, incoming commands remain in modem/SIM storage
until space returns. Background alert monitors advance their reported state
when their event is accepted into the outbox; a failed admission leaves the
transition pending while its condition remains present. Already queued events
remain queued even if the load later changes, preserving their event order.
Alert monitors continue observing while unregistered once the main loop is
running, and queue detected events for later submission. Long blocking modem
operations can still delay sampling. Modem/SIM storage capacity limits apply. An undeliverable message
can delay later messages to the same recipient while its submissions fail.

The outbox owns all message bytes: stack formatting buffers and modem receive
buffers can be reused safely. It uses no dynamic allocation or flash writes.
The queue is RAM-only and does not survive a restart. A submission timeout can
be ambiguous: an SMS may already have reached the network despite missing
confirmation, so retries of unconfirmed messages can still produce duplicates.
A modem OK is not a recipient delivery receipt.

## Load detection for any appliance

The load monitor no longer silently absorbs an initially connected load as a
startup baseline. A load above the configured OnThreshold must pass the same
five-reading average and three consecutive confirmation checks whether it is
present at boot with its relay already enabled, connected during initial
warm-up, or connected later on an already-enabled relay. It then queues
Device power restored for that outlet. An initially empty outlet does not
produce a lost alert. OffThreshold hysteresis still applies after connection.

Intentional relay OFF/ON and outlet reboot commands suppress load alerts caused
by those actions. A successful actual GPIO output change increments a per-outlet
sequence counter. The monitor observes that counter, resets its sample window,
and establishes the post-command state quietly from five fresh readings. This
also covers reboot commands whose OFF/ON cycle finishes between monitor polls.
An ON request for an already-ON relay does not increment the counter or suppress
subsequent real load events. Later load changes on the enabled relay still alert.
Utility restoration without a customer relay change can report confirmed loads.
Already queued genuine load events are not cancelled by later relay commands;
they may be delivered after the command if SMS submission was delayed. Detection
is based on measured active power and configured thresholds, not appliance type.
A physically plugged-in appliance drawing too little power to cross the on
threshold cannot be classified as a connected load by this method.

## Logs

```text
SMS queued: id=12 bytes=15 pending=2
SMS submit attempt: id=12 pending=2
SMS submit failed: id=12 retained; retry after 60 seconds
SMS submit attempt: id=12 pending=2
SMS accepted by modem: id=12
```

The same ID remains pending across failed attempts. Logs distinguish queue
acceptance from modem submission. Command polling pause/resume logs identify
backpressure. Load-monitor logs identify confirmed events and measured power;
they no longer describe an initial connected state being silently accepted.

## Verification scenarios

With RTT connected, verify these on hardware:

- Cause one submission failure, then send different commands: DeviceOn/Off,
  getStatus, getcellularinfo, threshold/heartbeat settings, GetOutletData and
  calibration. Their replies stay queued during cooldown and submit after
  recovery. Retry logs must not repeat command execution or settings writes.
- Let main status submit but fail the outlet report. Only the failed item ID
  should retry; the accepted main-status ID must not submit again.
- Lose registration with messages pending, then restore it. Submission
  resumes, maintaining each recipient's message order without issuing the commands again.
- Fill the outbox until fewer than four slots remain. A new command stays on
  SIM, then executes after space returns. Its entire response must fit.
- Boot with a load connected and the relay already enabled, connect one during
  initial warm-up, and connect one during idle operation. Each case should queue
  a restored alert after confirmation.
- Begin empty: no startup lost alert. Remove a confirmed load: one lost event;
  reconnect it on the enabled relay: one confirmed restored event.
- Command DeviceOff, DeviceOn and outlet reboot with a load attached: no new
  load lost/restored event caused by those relay actions. Wait for quiet settling,
  then disconnect/reconnect the load: normal confirmed alerts must return.
- Repeat DeviceOn on an already-ON relay while connecting a load. Monitoring
  must not reset or suppress that genuine connection event.
- Restart with pending messages to verify the documented RAM-only limit.

No project build, simulated modem test or hardware validation was run. Source,
API wiring, response-size bounds and documentation were checked statically.

## Optional OTA progress notices

OTA stage and periodic percentage messages use this same outbox. Only one optional
progress notice stays queued per update, with five free slots reserved after
insertion for other replies/results. An unsent older stage may be cancelled in
favor of the latest stage. Terminal verification/failure removes pending optional
progress before queuing the essential result; its specific SMS ID still controls
the install reboot. This cancellation applies only to optional OTA notices.
Already accepted messages cannot be recalled. Command responses and genuine alert
events retain their normal retries. See [HTTP OTA](HTTP_OTA.md) for intervals.
