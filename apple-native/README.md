# Native Apple client pilot

This is an isolated Apple silicon macOS 15+ pilot built from the native Vision
Client engine. It uses SwiftUI and AppKit, the existing VideoToolbox recovery,
the exact-colour Metal presentation, native transport, and stereo Opus audio.
The target links no Qt or SDL. It has a separate application identity and
bookmark domain; it does not import existing applications' private state.

The pilot is not a shipping replacement for the desktop Client. A signed build
and passing checks do not qualify streaming or physical tablet behavior.

The [native Mac hit list](../docs/development/plans/native-macos-hit-list.md)
records the accepted build-9 checkpoint and the next quality, multiple-screen
and webcam slices, with Wacom and release gates.

## Wacom boundary

The initial Mac UI offers **Off** or **USB Wacom on this Mac**. Source selection
applies on the next connection. Relay services and protocols remain unchanged.
The existing Relay engine is retained, but Mac enrollment and its UI are not
part of this first slice.

Direct capture compiles the existing Mac raw-HID worker with an injected native
sender. It preserves physical descriptors, report IDs, Host GET/SET/OUTPUT
replies, exclusive ownership, generation checks, bounded asynchronous device
I/O, and release barriers. The normalized transport required by first-generation
PTH-x51 tablets is not implemented by this pilot. Those devices must not be
represented as supported exact raw-HID devices.

macOS Input Monitoring permission is granted through the normal system prompt.
No privileged helper or privacy-database modification is used. Capture begins
only after Host feature negotiation and desktop focus. Session controls belong
to the desktop window. Leaving that window releases held keyboard/mouse input
and requests physical tablet release. Disconnect waits for capture release and
bounded submission of queued release messages before stopping the transport.
Late callbacks cannot use the destroyed Swift sender context.

## Desktop presentation

The tablet's remote cursor is drawn in a separate AppKit view above the video.
Positions received before the first cursor shape use an arrow at the Host
position. Mouse movement uses a native macOS cursor with the Host artwork and
hotspot at the local pointer position, avoiding delayed position echoes.
Only accepted mouse movement or tablet input changes presentation ownership;
HID feature replies do not. Input forwarding and raw tablet reports are unchanged. An explicit
Host request to hide its cursor is respected. Stale positions from a previous
resolution are not drawn into the new canvas.

The Full Screen toolbar button and Control-Command-F control all desktop
windows in the session. If any desktop is fullscreen, both return to windowed
mode; otherwise both enter native macOS fullscreen. The green macOS window
button remains independent and changes only its own window. That shortcut stays local rather than being sent to
the workstation. The green window control is enabled too. Full screen hides
the menu bar, Dock and toolbar until the pointer reaches their screen edge.
These are per-window presentation options, not changes to system preferences.

Session controls restore the normal Mac pointer and keep their keyboard/mouse
events local. Opening them releases held desktop keyboard/mouse input without
releasing the Wacom. Closing them returns desktop input routing.
Pointer hiding follows the topmost view under the mouse. Toolbar buttons and
the popover also own native arrow cursor regions, including AppKit's separate
full-screen windows. Volume and bitrate use AppKit slider controls with
explicitly painted handles and normal keyboard/accessibility input.
While the tablet owns the cursor, the native arrow is hidden only until the
next mouse movement. Mouse ownership uses no repeated hide/show cycle.
No transparent cursor image is installed. Local toolbars and the system menu
bar retain their native pointer. The green control uses a reversible fullscreen
per-window action, revalidated after transitions independently of
the window's zoom size. Exit does not depend on a separate transition latch;
resizability lost during SwiftUI content reparenting is repaired.

## Two remote displays (local candidate)

The Mac bookmark editor can request two displays with independent resolutions
and one refresh rate. The combined width is limited to the Host v13 limit of
8192 pixels. Existing bookmarks remain single-display. Changing the layout or
either resolution uses the existing close-session/sign-in warning.

One authenticated composite stream and one decoder supply two native windows.
Each window crops the Host's `source_rect`; the same retained topology snapshot
maps cursor positions and absolute mouse coordinates. Negative desktop origins
are not added to composite pixel coordinates. Raw Wacom messages remain
unchanged and one session-owned worker serves both windows. Session focus is
the union of its desktop windows; closing only the second window does not
close the connection or deactivate a focused first window.

Session Controls shows the output identity/primary role and offers **Move to
Mac display**. Leave fullscreen before moving a window to another display.
For two horizontally arranged Mac displays, the Client snapshots their logical
positions and system primary once per connection. It sends the existing
`plankPrimaryOutput` left/right hint only when the authenticated Host advertises
`0x2000000` and a virtual startup. Layout retries verify both selected modes,
the primary side and `DP-0` placement, preserving the earlier desktop Client's
Flame connector-order behavior. No new Host contract is introduced.

The windows associate Host outputs with Mac displays in spatial left/right
order, retaining their original composite crops and input offsets. Window
lifetime roles follow the accepted Host primary flag. Changing focus does not
change that assignment. Selected resolutions remain the bookmark resolutions;
they are not replaced with Retina logical sizes or backing dimensions.
Unmapped arrangements and old/physical-startup Hosts receive no primary hint.
Display removal keeps windows reachable; fullscreen placement waits for exit.
With one remaining Mac display, both windows stay available on that display.
The first window can reopen the second. Disconnect from either toolbar stops
the shared session once and closes both desktop windows. AppKit fullscreen
transitions are serialized; disposal waits for both exit acknowledgements.
Disconnect during entry waits for entry to finish before requesting exit.
Repeated toolbar fullscreen clicks during an animation are ignored. Failed or
unacknowledged transitions retain reachable windows for retry, with a bounded
30-second deadline. An already-active tablet stays owned through temporary
Space-transition focus gaps while PLANK remains active; switching to another
app or disconnecting still releases ownership. Screen removal uses AppKit's normal
window migration and updates backing scale; this requires physical acceptance.

Focused tests cover authenticated topology parsing, independent modes, negative
origins, crops, aspect/letterbox edges, mixed local point scales, cursor seams,
stale-frame rejection, shared frame delivery, bookmark compatibility, primary
mapping on either side, display removal fallback and window focus.
Fullscreen close/transition tests use an AppKit window double; they do
not establish physical Space disposal or multi-monitor Wacom acceptance.
The fixture `Tests/Fixtures/output-topology-v13.json` is from the accepted root
`e532a5e`, `tests/protocol/output-topology-v13.json`; the right-primary fixture
is `tests/protocol/output-topology-v13-virtual-primary.json` from the same pin.

Targeted acceptance: start single, change to two 2560 × 1440 outputs at 60 fps,
move one window to another Mac display, and check mouse edges plus pen pressure,
taps, buttons and held drags across outputs. Switch focus, close/reopen display
2, enter/exit fullscreen, disconnect with it fullscreen, and return to single.
Finish with display removal/reconnect if two physical displays are available.
No acceptance is inferred from a compiled build or from two local windows.

## Build

Initialize the existing common-C submodule. Supply compatible dependency inputs:

- `PLANK_TRANSPORT_DIR`: accepted `plank-transport` Cargo source;
- `PLANK_FFMPEG_DIR`: Apple silicon macOS 15 FFmpeg prefix;
- `PLANK_OPUS_DIR`: pinned libopus 1.6.1 macOS prefix;
- `PLANK_RELAY_SOURCE_DIR`: matching raw drawing Relay crypto source;
- `PLANK_RELAY_SODIUM_PREFIX`: libsodium 1.0.22 macOS 15 static prefix.

Use `scripts/build-macos-native.sh <build-dir>` with those environment variables.
The build isolates its Cargo outputs by deployment target. Bundle/sign with
`scripts/stage-macos-native.py`, supplying the FFmpeg prefix, license source
folders and a signing identity. Local staging does not notarize or publish.

Run `scripts/test-macos-native.sh <output-dir>` for focused production policy,
Wacom callback lifetime, preflight, decoder recovery and tablet-input checks.
The worker callback check constructs the production Swift session on the main
actor and invokes its sender through the production C wrapper on a foreign
thread. It also checks queue refusal and callback revocation after disconnect.

## First live acceptance

Use a test workstation with the existing accepted Host package. Close any other
Client that owns the USB Wacom. Keep the existing network unchanged.

1. Start the pilot, add the workstation, sign in, and open the desktop. Grant
   Input Monitoring when macOS asks, restarting the pilot if required.
2. Check hardware decoder selection in statistics, exact-colour video and
   audible left/right stereo. A hardware capability failure must be visible.
   Check the remote cursor is visible, then enter/leave full screen and retest
   pointer placement at the canvas edges.
3. With the Wacom attached directly by USB, verify hover, light/firm pressure,
   tip taps, held drags, side buttons and supported ExpressKeys. Check the log's
   six preflight gates rather than inferring attachment from cursor movement.
4. Change volume/bitrate in the toolbar popover while drawing; capture should
   stay owned. Switch to another app: capture must release. Return and retest.
5. Disconnect while a tip/button is held, then reconnect without using the
   mouse first. No stuck button or old generation may survive. Also test one
   resolution/timing change with the existing close-and-sign-in warning.
6. Unplug/replug USB and quit during drawing. Physical ownership must return
   to macOS and reacquire only for a fresh active session.

Do not change the Host to make this pass. Native queue-pressure and periodic
stutter issues remain separate; preserve any terminal failure evidence.

## iOS and iPadOS

Initial shared-model, recovery and Bluetooth source typechecks passed during
the feasibility audit. There is no linked or qualified iOS app yet. A mobile
pilot needs UIKit presentation/audio/lifecycle adapters, an explicit input
scope, and Relay enrollment. It must use the Relay raw-HID path for Wacom;
this Mac IOHID capture worker is not an iOS implementation. Validate the Mac
engine before extracting a broader shared Apple module.

## Build 20 session-window controls

Build 19 display arrangement was accepted by the operator on October 7. Wacom
startup failed twice and worked on the third connection; the failed symptoms
and workstation are not established, and attachment logs were not retained.
That remains an open investigation, not a qualified startup fix.

Build 20 makes toolbar fullscreen/disconnect actions global as described above.
Focused checks exercise serial two-window entry/exit, mixed states, independent
green-button actions, repeated clicks, disconnect during entry, transition
failure, single shared disconnect and disposal after both exit acknowledgements.
The AppKit animation is substituted in these checks; physical dual-display
fullscreen and Wacom behavior require the targeted live pass.

## Build 21 fullscreen chrome and scroll candidate

Build 20's operator report accepts ongoing pen operation but identifies the
primary window's green button as unavailable in fullscreen (available when
windowed), plus intermittent scrolling. Global fullscreen entry and exit are
recorded in its runtime log; complete disconnect acceptance is not inferred.

Build 21 revalidates each window's native green button on focus changes and
local-chrome hover/click dispatch, including when its delegate is unchanged.
This preserves independent green-button behavior and session-wide toolbar
commands. Physical fullscreen-button acceptance remains pending.

Wheel input targets the desktop under the pointer even when it is the non-key
session window. The Host pointer is positioned before scrolling without
changing keyboard focus or Wacom capture. Precise deltas keep fractional
remainders instead of losing every sub-unit event; ordinary wheel notches keep
the existing 120-unit scale. Controls, letterboxing and inactive-app input remain
excluded. Release clears fractional state, and saturation cannot replay a long
scroll tail. Focused checks cover fractional/sign/axis handling, legacy scale,
reset, saturation and invalid deltas, plus native green-button revalidation.

## Build 22 Space-transition input candidate

Build 21's operator reports that the window buttons work, but leaving fullscreen
can strand a mouse pointer while the pen draws a separate moving cursor. Returning
to fullscreen restores normal behavior; windowed Wacom acquisition is slower.
The first recorded connection also hovered without tip clicks despite a successful
Host attachment acknowledgement. That tip failure is not yet localized. Wheel
acceptance remains pending; the build-21 wheel implementation is retained.

Build 22 routes mouse hover to the actual unobscured desktop canvas while PLANK
is active, including a non-key window, without moving keyboard focus. Real clicks
focus their desktop; local controls and other apps retain normal input. The pen
hides the parked native cursor even when its Host cursor occupies the other
output. Background desktop surfaces no longer restore that cursor over the
pointed-at window. Tracking and the responder are refreshed after Space changes.

Individual green-button transitions now participate in tablet focus preservation,
as toolbar transitions already do. This cannot acquire an inactive tablet, bypass
app deactivation or disconnect, or outlast a 30-second grace. Temporary surface
reparenting preserves session membership; genuine dismantling still removes it.
Raw HID capture, report ordering, Host policy and the network are unchanged.

The primary Session Controls offer **Reopen second display** only when its other
window is absent. Existing windows are activated without a duplicate open request;
reopen dismisses the popover before opening the scene, and popover buttons use an
explicit independent style. No fullscreen command is issued on this path. The
previous unexpected fullscreen side effect needs physical acceptance; action-origin
and tablet-focus logs now distinguish reopen, toolbar, green-button and focus events.

Focused checks cover pen/mouse presentation across output and size changes,
individual transition notifications and bounded grace, existing/missing secondary
window requests preserving presentation, and temporary reparent versus teardown.
They substitute Space animation and cannot qualify physical Wacom tip behavior.

## Build 23 wheel candidate

The operator reports build 22 improves cursor/window behavior. The secondary
window remained reachable through macOS's Window menu; its reopen action had
been hidden while the window existed. Wheel behavior is reported as barely
working, so that feature is not accepted.

Build 23 keeps **Show second display** available to bring an existing window
forward, or **Reopen second display** when missing. Neither issues fullscreen.
Canvas wheel events are routed once through the local event monitor, independent
of a stale first responder; other windows, controls and letterboxes keep native
AppKit handling. The original event is consumed only after eligible forwarding.

Wheel conversion follows the established Mac Client's Cocoa/SDL path: use
NSEvent's line-equivalent delta values, round conventional sub-tick events away
from zero, use 120 protocol units per detent, and cap acceleration at one detent
per axis per event. Precise devices preserve fractions; the natural-scroll
preference is already applied by AppKit, and horizontal direction follows Cocoa.
Gesture start/cancellation, completed momentum and input release clear stale
fractions. Bounded per-second wheel counters identify forwarding without logging
pointer coordinates or event contents. Synthetic, unposted AppKit line/pixel
fixtures verify the production converter; physical wheel acceptance is pending.

## Build 24 wheel routing candidate

Build 23's physical wheel events reached the Linux virtual mouse, including
conventional up/down detents, but the operator reported little scrolling in
gedit. The established desktop Client sends wheel changes without preceding
absolute motion. The native pilot had repositioned the pointer on every tick.
On Linux those operations use XTEST and uinput respectively; GTK resets its
scroll baseline when the source device changes. This is a source-supported
explanation, with live correlation and acceptance still pending.

Build 24 removes that extra motion from wheel handling. Actual mouse movement
continues to position the Host pointer over either desktop, including the
non-key window. Eligible wheel events retain their existing conversion and
single delivery through the event monitor. Local controls, inactive apps and
letterboxes remain excluded. Host policy, tablet capture and pointer movement
are unchanged. Qualify stationary scrolling in both desktop windows, then
movement between them and fullscreen/windowed transitions.

### Build 24 targeted acceptance

The operator reports substantially improved wheel behavior, then accepts
scrolling across both desktop windows and fullscreen/windowed transitions.
A separate Host-side capture confirms balanced middle-button press/release
delivery and a released final state. This accepts the wheel correction and
middle-button transport; application-specific middle-button gestures were not
separately reported. The precise GTK device-switch mechanism remains a source
inference rather than a correlated live trace. Earlier accepted display
placement and session-window behavior remain the comparison checkpoint.

Full Mac release qualification, including repeated Wacom startup and interruption
coverage, remains open. Signed build 24 is a development pilot, not a notarized
replacement for the desktop Client.
