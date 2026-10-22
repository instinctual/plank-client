# Native macOS Client hit list

Date: 2026-10-06. Scope: the Apple silicon macOS 15+ native pilot.

## Accepted checkpoint

Runtime commit `60f7795`, signed local build 9, follows Vision publication
`5f2d28a`. The operator accepted mouse presentation over the desktop and local
controls, visible slider handles, and fullscreen entry/exit. Direct USB Wacom
hover and pen operation were also observed working earlier in the pilot.
These are targeted observations, not complete Mac release qualification.

Keep this checkpoint available for comparison. The pilot has a separate app
identity and bookmark store. Publication of its source does not replace the
existing desktop Client or publish a notarized application.

## Priority and delivery order

| Priority | Slice | First deliverable | Main boundary |
| --- | --- | --- | --- |
| P1 | Host video quality controls | Capability inventory, then an exact-profile selector | Only offer formats the Host and native Client both support |
| P1b | Mac-hosted Tablet Relay | USB Wacom on the Mac serving one registered AVP over LAN | One physical owner; existing authenticated raw-HID drawing contract |
| P2 | Multiple screens | Two Host output rectangles in two native windows | One session, shared geometry and exclusive tablet ownership |
| P3 | Webcam | End-to-end feasibility report and smallest supported source/sink pair | Local capture alone does not create a camera in remote applications |

Focused feature work may begin after publishing the checkpoint. Release
qualification proceeds alongside it; remaining tests must not expand into a
separate harness project.

The [Mac Tablet Relay plan](macos-tablet-relay.md) adds an artist-facing hosting
mode to the Mac application. Start with USB capture and network drawing; direct
Bluetooth from Mac to AVP and wireless tablet capture need separate feasibility
and acceptance. Prioritize this after the bounded quality-controls slice and
before webcam implementation.

## P1: Host video quality controls

The pilot already has live bitrate, volume/mute and statistics in its desktop
controls. Its shared launch builder currently requests HEVC 10-bit 4:4:4
identity (`0x0800`). Resolution and frame rate are bookmark settings with a
session-closure policy. Other profiles are not implemented by adding a menu.

1. Inventory Host-advertised capture/encoder/profile capabilities and compare
   them with the current desktop Client's profile negotiation.
2. Qualify each proposed profile through decoding and Metal import: codec,
   bit depth, chroma, range and color transform must all match. Decoder
   selection stays internal. Exact-format software fallback needs its own
   support and performance evidence; never substitute a different profile.
3. Offer only proven profiles, with explicit unsupported states. Preserve
   existing bookmarks and the current HEVC path.
4. Keep bitrate hot-adjustable only when the Host advertises that capability.
   Show accepted/capped targets using the existing acknowledgement path.
   Profile, resolution and timing changes require the existing explicit
   close/reconnect flow until a negotiated runtime mechanism exists.

Acceptance: verify the selected tuple reaches launch, Host acknowledgement
matches, color/chroma remain correct, and a 20/100 Mbps high-motion comparison
changes measured rate meaningfully. Changing local controls must preserve
Wacom capture, button state, cursor ownership and fullscreen behavior.

## P2: Multiple screens

Start with two remote outputs presented on two Mac displays, with a windowed
mode and native fullscreen per window. Use the existing output-topology
contract: `separate-displays` presents source rectangles from one composite
stream and one decoder. Independent per-output streams are future work.

The shared pilot topology model currently retains only a single layout and
desktop dimensions. Extend it to retain authenticated output IDs, source
rectangles, primary output and generation. Presentation and absolute mouse
mapping must consume the same snapshot. Reuse the existing desktop Client's
geometry and fullscreen lifecycle lessons.

- Choose remote-output/local-screen mapping explicitly; do not infer a second
  Host output solely from a second connected Mac display.
- Preserve aspect, mixed Retina scale, negative origins, primary identity and
  accurate pointer mapping at seams and edges.
- Keep one session-owned raw-HID capture worker. Wacom reports remain
  byte-for-byte data; Host tablet mapping uses the desktop topology. Focus
  moving between the session's windows must not detach the tablet.
- Handle screen removal, topology replacement and disconnect deterministically.
  Secondary windows must leave fullscreen before disposal, with no orphan Space.

Acceptance: single/dual/single transitions, mixed-scale displays, repeated
fullscreen exit/reentry, screen removal and reconnect. Verify pen pressure,
tip/button release, held drag across seams, mouse coordinates and teardown.
Do not change Host topology policy to compensate for a Client mapping defect.

### P2 implementation checkpoint

Local Mac build 17 adds a compatible second-resolution bookmark field, bounded
v13 output parsing, two surfaces consuming one decoded frame, shared output
crop/input geometry, session-wide Wacom focus ownership, explicit Mac screen
placement and fullscreen-aware secondary disposal. This follows the accepted
Mac Relay build 16. The separate experimental quality branch is not folded
into this comparison. Focused checks and Mac/Vision compilation are evidence
of source correctness; live two-output, mixed-scale, fullscreen/screen-removal
and Wacom acceptance remain pending.

The operator observed two remote windows in build 17 and requested that their
assignment follow Mac primary/secondary priority. Build 18 explicitly places
both windows by that priority, uses the Host primary flag for window roles,
and retains source rectangles for rendering and input even when the primary
Host output is on the right. Focus does not change priority. A Mac display
configuration change reassigns windowed surfaces; fullscreen placement waits
until exit. Live placement/fullscreen and Wacom acceptance remain pending.

The operator clarified that the Host virtual desktop must follow the Mac's
spatial arrangement and primary/connector creation order, as the earlier
desktop Client did. Build 18's role-only placement was insufficient. Build 19
ports the existing negotiated `plankPrimaryOutput` behavior, retains the
connection's local display snapshot, verifies mode/primary/DP-0 binding during
layout retry, and maps Host crops to local displays in spatial order. This
uses the existing qualified resolutions and Host contract. Host builds and
network settings are unchanged. See [the geometry correction](native-macos-display-matching.md).

## P3: Webcam feasibility

At inspected root upstream commit `e92060b`, `protocol/camera.md` describes an
in-development Ubuntu Client → macOS Host lane. Native macOS Client capture
is not implemented there. The current pilot's pinned transport has no camera
integration; a Linux workstation camera sink is an additional dependency.

Produce a bounded report before implementation:

1. Select the destination Host OS and prove that a remote application can
   enumerate and read an injected camera through its supported device path.
2. Probe the Mac camera's actual output formats and permissions. The existing
   camera policy requires native compressed H.264 or MJPEG, without an encoder.
   If available output is raw-only, propose a reviewed encoding-policy change
   rather than treating it as already supported.
3. Reconcile native Mac metadata with PCAM's V4L2 color/sequence fields.
   Decide compatibility and feature/version negotiation from evidence.
4. Specify explicit camera-off default, device selection, indicator, bounded
   queues, recovery and immediate release on disable/disconnect/background.
   Optional camera failure must leave desktop video, audio and tablet alive.

Acceptance must span physical capture → transport → Host device → application,
including loss/reconnect, stale-frame rejection and permissions. Microphone and
Vision Pro Persona are separate slices. Host changes require a separate plan;
this backlog does not authorize a Host package or installation.

## Wacom and release gates

Every feature preserves descriptors/report IDs, bidirectional GET/SET/OUTPUT,
exclusive capture, bounded callbacks, generation checks and release barriers.
Do not coalesce raw reports, synthesize tip state or use mouse clicks to repair
pen acceptance. First-generation normalized-only devices remain unsupported.

Before a Mac release, qualify held-tip disconnect/reconnect, focus changes,
USB removal/return, sleep/wake, resolution/timing changes and actual macOS 15
runtime behavior. Track periodic video stutters separately and retain terminal
queue failures; passing UI checks does not settle them. Signed staging is not
notarization or release packaging.

## Source anchors

- Pilot: [README](../../../apple-native/README.md),
  [session controls](../../../apple-native/Sources/PlankMacDesktop.swift).
- Shared [launch request](../../../visionos-native/Sources/Models/PlankStreamRequest.swift)
  and [topology/bookmark model](../../../visionos-native/Sources/Models/HostBookmark.swift).
- Root protocol contracts: `protocol/encoding-profiles.md`,
  `protocol/dynamic-bitrate.md`, `protocol/output-topology.md`; camera contract
  is on root upstream at the inspected revision above. Re-check exact candidate
  versions before implementation; newer contracts are not automatically in the
  pilot's pinned dependencies.

Mac-hosted USB Tablet Relay build 16 passed targeted drawing and lifecycle acceptance on Flame4 Host 1.1.030, including background use, reconnect, USB return, sleep/wake and return to local Mac capture. Flame3 Host 1.1.024 had a hover-only USB-return failure; see [the Relay acceptance record](macos-tablet-relay.md) for that limitation and unqualified transports. Multiple screens is the next development slice.

### October 7 session-window controls

The operator accepted build 19's spatial placement and primary negotiation,
using the existing Host contract. Wacom startup required three connections;
that remains unresolved and does not block recording the narrower display pass.

Before further features, build 20 makes Disconnect and the toolbar fullscreen
command act on the whole session from either desktop. The green macOS button
remains per-window. Group Space transitions are serialized; disconnect waits
for both exit acknowledgements before disposing the desktops. A bounded focus
hold preserves existing Wacom ownership through group animations only while
PLANK is active. Physical acceptance is pending; capture console output on the
next authorized launch to retain startup/attachment diagnostics.

Build 20 targeted feedback: pen works well; green button works windowed but
not on the main window in fullscreen, and wheel input is intermittent. Build 21
is a narrow fullscreen-control revalidation and pointer-targeted scroll candidate.
It retains fractional precise scroll movement and the existing legacy wheel
scale. Independent green-button and both-output wheel behavior need live
acceptance; prior Wacom startup failures remain separately unresolved.

### October 7 build 22 transition candidate

Build 21 feedback accepts the window buttons provisionally; wheel testing has not
been performed. Wacom hovered without tip clicks on the first recorded connection,
although Host 1.1.024 acknowledged attachment and the Client sender had no sampled
backlog. Reconnect included capture releases and reattachments; this does not
establish their cause or prove the earlier Host pressure-policy defect recurred.
The operator additionally reports duplicate parked/moving cursors after fullscreen
exit, quicker tablet acquisition fullscreen, and Show second display entering both
windows into fullscreen.

Build 22 addresses concrete Client focus/presentation paths: non-key canvas mouse
hover, parked native cursor suppression while the pen is on either output, individual
green-button transition focus grace, and temporary Space surface reparenting. Reopen
uses an independent button, defers until popover dismissal and activates an existing
secondary instead of reopening it; it never requests fullscreen. Physical acceptance
is still required. No Host, Relay, transport, network or raw HID worker changes.

### October 7 wheel follow-up

Build 22 is reported as working better for the transition changes. Wheel input
is not accepted. Build 23 corrects native event conversion against the established
Cocoa/SDL Mac path and routes eligible wheel events once from the local monitor.
It retains display activation in Session Controls even while the second window
exists. Physical wheel type and smoothness remain to be verified; no Host/Relay,
raw HID worker or shared Vision source changes are included.

Build 23 subsequently delivered physical wheel detents to the Linux virtual
mouse while gedit still barely scrolled. Build 24 matches the established
desktop Client by sending wheel input without an extra absolute pointer event
on every tick. Linux uses different devices for those operations; GTK's device
change handling resets its smooth-scroll baseline. The mechanism is supported
by source comparison but not yet confirmed by a correlated live trace. Existing
mouse movement still targets either desktop; controls and inactive-app input
stay local. Physical scrolling on both outputs and through fullscreen changes
requires acceptance. No further Host or tablet changes are included.

### October 7 accepted wheel checkpoint

Signed Mac build 24 (runtime `5db90ff`) passed targeted operator acceptance for
wheel scrolling in both desktop windows through fullscreen/windowed changes.
Live Host observation also confirms balanced middle-button press/release and
released final state. Application-specific middle-button gestures and a live
correlation of GTK device switching are not claimed. Preserve this checkpoint
alongside the earlier accepted placement and session-wide controls.

Proceed with P3 webcam feasibility before implementation. This does not close
the remaining Wacom startup/interruption or general Mac release gates, and
does not authorize a new Host package or installation.
