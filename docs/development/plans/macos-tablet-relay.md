# Mac-hosted Tablet Relay

Date: 2026-10-06. Status: first network Relay pilot implemented on `codex/macos-tablet-relay`; build 16 passed targeted Mac-to-AVP drawing performance and USB/TCP lifecycle acceptance on Flame4.

## Current acceptance

The development Mac's USB Wacom is registered through Setup and authorized for
PLANK. After opening signed Mac build 16, restoring sharing and repeating the
direct-IP drawing comparison, the user reported: “Perfect. Works super smooth
now.” Accept that targeted live drawing/performance pass. Build 16 runtime is
`a0c126c`; build 15 remains the signed rollback. Headset approvals and Relay
identities were unchanged when build 16 was opened.

The test used the Mac's shared Wi-Fi network after the venue LAN would not permit
the tested AVP-to-Mac path. This does not qualify other network paths or wireless
Wacom capture. Mac Relay background operation passed, and the user reported two
successful same-bookmark desktop reconnects on build 16. USB disappearance and
return failed on Flame3: hover returned, but tip taps and dragging required a
desktop reconnect. The initial full-recovery report is superseded by that
correction. Read-only inspection after the reconnect found Host 1.1.024 and
Pressure Recalibration = 1 on the PLANK UHID stylus. This is a suspected
workstation initialization confound, not proof of the failure cause. Flame4
has the previously accepted Host 1.1.030 pressure policy. Its active virtual
stylus was verified at Pressure Recalibration = 0 before USB hotplug and again
after reattachment (generation 12 to 13). USB return restored all input with
hover first, and a second test with tip contact during USB return also passed
without desktop reconnect. These passes strengthen the policy explanation but
do not establish the cause of the Flame3 failure. No Host configuration or
package was changed during this investigation. Mac sleep/wake then passed,
including resumed sharing and full pen input. After ending the AVP desktop and
disabling sharing, local Mac Client USB capture on Flame4 also passed hover,
taps and dragging without restarting the Mac app. These are targeted physical
passes on this Mac and Host 1.1.030; the Flame3 failure remains an explicit
compatibility limitation. Earlier Linux Relay acceptance was not substituted
for these Mac platform checks.

## Product journey

An artist plugs a supported Wacom into their Mac, enables **Share tablet with
PLANK**, registers that Relay on the AVP, and draws in the AVP's workstation
session. The Mac does not need its own workstation connection or credentials.
The Mac application shows the shared tablet, approved headset, connection state
and a clear Stop Sharing action.

First path: USB Wacom → Mac Relay → authenticated TCP over the local network →
AVP Client → existing raw-HID Host channel. Internet access is not required for
the Relay link. This does not alter the AVP's workstation connection.

Keep the AVP boundary: Setup registers a Relay; the Client selects a registered
Relay. Do not restore manual credentials or tablet-specific ExpressKey approval
in the Client. Sharing is off by default and requires explicit Mac consent.

## What can be reused

- Native Client `MacRawWacomInput` already accepts an injected frame sender,
  handles Host control replies and exclusively opens physical HID interfaces.
  Preserve its descriptors, report IDs, bounded I/O and callback lifetimes.
- Raw Relay revision `029721f` supplies PLTR/PLWH framing, Noise-authenticated
  links, approved-key identity, session ordering and enrollment semantics.
  Reuse these contracts and portable components rather than inventing another
  wire format or mouse/pressure approximation.
- Existing AVP network drawing is the receiving path. A Mac Relay is a new
  physical device owner, not a new Host input implementation.

This is not a direct build of the Linux service. At the inspected revision,
the worker bridge and dispatcher name `LinuxRawWacomInput`; capture leases use
Linux abstract sockets, TCP wakeups use `eventfd`, and enrollment relies on a
Linux local privileged endpoint. CMake places service and crypto/link targets
under its Linux guard. The Mac adapter and build boundary must be explicit.

## Bounded implementation order

1. **Separate portable link/session logic from platform service plumbing.**
   Keep Linux behavior and tests intact. Add a macOS transport adapter with
   bounded reads/writes, whole-operation deadlines, cancellation and normal
   Bonjour discovery. Bind a network listener only while sharing is enabled;
   unauthenticated peers cannot start capture or send HID controls.
2. **Adapt the existing Mac worker to the Relay bridge.** One network owner
   encrypts/decrypts; capture callbacks only enqueue bounded raw records. Host
   GET/SET/OUTPUT replies reach the actual tablet unchanged. Queue exhaustion
   must close/recover explicitly rather than silently lose tip/button events.
   Prove attachment generations remain fresh across sessions and process
   restarts; the current Mac worker's process-local generation counter is not
   sufficient evidence for persistent Relay identity.
3. **Coordinate ownership with the Mac Client.** Introduce one shared capture
   authority for direct Client use and Relay hosting. Acquire before opening
   interfaces and release only after the worker has released them. Refuse
   conflicting use with a visible busy state; never steal an active session.
   Test coexistence with the current desktop Client too, preserving exclusive
   IOHID refusal even when that older app does not participate in the new lease.
4. **Complete Mac approval and AVP Setup registration.** Authenticate discovery
   identity before pinning. Adapt Setup-mediated enrollment to a Mac consent
   authority; an untrusted Bonjour advertisement cannot approve a headset.
   Approval must prove possession of the AVP Client key and durable acceptance,
   with cancellation, expiry and replay protection. Persist identity/approvals
   in protected Mac storage using existing contract semantics.
5. **Integrate optional hosting into the native Mac app.** Hosting starts only
   with consent and an authenticated AVP session ready for tablet input. Unlike
   direct desktop capture, active hosting is not tied to Mac window focus.
   Quitting, Stop Sharing, remote disconnect and device disappearance release
   capture. Ordinary Mac input must return after release. No always-running
   privileged daemon or login item is needed for this first slice.

## First live acceptance

Use the accepted Host and AVP Client where compatible; inventory any required
Setup/Client capability change before building. Keep the existing network and
Host installation unchanged. No Host package is part of this slice.

- Register the Mac through Setup and select it in the AVP Client. Verify the
  approved identity, permission-denied flow and refusal of an unapproved peer.
- Prove hover, pressure, tip taps, held drags, side buttons and supported pad
  controls through the raw-HID path, including bidirectional feature traffic.
- Switch focus away from the Mac app while drawing on AVP; hosting continues.
  Attempt direct Mac Client capture of that same tablet; it reports busy.
- End the AVP session while the tip/button is held, then reconnect tablet-first.
  No stale held state, old generation or duplicate Host device may survive.
- Unplug/replug USB, quit/relaunch the Mac app, and sleep/wake the Mac. Retest
  approval retention, automatic recovery and return of local tablet ownership.
- Compare sustained drawing with the accepted Relay path, recording terminal
  failures and bounded queue behavior without adding per-report UI updates.

Release packaging must retain the normal Input Monitoring permission flow and
clear network/hosting consent. A successful compile is not physical acceptance.

## Later transport/device work

Wireless Wacom → Mac requires a descriptor/control/ownership probe; the current
Mac worker identifies itself as USB and must not simply relabel Bluetooth input.
Mac → AVP Bluetooth requires proof of macOS peripheral/listener APIs, negotiated
L2CAP behavior and raw-HID throughput, then the versioned handoff contract.
Linux BlueZ behavior cannot establish either Mac capability. Keep both separate
from the network-first pilot and do not advertise them before qualification.

## Candidate implementation and acceptance boundary

Mac build 11 starts from the accepted build-9 runtime, with this optional Relay
slice. Experimental native 10-bit capture from build 10 stays on its separate
branch. The first Relay is USB-only and uses authenticated TCP on 28990, with
Setup discovery/management on 28991. Both listeners exist only while sharing.
Their stable ports allow saved routes to reconnect after quit/relaunch or wake.

The existing raw codec is pinned at `029721f`; the managed Setup codec at
`73a3743` has a separate symbol namespace and identity. Its public bootstrap
only returns status and a public key. The first authenticated Setup connection
requires **Approve Relay Setup** on the Mac. That durable Setup approval can
then issue a 120-second, one-use PLEN grant; the AVP Client proves possession
of its own key before durable drawing approval. Cancel, expiry, wrong keys and
replays refuse enrollment. No private keys pass through links or Bonjour.

Mac Settings includes **Share tablet with PLANK**, off at every app launch.
Hosting and a local Mac workstation session cannot be started together in the
pilot. A file lease held by the HID worker also coordinates cooperating processes;
exclusive IOHID open remains protection against older clients. A stalled worker
retains its lease and store until interfaces close. Raw reports are never
coalesced. A full 256-record/256-KiB inbox closes the drawing link explicitly.

Mac sleep releases active capture and listeners. Wake restarts sharing only if
it was enabled before sleep; quit always ends sharing. USB disappearance uses
the existing worker's retry path. OS Input Monitoring permission is requested
only from the local Share action, never by an unauthenticated connection.

This pilot registers through current Relay Setup; it does not implement Linux
network administration, Bluetooth tablet pairing or Setup's decoded drawing
preview. Those operations return a clear unsupported message. Register the Mac
and test raw drawing in PLANK. Mac wireless tablets and Mac-to-AVP Bluetooth
remain later slices.

New suites pass: 1,791 raw/enrollment/lease checks, 55 Setup codec checks, and real socket discovery/validation checks. The existing Mac suites also pass. Deliberately bypassing approval or ignoring cancellation fails the new checks.

Focused checks cover real Noise/PLTR/PLEN framing and approval persistence,
wrong/unapproved keys, cancel after claim, deadlines, replay, byte preservation,
bounded backlog, durable generations, capture exclusion in another process,
late callbacks, fragmented Network discovery and duplicate JSON refusal.
The actual HID worker is faked in the protocol tests. A signed app build and
passing tests do not establish physical Wacom forwarding, focus independence,
sleep/wake or reconnect quality.

Run `scripts/test-macos-relay.sh <output-dir>` with the pinned raw/managed
source and Sodium prefixes, plus the existing Mac regression checks. Build with
`scripts/build-macos-native.sh`; the new managed source input is mandatory.
First live pass: local sharing, Setup approval/registration, AVP selection,
then hover, pressure, held drag and buttons. Leave the Mac app in the background
while drawing, then Stop Sharing and confirm ordinary local tablet use returns.
Only after that pass proceed to reconnect, unplug/replug and sleep/wake.

### Build 12 — Setup channel interoperability correction

Build 11 reversed the established TCP channel mapping, preventing current Setup
from completing its discovery probe. Build 12 uses channel 1 for read-only
status and channel 0 for authenticated management, matching RelayPairingClient
and the Linux network service. The corrected socket check uses those existing
channel numbers. A separate process using the actual Setup client now completes
both discovery and local approval against the Mac server fixture. Physical Mac
Relay drawing acceptance remains pending. Peer-to-peer Wi-Fi was not enabled as
part of this fix; network reachability is a separate live gate.

### Build 13 — reopen existing identity storage

After the first launch, FileManager's nonrecursive create-directory call refused
the existing identity folders (Cocoa error 516). Build 13 allows those existing
folders to be reopened; the native stores still enforce owner, private mode,
no-follow paths and exclusive locks. The socket suite now opens, closes and
reopens the Swift native adapter and requires both public identities to remain
unchanged. Build 13 includes build 12's channel fix; live acceptance is pending.

### Build 14 — Passive USB presence in Setup

Build 13 could register on the Mac's shared Wi-Fi network, but its placeholder
`usbTablets: []` made Setup show “No tablet connected” even with an Intuos Pro M
on USB. Build 14 reports a bounded USB inventory to authenticated Setup only.
It reads IORegistry properties without opening HID interfaces, taking the
capture lease, registering callbacks or requesting permission. Physical USB
parents collapse the tablet's multiple HID interfaces, ordered like the raw
worker. The first tablet is the selected USB candidate; presence does not prove
that capture or raw forwarding has succeeded.

Setup decoded preview remains unsupported: `attached` and `captureActive` stay
false in this management status. Use in PLANK remains the drawing acceptance
path. Focused checks cover authenticated presence, no unauthenticated inventory,
multiple interfaces, empty inventory and bounded names/list size. A passive live
probe detects one Wacom Intuos Pro M; end-to-end raw drawing is still pending.

### Build 15 — first-use approval state

Build 14's inventory was shown only after local approval. Before approval,
current Setup rendered its `idle` reply as “No tablet connected” and hid the
reply's instructions. Its diagnostic actions were disabled while management
was in progress. Build 15 reports `verifying` for an encrypted first-use session
awaiting Mac approval, which current Setup renders with the approval instruction.
It includes bounded USB candidates with no serial numbers and no active
selection. Public discovery still has no USB inventory. Management, capture,
headset authorization and drawing handoff remain unavailable until local approval.

The Mac clears the approval prompt when that management connection closes and
directs the user to restart setup. Approval success is reported only after the
native store accepts it. Setup cancels its operation when inactive; keep its
screen open while approving on the Mac. This slice does not change that lifecycle.

The real Setup client fixture received three pending replies, verified its
waiting state and absent drawing authorization, then completed approval through
the existing Noise management channel. Raw, Setup codec and socket checks pass.
Build 15 is signed and staged; physical first-use approval and drawing acceptance
remain pending.

### Build 16 — remove per-report output pacing

The first live Mac Relay drawing trial was laggy. Connecting the headset to the
workstation by direct IP improved it, with substantial lag still reported. The
Mac Relay's output loop also limited queued output to one frame per 5ms timer
tick, leaving approximately 200 frames/second for raw input plus control traffic.
This is a source and fixture finding; the active transport/relay selection for
the live lag report has not yet been independently captured.

The drawing socket now continues queued output after each write completion.
It arms a receive before draining again and handles buffered incoming messages
first, preserving bidirectional HID control and heartbeat progress. There is
still only one outstanding write, the native inbox remains bounded, and every
raw report is forwarded in order. The timer still checks deadlines and discovers
newly queued work; management and registration pacing are unchanged.

A real socket test uses production Noise framing and an approved test identity,
with the physical HID worker faked. All 128 reports arrive byte-for-byte and in
order, alongside a returned heartbeat. The previous loop took 642.5ms and failed
the 300ms latency bound; the new loop took 6.3ms and passed. The 1,791 raw checks,
55 Setup checks and existing socket checks also pass. Signed build 16 is staged,
not installed. Live performance acceptance remains pending.
