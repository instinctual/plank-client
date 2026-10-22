# Native iPad pilot

Separate development application, `com.instinctual.plank.ipadpilot`, using the
existing native session/media services at their current source paths. Shipping
Mac and Vision app identities and targets are unchanged. iPadOS 26 and Apple
Silicon device/simulator are the initial compile scope, not a final OS matrix.

## Current accepted development checkpoint — October 10

Installed iPad31 is the accepted editor checkpoint, with compatible Vision49
installed and native Mac build 26 signed/staged for separate device qualification. The pilot
supports direct remote playback/input and foreground Apple Pencil sharing from
the normal client screen, plus one saved artist-control library in both modes.
Layouts, bindings, labels, button sizes and positions are editable; the toolbar
offers quick layout selection, touch-and-hold editing, visibility and global
transparency. Empty editor space hides the inspector; a control tap reopens it,
and moving or resizing clears it out of the way.

See [the current checkpoint and review handoff](../docs/development/ipad-client-checkpoint.md)
for exact runtime revisions, accepted tests and remaining gates. Earlier build
sections below retain the development history and are superseded by that record.
Live image overlays and magnification remain planned separately.

## Current keyboard behavior — build 17

The keyboard icon opens UIKit's native keyboard and sends typing directly to
the remote application. Esc, Tab and Hide remain in the native assistant bar.
There is no PLANK typing preview or local text echo. A docked keyboard reduces
the visible desktop using UIKit's screen-frame report converted to the canvas;
a floating keyboard overlays it without changing its size. Video and input use the same rectangle,
and resizing retires held input. SwiftUI does not reserve keyboard space again.
The video height is derived from the current canvas and keyboard intersection,
never from the previous video height or a separate guide reservation.
Keyboard presentation with Bluetooth attached remains under iPadOS's native
controls. For full-size typing while holding the Pencil, the tested setup is
**Settings → Apple Pencil → Scribble off**. On October 8 the user confirmed
this resolves the small-only keyboard state on build 17, with Bluetooth still
connected. Putting the Pencil down also restored normal presentation after
one or two opens. Scribble controls local handwriting-to-text; PLANK's remote
Pencil drawing uses its separate normalized pen path and does not depend on it.
Ordinary floating keyboards support More → Full, but that action was missing
in the user's Pencil-related state. Do not rely on it as that state's workaround.
This acceptance covers the controlled repeated-open check, not an independently
recorded floating/full-size geometry cycle.
The earlier build notes below retain the history of the removed preview.

## Local sign-in keyboard — build 27 candidate

Username and Password use local native fields with explicit focus-aware
scrolling after keyboard layout changes. Next advances to Password; Go submits
Sign In when both fields are filled and authentication is idle. Connect and
Sign In retire credential focus. Password clearing and the streaming-only
remote input gate remain separate from this local form. The login correction
does not change the desktop's keyboard viewport or Pencil sharing.

## First slice

The iPad24 custom-controls revision uses a fullscreen editor with a uniformly
scaled live-surface preview, six-point edge/neighbor snapping, and separate
Resize mode. Desktop and Pencil Sharing retain one saved library with separate
measured preview dimensions. Direct finger holds survive drift; Pencil contact
over a key goes to the drawing surface. Global cancellation clears both key
ownership and held appearance. See [shared custom controls](../docs/development/ipad-custom-controls.md)
for the reported iPad23 defects, local verification and remaining device gates.
The user reports iPad24 working considerably better in both direct desktop and
AVP sharing, with intermittent button releases still open. iPad25 changes the
grid icon to one-tap Show/Hide Controls; long-press opens Edit Controls. The grid
button stays available beside the restore button when the desktop toolbar is
hidden. This does not add a physical keyboard shortcut or live image overlay.
Installed iPad26 moves naming to a native Cancel/Save popup independent of the
resizing canvas. The Control inspector edits only Button label; Rename Layout
is an explicit layout-menu action. Reassigning a key updates its generated
name, while custom action labels remain; Use Key Name restores automatic names.
The editor still saves only through its outer Done action. The user accepted
this naming revision on October9.

Pencil sharing build 19 corrects the hover-to-contact timing epoch in the
foreground pad. First build 18 / Vision 47 tests passed hover/right-click but
failed tip/drag on two workstations. See the [Pencil relay record](../docs/development/ipad-pencil-relay.md)
for the reproduced publisher defect and separate normalized-Host pressure-policy
observation. On October 9 the user reports the requested build 19 tap/drag test
working well on Flame03; correlated Host events show tip edges and pressure.
The user also accepted desktop reconnect and fresh strokes after rotating the
sharing pad. Focus, lock/background, final held-contact receipt and latency
qualification remain separate gates.

Build 20 adds **Pad Options**: independent edge margins, Match desktop/Use full
pad mapping, and a dark charcoal surface with adjustable shading and optional
warm gray. Settings persist locally. Pad glow adjusts the app's appearance;
device display brightness remains in Control Center. Opening options pauses
drawing and retires the current contact. Geometry/persistence checks pass;
The user accepted these build 20 options on October 9.

Build 21 / Vision 48 adds a floating shortcut pad to Pencil sharing. Its keyboard
button toggles finger-held Shift, Ctrl, Option, Command and Space, with a draggable
header. Closing the pad releases held keys. The version 2 authenticated protocol
requires both apps and a fresh code comparison for the expanded capability;
existing identities and pen-only approvals remain preserved. Local keyboard and
pad ownership merge at the receiver. Source-specific queue/codec/ownership checks
pass; simultaneous physical finger/Pencil and interruption acceptance are pending.
The dimmed desktop overlay remains planned.

One saved workstation, login, one aspect-fit desktop, video/audio, direct finger
mouse emulation, indirect pointer buttons, physical wheel and hardware keyboard.
Local controls live above the canvas; opening a control sheet retires locally
held input. The toolbar's up-chevron hides PLANK's navigation/status bars and
expands the canvas. A small down-chevron restores the toolbar. This preference
persists; the connection screen always shows its navigation bar. Hiding or
restoring controls cancels held local input without restarting the stream or
changing the remote display mode. iPadOS retains its own window management and
system gestures. Build 9 replaces the compose-and-send keyboard sheet with live
on-screen typing (see below). Rotation/size changes cancel the current local contact. Background
ends this pilot's stream and requires reconnect; it does not log out Linux.

Build 2 simplifies resolution selection to **Balanced (1920×1200)** and
**Sharper (2560×1600)**, both 16:10. Existing bookmarks retain their saved mode
as an additional choice until explicitly changed. The new-bookmark default is
Balanced. Rotation resizes the local aspect-fit viewport; it does not renegotiate
the Host or crop/stretch the desktop.

These are interim supported modes, not exact iPad fit. The test iPad Air's
landscape panel is 2360×1640 (59:41). The pinned virtual-startup Host mode list
has no exact match. The maintained parent already documents optional bounded
physical-startup display matching (`0x400000`), but that contract explicitly
keeps headless virtual startup preset-only. Exact-fit support must use a
capability-qualified existing matching path where available, and qualified
virtual EDID modes for headless workstations. Do not blindly send arbitrary
sizes or stretch/crop the image to claim native fit. No Host deployment is
included in this pilot change.

Build 3 adds Apple Pencil through the existing normalized pen protocol (type 7),
separate from mouse emulation and raw Wacom identity. The Host must advertise
pen support (`0x01`), and a selected raw Relay disables this pen path. Actual
coalesced contact samples carry subpixel coordinates, force divided by UIKit's
reported maximum, and altitude/azimuth converted to Host tilt direction. Hover
uses UIKit's normalized distance. Predicted samples and late estimated-property
updates are not sent. Build 3 does not map squeeze, double-tap, eraser or barrel roll.
USB-C Pencil does not provide pressure sensitivity; an unknown/zero force range
sends zero pressure rather than inventing a measurement.

Rotation, canvas dimension changes, control sheets, focus loss and local source
handoff request cancel/proximity leave. Lift and start a fresh stroke after a
geometry change. Terminal up is accepted even at the last move's timestamp;
stale moves cannot restart a cancelled contact. Finger/pointer movement cannot
interrupt an active Pencil stroke. Settings show negotiated Pencil availability.
The local probe has observed contact, hover and rotation cancellation, and the
user reports force responding. On October 8 the user accepted remote pressure, navigation and tip/cursor
alignment on Flame1 with build 3. The initial offset occurred inside Flame and
resolved when the user turned off Flame Tablet Margins. Use zero Tablet Margins
for this Pencil path; do not compensate the iPad coordinate mapping globally.
Remote tilt and rotation/interruption recovery remain unqualified; compilation
and wire tests do not qualify them.

Build 4 adds persistent **Windows / PC Keyboard** and **Apple Extended Keyboard**
choices in Session Controls. F1–F12 are literal function keys in either mode;
PC maps F13–F15 to Print Screen/Scroll Lock/Pause, and Apple keeps F13–F24
literal. Hardware key edges use GCKeyboard while connected, with UIKit as the
fallback. Space receives a priority local command while the remote canvas has
focus, preventing local control activation; its actual down/up still come from
the hardware adapter. A Pencil contact preserves held keyboard shortcuts.
Focus loss, opening controls, keyboard removal and mapping changes retire held
keys. iPadOS-reserved shortcuts and keyboard media actions are not remapped;
use the keyboard's Fn key if it emits media actions instead of F-keys.

Pencil Pro squeeze sends one right mouse click (wire button 3) on gesture end,
at the supplied hover location. Lift the tip before squeezing. It does nothing
while drawing, another mouse button is held, local controls are open, or the
Pencil is outside hover range/the video area. This avoids ending an artist's
stroke or releasing a physical mouse button. Double-tap and barrel roll remain
unmapped. Apple Pencil system shortcuts can consume squeeze before the app
receives it. On October 8 the user confirmed F-keys reach Flame with Fn held on a separate
Bluetooth Apple Magic Keyboard; ordinary top-row keys still control the iPad.
The user subsequently confirmed Space during Pencil dragging and squeeze right-click
working. No iPadOS
setting reversing the keyboard's default top row has been verified.

Build 5 separates UIKit discrete wheel and continuous scroll recognizers. Each
nonzero physical-wheel callback sends one bounded 120-unit protocol notch;
continuous motion accumulates fractional units at a nominal 32 view points per
detent. Horizontal direction follows the existing Mac adapter. Cancelled events,
local controls, letterboxes and active Pencil strokes do not send scroll input.
Focus/geometry changes clear fractional motion. Bounded aggregate console counts
record locally submitted scroll events/units, not Host acknowledgment. Physical
wheel speed/direction and continuous-device behavior require device acceptance.
The user reported build 5 did not scroll. Build 6 corrects recognizer admission:
indirect-pointer input is allowed explicitly, with zero touches so contact drags
cannot become scrolling. Session Controls → Show statistics reveals received,
submitted and blocked scroll counts. They remain available after reopening the
controls, even when a launch console has ended. This is a diagnostic correction,
not a device acceptance claim.

Build 6 delivered balanced wheel-up/down events to Flame1's XInput mouse, but
the user's gedit document with hundreds of lines still did not scroll. Build 7
removes the absolute pointer update previously sent before each wheel event,
matching the accepted Mac correction: alternating XTEST pointer motion with
uinput wheel input resets GTK's scrolling baseline. Hover and contact still
position the pointer; scroll admission and unit conversion are unchanged.
Application scrolling acceptance remains pending.

Build 7 produced occasional small scrolling, which the user reported was still
mostly ignored. Build 8 suppresses stationary mouse-hover positions as another
possible source of XTEST/uinput alternation. It compares exact remote pixels,
without smoothing, and preserves contact/button positioning. Pen handoff, focus
and geometry changes reset the comparison. Scroll diagnostics include pointer
move and stationary-suppression counts; callback counts do not prove the cause
or application acceptance. Wheel scale is unchanged.

On October 8 the user accepted build 8 physical-wheel scrolling in gedit on
Flame1: "Working great now." The log shows wheel submissions continuing while
stationary hover callbacks are suppressed; during one stationary interval the
pointer-move count stayed at 88 as scroll submissions rose from 15 to 70.
This qualifies the tested physical wheel, not continuous or horizontal scrolling.

Build 9 makes the toolbar keyboard icon show/hide UIKit's keyboard while the
canvas remains the first responder. Characters go to the remote application
immediately; there is no local composition field or Send button. Backspace also
works against pre-existing remote text. Return comes from the keyboard, with
Esc, Tab and Hide Keyboard in its accessory bar. Opening controls, losing focus,
backgrounding or disconnecting hides it and retires locally held input.
Keyboard-driven viewport resizing follows the existing geometry cancellation.

Committed ASCII characters use the Vision software-keyboard mapping; other
characters use the existing UTF-8 text path. Autocorrection, capitalization and
smart punctuation are disabled. This is basic UIKeyInput, not qualified complex
IME composition. Hardware mapped keys retain their existing single-owner path.
iPadOS controls whether a full software keyboard is visible while a physical
keyboard is connected; no private keyboard-forcing API is used. Live typing,
Backspace/Return, accessory keys and hardware coexistence need device acceptance.

The user confirmed build 9 keyboard presentation works after turning off the
Bluetooth Magic Keyboard. Apple also documents switching to the onscreen keyboard
without disconnecting hardware through the bottom Shortcuts button → Show Keyboard:
[Apple iPad guide](https://support.apple.com/guide/ipad/ipaddd28d7ed/ipados).
No documented public API to force this system action was identified; do not use
private keyboard APIs or claim PLANK's icon forces presentation with hardware.

Build 10 adds a two-line, 24-point live typing preview above the software keyboard.
It echoes the latest 160 committed characters, follows Return/Tab and Backspace,
and clears on Escape, keyboard hide/open, focus loss and disconnect. This is a
read-only echo of software-keyboard typing, not a synchronized remote document;
remote mouse/cursor edits and physical keyboard text are not reconstructed.
Text stays in memory, is not logged/saved, and is sent live just as in build 9.
Focused checks cover grapheme deletion, CRLF, bounded length and clearing; device
preview/layout acceptance is pending.

The user accepted build 10's preview behavior, requested a smaller native style,
and reported that with Bluetooth connected only PLANK's accessory/keys appeared;
the system Show Keyboard chooser was not available. The prior Apple-guide route
therefore remains unqualified in this custom-input application.

Build 11 uses one compact preview row with the system Body font and Dynamic Type,
removing the separate heading and bespoke toolbar. Esc/Tab/Hide use native iPad
input-assistant groups. A standard UITextField owns software typing so UIKit
can provide its native hardware-keyboard shortcuts/chooser. It is an invisible
responder, stores no editable remote document, and forwards committed characters
live. Physical key edges retain the existing owner; switching responders retires
held input once without silently hiding the keyboard request. Aggregate focus/
hardware/window state can be logged, never typed text. Full native keyboard
presentation with Bluetooth attached is a device acceptance gate, not a claimed
public forcing API or proven fix. Preview is still a bounded software-text echo.


The user confirmed build 11 can display a floating keyboard with the Bluetooth
Magic Keyboard connected and expand it to full size using two fingers. This
qualifies that presentation route, not software/hardware typing coexistence.
The user then reported the accessory preview drifting after a floating/full-size
round trip. Build 12 makes the preview an app-owned view constrained to UIKit's
keyboard layout guide, with `followsUndockedKeyboard` enabled. It follows the
keyboard's width/position, stays within the canvas, and moves below a floating
keyboard near the top edge where there is no space above. Esc/Tab/Hide remain
in the native assistant. Preview hit areas do not forward input to the desktop.
No keyboard accessory or keyboard-frame notification offsets are retained.
Mode round trips, floating movement and rotation require device acceptance.
See [Apple's keyboard layout example](https://developer.apple.com/documentation/uikit/adjusting-your-layout-with-keyboard-layout-guide).

Build 12's on-device screenshots still showed displaced preview/extra blank
space after floating-to-full-size expansion. Build 13 gives the active desktop
one UIKit keyboard-space owner: SwiftUI navigation/content avoidance is disabled
for the desktop, a flexible geometry container keeps its full bounds, and one
public guide places the preview and reserves space only for a docked keyboard.
Floating keyboards overlay the desktop. Video and input mapping share the same
viewport frame. The isolated `Tests/KeyboardLayoutFixture.swift` uses the actual
production helper and system keyboard without connecting to a workstation.
Floating/full-size round trips and physical-keyboard coexistence remain device
gates; simulator geometry checks do not qualify them.

No raw Wacom driver or direct Bluetooth Wacom support is claimed. PTH-660
produced no visible probe input over USB or Bluetooth on the test iPad. The
registered Mac/Linux Relay route is the planned first Wacom path; its codecs
compile here but the enrollment/selection UI and iOS drawing qualification are
a subsequent slice. No Relay keys or approvals are imported from another app.

The shared engine retains the reviewed native recipe's exact parent/Kymux pins.
Upstream issue 9 still owns persistent trust, exact-format decoder fallback,
audio clock, input lifetime and current-parent reconciliation. This development
pilot is not a production security/precision or held-input release qualification.
Local release requests before disconnect are not proof of Host receipt: the
known shared sender shutdown ordering remains an upstream gate. No Host package
change or deployment is part of this target.

`PlankIPadVideo.swift` and `PlankIPadKeys.swift` mechanically extract the current
Vision fit/cursor container and UIKit key map; Metal presentation itself compiles
from its original shared source. These extractions are temporary pending the
agreed upstream shared-directory move, not new wire or decoder implementations.

## Unsigned build

The existing checksum/source-verified recipe accepts `ios-device` and
`ios-simulator`. Each needs its own fresh work directory and matching libraries.

```sh
rustup target add --toolchain 1.96.0 aarch64-apple-ios aarch64-apple-ios-sim
python3 scripts/apple-native/build.py prepare --platform ios-device \
  --work "$PLANK_IPAD_WORK" --cache "$PLANK_NATIVE_ARCHIVES"
python3 scripts/apple-native/build.py deps --platform ios-device --work "$PLANK_IPAD_WORK"
python3 scripts/apple-native/build.py build --platform ios-device \
  --work "$PLANK_IPAD_WORK" --client "$PLANK_CLIENT_SOURCE"
xcrun swiftc -Onone visionos-native/Sources/Models/HostBookmark.swift \
  ios-native/Sources/PlankIPadDisplayOptions.swift \
  ios-native/Sources/PlankIPadKeys.swift \
  ios-native/Sources/PlankIPadInputPolicy.swift \
  ios-native/Tests/InputPolicyChecks.swift -o "$PLANK_IPAD_CHECKS"
"$PLANK_IPAD_CHECKS"
```

Signing, profile/device membership checks, installation and runtime acceptance
are separate. Do not treat successful compilation or standalone finger/mouse
probe acceptance as a remote desktop pass.

## Pencil checks

```sh
xcrun swiftc -Onone visionos-native/Sources/Services/PlankInputQueue.swift \
  visionos-native/Sources/Models/HostBookmark.swift \
  ios-native/Sources/PlankIPadKeys.swift \
  ios-native/Sources/PlankIPadInputPolicy.swift \
  ios-native/Sources/PlankIPadPencilPolicy.swift \
  ios-native/Tests/PencilPolicyChecks.swift -o "$PLANK_PENCIL_CHECKS"
"$PLANK_PENCIL_CHECKS"
xcrun clang -O1 -DPLANK_NATIVE_TRANSPORT=1 -I "$PLANK_TRANSPORT_DIR/include" \
  visionos-native/Tests/PlankPenBridgeTests.c \
  visionos-native/Bridge/PlankRawHidFrame.c \
  -Wl,-dead_strip,-undefined,dynamic_lookup -o "$PLANK_PEN_WIRE_CHECKS"
"$PLANK_PEN_WIRE_CHECKS"
```

Orientation follows Apple's [azimuth definition](https://developer.apple.com/documentation/uikit/uitouch/azimuthangle(in:))
and the existing Host virtual-pen convention, with four cardinal-direction
fixtures. Host observation of tilt is still required.

## Follow-up keyboard and Pencil sharing

Build 13 improved floating presentation, but on-device expansion still displaced
the full-size preview. Build 14 uses the public keyboard notification frame,
converted from the matching screen into canvas coordinates, for docked preview
and viewport layout. Floating placement retains the tracking guide. Simulator
fallback checks do not qualify the hardware-attached expansion gesture.

[iPad Pencil sharing to PLANK Vision](../docs/development/ipad-pencil-relay.md)
records the next requested feature and its implementation order. It requires a
negotiated normalized-pen source on both apps; raw Wacom Relay compatibility is
not assumed. The existing workstation normalized pen path is the intended
backend, with no new Host package planned.

Build 14 still failed full-size expansion on the device. At the user's request,
build 15 removes the preview, its text mirror, custom guide/notification layout
and obsolete preview fixture/checks. Native live input and assistant controls
are retained. The old keyboard fixture paths in earlier notes are historical;
they were removed along with the feature. Device keyboard transitions and live
typing remain the focused acceptance check for this simpler candidate.

## Build 18 Apple Pencil sharing candidate

Share Apple Pencil on the connection screen opens a foreground, aspect-fit pad.
Choose the iPad in the matching PLANK Vision candidate's Settings → Apple Pencil,
compare the code and approve on both devices. Keep the raw Tablet Relay Off.
The iPad pad sends actual normalized Pencil samples and lifted-tip squeeze
right-click; it does not emulate a Wacom HID device. Closing, backgrounding or
locking the pad stops sharing. The first link is local network, with separate
app-owned identity and approvals. See [the contract and acceptance scope](../docs/development/ipad-pencil-relay.md).
Build19 contact, reconnect and rotation tests passed. Build21 hotkeys are
accepted working; build22 adds saved shortcut ordering in Pad Options. The
latest margin remapping report remains under investigation despite passing
local corner checks. Image overlay is planned. Existing direct desktop input
remains a separate accepted path.
