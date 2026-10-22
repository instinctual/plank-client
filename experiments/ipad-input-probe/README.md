# Standalone iPad input test

This separate app exercises local UIKit input before integration into the
native Client. It has no transport, Relay, USB driver, workstation connection
or dependencies on the shared session engine. Bundle identity:
`com.instinctual.plank.inputprobe`, version `0.1.0 (1)`.

The development target is iPadOS 18+, arm64, using SDK 27. This is a probe
minimum, not a committed PLANK product minimum. It has no camera, microphone,
Bluetooth, local-network or USB-driver entitlement requests.

## What it records

- Pencil actual/coalesced contact samples and force/altitude/azimuth API values.
- Pencil hover where supported, and ordinary pointer hover/contact separately.
- Physical wheel/trackpad scrolling through a touch-excluding scroll recognizer;
  the teal marker moves vertically to make scrolling visible.
- Keyboard down/up/cancel counts, without recording key text or identifiers in
  the exported report.
- Active contact count, terminal transitions, duplicate/rejected samples,
  estimated-property update count and lifecycle cancellation.

Pencil blue strokes, pointer orange strokes and finger gray strokes exist only
in memory. Rendering is bounded to 32 strokes and 1,024 points per stroke;
the retained picture may truncate during long use. Sample counters continue.
Strokes use a fixed width; inspect the force readings rather than interpreting
line thickness as measured pressure. Each stroke renders as one batched path.
The diagnostics label refreshes at 8 Hz rather than on each sample.
**Share counts** exports aggregate JSON through the system share sheet; it
contains no drawing coordinates, images, entered text or device serial number.
Nothing is saved or uploaded automatically.

Force API readings alone do not prove hardware pressure support. Enter the
actual iPad/Pencil combination in the acceptance notes. No predicted samples
are used. Late estimated updates are counted, not replayed as new contacts.
Independent barrel roll, squeeze/double-tap gestures, remote pen delivery and
raw Wacom acquisition are outside this first probe.

An external Wacom acting as a system pointer may produce orange input. That
does not qualify pressure, tool IDs, buttons or bidirectional raw-HID capture.
Those follow the separate [readiness plan](../../docs/development/native-ios-ipados-readiness.md).

## Build and accounting checks

From this directory, use new build folders outside the checkout:

```sh
python3 build.py --platform device --work /tmp/plank-ipad-probe-device-1
python3 build.py --platform simulator --work /tmp/plank-ipad-probe-simulator-1
xcrun swiftc -Onone ContactLedger.swift ContactLedgerChecks.swift \
  -o /tmp/plank-contact-ledger-checks
/tmp/plank-contact-ledger-checks
```

The device and simulator receipts record source-file hashes, toolchain,
deployment target, app identity and executable hash. Builds are unsigned and
do not install or launch anything. Physical installation needs a separate
development signing/provisioning step and an enabled Developer Mode on the
iPad. Do not reuse the Vision or production Client bundle identity.

## First device pass

1. Record iPad model, iPadOS version, Pencil model and connected pointer model.
2. Reset. Tap and lift the Pencil several times, then draw a continuous held
   stroke. Down/up counts should balance after lifting; Held should be 0.
3. Draw with light/heavy contact and tilt in both landscape and portrait.
   Confirm readings against the hardware's documented capabilities. Hover is
   optional by model; lack of hover is not automatically a failure.
4. Hold contact and rotate, resize the app window, or background it. Contact
   must cancel and Held return to 0. Returning to the app must permit a fresh
   stroke without reconnecting anything or resuming the old contact.
5. Hover and click/drag with the mouse, then scroll slowly in both directions.
   Check orange pointer input, wheel counts and the teal marker. Keep Pencil
   lifted while testing the pointer.
6. Press/release a few physical keys, then background with a key held. Keys
   held should clear. Share aggregate counts and describe any missing input.

`Max contact sample gap` includes deliberate pauses during held contact; it is
not end-to-end latency or a hardware polling-rate measurement. Simulator
compilation/execution cannot qualify physical Pencil, wheel or tablet input.

## Public API references

- [Pencil force/orientation](https://developer.apple.com/documentation/uikit/illustrating-the-force-altitude-and-azimuth-properties-of-touch-input)
- [Coalesced samples](https://developer.apple.com/documentation/uikit/getting-high-fidelity-input-with-coalesced-touches)
- [Pencil hover](https://developer.apple.com/documentation/uikit/adopting-hover-support-for-apple-pencil)
- [Scroll recognition](https://developer.apple.com/documentation/uikit/uipangesturerecognizer/allowedscrolltypesmask)

The local contact ledger is a diagnostic fixture, not the production sender or
proof that the Host receives a release. Remote delivery and cancellation
ordering remain explicit Client integration gates.
