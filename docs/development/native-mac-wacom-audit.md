# Native Mac Wacom ownership and recovery audit

Source review: October 7, 2026. Native sharing runtime `16ef937`, display runtime
`fd844b5`, based on maintained Client integration `1953cc1`. Component evidence
and import scope are in [the slice map](native-mac-integration.md).

October 8 update: foundation review fixes are carried through this stack.
Session opt-out now retires local USB admission/capture, preserving release
submission while its input sender is alive. The native wrapper now carries
filtered physical activity separately from raw input, revoking both callbacks
on destruction. See the slice map's review-correction evidence. This does not
resolve the separate unexpected-failure ordering/Host-cleanup finding below or
change the registered-Relay presentation path.

## Ownership boundaries

| Boundary | Source behavior | Evidence / remaining qualification |
| --- | --- | --- |
| Physical tablet | `MacRawWacomInput` opens USB interfaces exclusively. Sharing adds a worker-owned same-user file lease before discovery, released after interfaces close. A stalled worker retains ownership. | 1,791 protocol/lease checks include cross-process refusal with a fake worker; real competing capture and local return need changed-worker acceptance. |
| Permission | Maintained `requestPermissionIfNeeded` checks access and supported USB presence. Native direct creation requests from its MainActor constructor; remote Relay capture passes `false` and uses explicit local sharing consent. Shipping launcher behavior is retained. | Production native worker compiles; denied/granted permission and shipping application CI remain gates. |
| Worker-to-Swift | Production C wrapper owns shared callback state; destruction waits for deactivation then revokes its Swift context before destroying capture. Late worker callbacks cannot use the revoked context. The Swift callback thunk is nonisolated. | 266 actual wrapper/Swift foreign-thread checks plus lifetime/barrier tests passed with fake HID. |
| Host availability | Native capture activation waits for required Host raw-HID features and scene ownership. Direct and registered-Relay sources are selected per session. | Pure policy checks and historical targeted hardware passes; intermittent physical startup is not resolved by a compile. |
| Multi-window focus | Scene ownership is a union of desktop windows. Fullscreen transition protection preserves an already active owner; local settings retain normal UI input ownership. | Focus/presentation tests and accepted native Mac build 24 targeted passes; real OS Space transitions remain platform gates. |
| Sharing focus | Sharing is opt-in and independent of whether the Mac app is foreground. Authenticated management/drawing peers have separate bounded ownership and generation-scoped callbacks. | Existing Finder-front and reconnect hardware passes are carried; no new hardware test performed for this import. |
| Relay generations | Native identity store persists attachment generations; exhaustion/refusal does not silently reuse an old generation. Worker creation keeps the store alive. | Protocol/generation/replay tests; reconnect must also verify Host device state. |
| USB loss/return | Worker releases missing interfaces, retries discovery and advertises a new attachment. Protocol forwards raw descriptors/reports and Host feature/control traffic. | Historical Flame4 full return passed. Earlier Flame3 hover-only return on old pressure policy remains a recorded confound, not a proven root cause. |
| Stop/sleep/quit | Sharing cancels listener and peer ownership; sleep releases capture, wake restarts only prior opt-in sharing. Local session disconnect closes capture before cancelling its stream. | Historical targeted lifecycle passes plus component cancellation tests; a new integration cannot inherit every OS/package claim. |

## Teardown finding requiring shared-engine coordination

Explicit local disconnect in `PlankCoreClient.disconnectSession` awaits
`nativeWacom.close()` before cancelling the stream and stopping its input queue.
`PlankMacWacomSession` records accepted SUSPEND/DETACH submission and waits up to
one second for the input sender's successful transport submission. The underlying
worker has its own release/exit deadlines and can retain physical ownership if
driver shutdown stalls. These are separate barriers.

Unexpected stream failure and task cancellation take a different path:

1. `PlankSessionEngine` stops `inputQueue` and waits for its sender, or its
   cancellation handler stops the queue immediately.
2. The worker scope's deferred `beforeTransportClose` hook then closes native
   physical capture, before destroying the transport endpoint.
3. At that point `offerNativeRawHid` refuses a new release record because the
   input queue is stopped. Physical capture still closes; a new release cannot
   be guaranteed through that sender. An input-lane failure may already make
   transmission impossible regardless of ordering.

This is a source-confirmed difference in local release submission, **not proof
of a stale Host button or the historical hover-only failure cause**. Even a
successful submission is not a Host acknowledgment that button/tool state was
cleared. The one-second barrier must not be described as end-to-end release.
Transport close and Host endpoint destruction/reset remain part of recovery.

Upstream owns the shared input/transport reconciliation through
[issue 9](https://github.com/instinctual/plank-client/issues/9). Review the current
Host endpoint cleanup alongside these paths before changing shared teardown.
Required proof is a deterministic ordering test for normal/error/cancellation
paths plus a targeted held-tip/button failure-and-reconnect test showing one
fresh Host device and balanced pen state. Keep callback revocation, sender and
control-thread joining, and endpoint lifetime ordering intact. Do not add a
second sender racing normal input or wait forever on an unavailable lane.

## Bounds and limitations

- Raw input admission is bounded to 256 queued records and refuses overflow.
  The Host-control inbox is bounded to 128 and closes capture on exhaustion.
  Relay raw inboxes are bounded to 256 records / 256 KiB and close the link on
  exhaustion. Raw pressure/button reports are not silently coalesced.
- This does not establish a globally bounded ordinary input queue or resolve
  the observed Host input receive-queue overflow. That Host experiment is held.
- The cross-process lease coordinates same-user cooperating applications;
  exclusive IOHID opens remain the gate for older/nonparticipating applications.
- The Mac bridge is USB-only and TCP-only. Linux Bluetooth drawing acceptance
  cannot establish Mac wireless capture, peripheral L2CAP or raw throughput.
- Pure tests do not measure physical pressure, latency, OS permission prompts,
  USB driver shutdown, UI Space animation or installed release compatibility.
- The changed shared worker needs maintained Qt/SDL build/CI verification as
  well as native compilation. No shipping package is promoted by this audit.

The immediate acceptance order is in the slice map. Carry accepted unchanged
flows, verify changed ownership/permission/teardown and dependency inputs first,
then establish the same-package macOS15/27 matrix. Avoid using repeated desktop
reconnect as a substitute for proving held-state cleanup.
