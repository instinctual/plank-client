# Native iOS and iPadOS readiness

Audit date: 2026-10-08. Status: input probe partially accepted; single-display
iPad Client has targeted playback, mouse, keyboard and resolution-switching
acceptance, not production qualification.

The audit and build notes below are historical. The October10 accepted iPad31,
Vision49 and staged native Mac build 26 state is summarized in the
[current checkpoint](ipad-client-checkpoint.md), including Pencil sharing,
saved artist controls and their separate remaining qualification gates.

## Recommended order

Start with an iPad input probe for Apple Pencil, then a native single-display
Client using the shared media/session stack and registered Relay drawing.
Include external Wacom in the design from the beginning. Treat direct USB
capture as a separate feasibility gate rather than a prerequisite for the
first Client. Investigate direct Bluetooth only against an exact tablet model
and accessible protocol.

This work can proceed while the native Mac foundation is reviewed. Changes to
shared trust, input lifetime, transport precision and the maintained-parent
baseline remain coordinated through [Client issue 9](https://github.com/instinctual/plank-client/issues/9).

## Baseline and evidence limits

The inspected native Client is `b45921542e9e7224fc6787ee0abc09e2d10e5e8a`,
including Mac foundation `ac9fb757979cf6c92768448345abad592920ddc5` and tablet
sharing `73add890536d38b42b11d687cd0a68ddd48ae90e`. The public native recipe
still selects parent `6c6865562713d265a657dec613f55169ccb379b2`, Kymux
`8654cfece0fe5f3ab35177f520ca9378f6d35c24`, managed Relay `73a3743` and raw
Relay `029721f`. The independently inspected maintained parent is `0709ef0`;
its existence does not change those recipe pins.

UIKit Pencil, CoreBluetooth L2CAP, AVFAudio and Metal API references passed
standalone Swift typechecking for arm64 iOS device and simulator against SDK
27.0, using an iOS 18.0 probe deployment target. That target is not a chosen
product minimum. These checks establish API availability only: no complete
iOS app was built, installed or tested; no USB driver or entitlement was
validated. Existing AVP and Mac acceptance remains specific to those platforms.

## Input capability map

| Path | Evidence | Proposed status |
| --- | --- | --- |
| Apple Pencil → compatible iPad → workstation | Public UIKit touch, pressure, orientation and optional hover APIs; native transport already defines normalized pen input | First input probe and initial iPad drawing path |
| Wacom → Mac Relay → iPad/iPhone → workstation | Mac capture/sharing and native authenticated Relay receiver exist; iOS receiver not qualified | First external Wacom route, over TCP |
| Wacom → Linux Relay → iPad/iPhone → workstation | Existing raw drawing service and native TCP receiver; Bluetooth receiver uses CoreBluetooth GATT discovery plus L2CAP | TCP first; qualify Linux Bluetooth separately on iOS |
| Wacom USB → M-series iPad → workstation | Apple supports USBDriverKit on M-series iPads; no Wacom-specific driver proof | Feasibility investigation only |
| Wacom USB → A-series iPad or iPhone | The documented iPad DriverKit path requires M-series hardware | No direct raw USB implementation proposed through DriverKit |
| Wacom Bluetooth → iPad/iPhone → workstation | No verified public raw-report route for the target tablet | Research gate; generic pointer operation is insufficient |

Apple's [iPad driver guide](https://developer.apple.com/documentation/driverkit/creating-drivers-for-ipados)
and [USBDriverKit documentation](https://developer.apple.com/documentation/usbdriverkit)
provide the platform basis for the USB investigation. Wacom's own
[Intuos compatibility statement](https://support.wacom.com/hc/en-us/articles/1500006331582-Does-the-Wacom-Intuos-pen-tablet-work-with-an-iPhone-or-iPad)
does not promise direct iPad/iPhone support. A custom driver remains an
engineering hypothesis, not vendor-supported plug-and-play behavior. The
statement concerns Intuos; it does not resolve every Wacom product or model.

For iPhone, begin with touch, keyboard/mouse and registered Relay reception.
Pencil support targets Apple's compatible iPads. Capabilities must be checked
for each Pencil/iPad combination: for example, Pencil USB-C lacks pressure
sensitivity, and hover depends on the combination. See Apple's
[Pencil feature comparison](https://www.apple.com/apple-pencil/).

## What can be reused

Paths below refer to the inspected Client tree, before any shared/platform
directory reorganization.

| Existing component | Reuse candidate | Required platform work |
| --- | --- | --- |
| `PlankSessionEngine`, hardware decoder and media bridges | Session/media plumbing, VideoToolbox decode, native transport | Adopt the upstream-reconciled lifetime/contract; iOS foreground, lock and interruption behavior |
| `PlankAudioOutput`, receiver, ring and format policies | AVFAudio output and stale-audio retirement | Route, interruption and background validation on iOS |
| `PlankMetalVideoView` | UIKit/Metal presentation | iPad point/pixel scaling, safe areas, viewport transforms, rotation and window resizing |
| Host HTTP/discovery, trust and bookmark services | Connection and saved-host policies | Separate app identity/settings, local-network permission, UI and lifecycle |
| Relay keys, enrollment, live link and byte transports | Authentication, raw-HID framing, TCP and Bluetooth receiver logic | iOS handoff/enrollment flow and per-platform permissions/acceptance |
| Mouse/input policy helpers | Arbitration, coordinate and held-state concepts | UIKit pointer, wheel, keyboard, touch and Pencil adapters |
| Mac IOHID Wacom worker and wrapper | Raw-report, activity filtering and exclusive ownership design | Desktop IOHIDManager capture is not an iOS app capture implementation |
| Mac AppKit windows and `PlankMacInput` | Behavioral reference | New UIKit/SwiftUI iOS shell and input surfaces |

The current Vision app entry point uses visionOS scene/window policies, and
its build targets are Vision and Mac. A new iOS target needs iPhoneOS and
simulator Rust/C libraries, matching architecture and deployment settings,
assets, usage descriptions, signing and a reproducible build-input receipt.
Renaming the Vision app is insufficient. Keep the platform shell small;
directory moves should remain mechanical and distinct from behavior changes.

## Pencil: use normalized pen input

The pinned parent's
[input header](https://github.com/cnoellert/plank/blob/6c6865562713d265a657dec613f55169ccb379b2/protocol/plank-transport/include/plank_transport_input.h)
already defines `PLANK_TRANSPORT_INPUT_PEN` (type 7), a 32-byte payload and
`plank_transport_input_encode_pen`. It includes event/tool/buttons, coordinates,
pressure-or-distance, tilt and rotation. iPad pilot build 3 adds an additive
normalized-pen event to the shared queue and C bridge, using this canonical
encoder and the existing input sender. It requires the negotiated Host feature
`0x01`; a selected raw Relay excludes Pencil. No dependency or Host change is
part of this adapter.

The same parent's Host gitlink is `5829bf7c335440a8b25c3330643eacb4d914f00a`.
Its [input parser](https://github.com/instinctual/plank-host-linux/blob/5829bf7c335440a8b25c3330643eacb4d914f00a/src/input.cpp)
accepts type 7 and selects the normalized pen backend; its
[virtual pen backend](https://github.com/instinctual/plank-host-linux/blob/5829bf7c335440a8b25c3330643eacb4d914f00a/src/platform/virtualhid_input.cpp)
handles contact, pressure, distance and tilt. This source evidence supports
trying the existing contract before proposing a Host extension. It does not
verify any installed Host package or its advertised capabilities.

The first Pencil adapter should:

- Read actual UIKit Pencil samples, normalize supported force, and preserve
  sample order. Use coalesced touches without forwarding the terminal sample
  twice. Keep acquisition independent of SwiftUI redraws.
- Map positions through the visible video rectangle to the selected remote
  display; exclude letterboxing and local controls. Define behavior for zoom,
  pan, rotation and geometry changes during contact.
- Convert altitude/azimuth into the existing tilt/orientation convention with
  a fixture and Host observation. The current backend uses packet rotation to
  derive tilt direction: Pencil Pro barrel roll is not that same quantity.
  Independent barrel roll may require an explicit contract addition later.
- Emit contact up, cancel and proximity leave consistently on cancellation,
  focus/background transitions, source switches and disconnect. Establish
  cleanup ordering before stopping the sender; do not equate local submission
  with Host acknowledgement.
- Keep predicted touches local to an optional preview. Sending them as real
  remote pen events would create speculative input that cannot be retracted.
- Treat unsupported pressure, hover, squeeze, double-tap and barrel roll as
  explicit capabilities. A pressureless contact policy must be labeled; do
  not report a fabricated measurement. Gestures must not invent ExpressKeys.

Primary API references: Apple's
[force and orientation example](https://developer.apple.com/documentation/uikit/illustrating-the-force-altitude-and-azimuth-properties-of-touch-input),
[coalesced touches guide](https://developer.apple.com/documentation/uikit/getting-high-fidelity-input-with-coalesced-touches),
[predicted touches reference](https://developer.apple.com/documentation/uikit/uievent/predictedtouches(for:)),
and [Pencil hover sample](https://developer.apple.com/documentation/uikit/adopting-hover-support-for-apple-pencil).
The ordering, cancellation and remote-preview rules above are PLANK design
requirements, not claims that these APIs alone enforce them.

## External Wacom: preserve the drawing semantics

Registered Relay drawing should retain the existing authenticated,
bidirectional raw-HID path: device identity/descriptors, input reports,
workstation-to-device communication, pressure, tool/button transitions and
exclusive capture ownership. A pointer-only fallback must not be described
as qualified Wacom drawing.

Pencil uses a normalized virtual pen. External Wacom uses its existing raw
device path. Do not synthesize a Wacom USB identity for Pencil or run both
capture sources simultaneously by default. Keep source selection explicit,
retire held state before takeover, and avoid duplicate remote endpoints.

The Relay discovery UI must distinguish discovering a service from reaching
and authenticating it. The earlier Mac/AVP network experience showed why a
working Apple virtual display is not proof of a reachable Relay TCP route.
Keep source availability and actual drawing connection status separate.

### Direct USB feasibility gate

An iPad driver is a separate bundled extension with app communication and
user enablement. Apple's supported iPad families include USBDriverKit; the
iPad guide does not list HIDDriverKit as a supported family. The existing Mac
IOHIDManager worker must not simply be compiled into the iPad app.

Before committing to a direct driver, establish:

1. Exact iPad chip/OS and Wacom USB vendor/product/interface/report descriptors.
2. Whether an approved USBDriverKit match can obtain the required interfaces
   when system HID handling exists, including bidirectional feature/output
   operations. Validate exclusive ownership and app/user-client boundaries.
3. Required signing/provisioning and Apple-approved device entitlements, plus
   activation, hotplug and restart behavior on real hardware.
4. Whether the same raw wire contract can be retained without report rewriting,
   and which supported tablet models can be qualified reproducibly.

Apple documents the approval process in
[Requesting Entitlements for DriverKit Development](https://developer.apple.com/documentation/driverkit/requesting-entitlements-for-driverkit-development).
No entitlement request, distribution approval or successful Wacom device claim
is established by this audit.

### Bluetooth feasibility gate

Receiving from the Linux Relay is distinct from capturing a Bluetooth Wacom
directly. The existing receiver discovers PLANK's GATT service, reads its PSM,
opens a CoreBluetooth L2CAP channel and authenticates the saved Relay identity.
Its iOS API availability passed typechecking; its iOS runtime remains untested.

[CoreBluetooth](https://developer.apple.com/documentation/corebluetooth)
supports documented Bluetooth services, but that does not establish access to
arbitrary Wacom HID channels. Direct support needs a verified model/protocol
and public access path. Likewise, [ExternalAccessory](https://developer.apple.com/documentation/externalaccessory)
requires manufacturer-supported accessory protocols and is not a generic HID
escape hatch. Do not advertise direct Bluetooth Wacom support based on pairing
or a moving system cursor.

## Delivery slices and acceptance

1. **Standalone iPad input probe:** Pencil hover where supported, contact,
   force/tilt, coalesced samples, cancel/leave and local mouse/wheel/keyboard.
   Record timing/counts and capability metadata; no full Client integration
   or Host package is needed for this source probe.
2. **Native iPad Client:** new build slice and single-display shell, using the
   agreed shared engine. Validate login, video/audio, coordinate scaling and
   input releases through the existing normalized pen contract on a compatible
   Host. Initially keep full multiwindow/multidisplay behavior out of scope.
3. **External Wacom via TCP Relay:** adapt enrollment/handoff to the iOS app
   identity. Qualify pen hover, tap, held drag, pressure, side/middle buttons,
   pad controls and return traffic for the selected supported tablet.
4. **Interruption qualification:** lock/unlock, foreground/background,
   resolution/timing changes, held contact at disconnect, tablet disappearance
   and return, Relay restart and source takeover. Inspect Host endpoints and
   releases; hover-only recovery is a failure.
5. **Linux Relay Bluetooth and direct USB research:** separate prototypes and
   receipts. Qualify the Relay path with its network disabled; require a real
   USB device claim before planning a production driver. Direct Wacom Bluetooth
   remains a separate gate.
6. **iPhone adaptation:** touch/mouse/keyboard and Relay reception after iPad
   foundation acceptance, with its own viewport and lifecycle checks.

The current hardware is iPad Air 11-inch (M4), iPadOS 26.5, with Wacom Intuos
Pro PTH-660. Pencil hardware is now available. The precise Pencil model is
still unrecorded; pressure/hover support must not be inferred for other models.

## Standalone probe checkpoint

The [local iPad input probe](../../experiments/ipad-input-probe/README.md) is a
separate UIKit app with contact accounting, actual/coalesced Pencil samples,
force/orientation readings, optional hover, pointer/wheel and keyboard
diagnostics. It makes no remote input or raw-Wacom capture claim. Device and
simulator unsigned compilation and the focused terminal/order/cancellation
checks passed on October 8. Development build 1 was signed, installed and
launched after profile/device membership verification. The user reports finger
and mouse working. On October 8, device screenshots show Pencil contact and
hover. The user reports force responding. Rotation deliberately cancelled one
contact: 26 downs, 25 ups, one cancellation and zero held contacts. Fresh strokes
after rotation and background cancellation remain unqualified. These are local
probe observations, not remote drawing acceptance.

The connected test device reports iPad Air 11-inch (M4), iPadOS 26.5. This
meets the documented M-series condition for a later USBDriverKit feasibility
investigation. The user enabled Developer Mode. PTH-660 produced no dot or
other visible probe input over both USB and Bluetooth. This is a failed
system-input probe, not a test of raw report capture, which the app does not
implement. Direct Wacom support remains a separate research gate.

## Single-display Client checkpoint

The [separate iPad pilot](../../ios-native/README.md) adds an iPad-only target,
independent identity/bookmarks, manual workstation entry/login, one fit desktop,
finger/pointer, physical wheel, hardware keyboard and session controls. It
compiles the existing shared session, video, audio and authenticated Relay
codecs from their original source paths. No transport dependency pin is moved.
Shared source adaptations guard the Vision spatial-audio API to visionOS and
omit unsupported presentation-timing callbacks in the iOS simulator. Device
rendering and the session/trust/precision/audio-clock implementations remain
unchanged.

Pencil remains separate from mouse emulation. Registered Relay enrollment/selection
and iOS Wacom acceptance are next; compiled receiver code is not drawing
qualification. iPhone and multiple displays are outside this first target.
The initial pilot deployment target is iPadOS 26, separate from the input
probe's iPadOS 18 target and from any final supported OS matrix.

On October 8, the user accepted playback, mouse, hardware keyboard and several
resolution changes on installed pilot build 1. This report does not establish
Pencil, pressure, Relay drawing, audio, wheel, held-contact teardown or sustained
qualification. The next local pilot simplifies display choices and makes the
session toolbar hideable; it requires its own focused device check.

### iPad display fit

The connected iPad reports native portrait pixels 1640×2360 and landscape-right
orientation, consistent with Apple's [iPad Air 11-inch (M4) specifications](https://support.apple.com/en-us/126471).
Its landscape aspect ratio is 59:41. The currently pinned native Client requests
only the shared qualified virtual modes; none matches that panel exactly.
The interim iPad choices are 1920×1200 and 2560×1600 (both 16:10), with saved
legacy modes preserved. They must not be labeled exact-fit or native resolution.

The maintained parent's `protocol/output-topology.md` already defines bounded
physical matching (`0x400000`), so a new general display contract is not the
first step. Coordinate adoption with issue 9, verify the installed Host's
capabilities/startup kind, and use that existing path only where negotiated.
Headless virtual startup remains EDID-preset-only under that contract and needs
qualified iPad modes before exact fit can be offered. Suggested native-panel
target for this device: 2360×1640; any lower-cost same-aspect preset also needs
timing/EDID and encoder qualification. Never silently send unsupported modes.

Toolbar visibility and rotation only alter local layout; they do not change
remote session geometry. Local input is retired before layout changes and the
renderer/input adapter continue sharing the same aspect-fit rectangle. This
avoids cropping, stretching or display/session churn just to hide controls.

Focused fit/edge, held-button/key release and fractional-wheel checks pass.
Unsigned device and simulator compilation, protected signing and remote
desktop acceptance must be recorded independently. The shared issue-9 gates
remain open, including unexpected-stop release forwarding and persistent trust.

## Remote Pencil candidate — October 8

Build 3 forwards ordered actual/coalesced samples through type 7. The adapter
uses the presentation viewport without pixel quantization, rejects letterboxing
for new contacts, clamps held contacts at the desktop edge, normalizes measured
force, and converts altitude/azimuth to tilt direction. Hover distance is the
UIKit normalized value. Unknown orientation is sent as unknown; zero/unknown
force range sends zero pressure. No predicted or late estimated samples are
replayed. Barrel roll, squeeze and double-tap are not implemented.

Focused checks cover subpixel mapping, force limits, all four Host tilt axes,
same-timestamp up, duplicate/stale moves, cancel/leave idempotence, new contact
after cancellation, queue transition order, canonical big-endian type-7 bytes,
invalid payload refusal and sender failure propagation. Local admission closes
on sheets, focus, size/rotation and source changes. The shared stop path can
still discard queued releases: disconnect/background Host cleanup is an upstream
qualification gate, not a claimed release guarantee.

Next acceptance is remote hover/tap/drag, varied-pressure strokes and tilt in a
compatible drawing app, plus rotation ending the old stroke and allowing a fresh
one after lift. Finger/mouse/keyboard and playback acceptance already recorded
must not be repeated as prerequisites. This candidate is separate from the
shipping Mac/Vision targets and adds no Host package deployment.

### Remote Pencil acceptance

On October 8, the user reported pressure and navigation working on iPad build 3
connected to Flame1. A stationary tip/cursor offset was isolated to Flame. The
Host's virtual stylus had an active-area top/left inset; turning off Flame's
Tablet Margins resolved the offset according to the user. Pressure, navigation
and alignment are accepted for this tested setup, without a client coordinate
change or Host package deployment. Keep Tablet Margins at zero for the Pencil
path. Remote tilt, rotation recovery and interruption/held-state cleanup are not
inferred from this pass. Do not repeat the accepted pressure/navigation/alignment
checks as prerequisites.


## Hardware keyboard and squeeze candidate — October 8

The user reported missing function keys and Space in build 3, qualifying the
previous general keyboard pass. Build 4 adds the same Apple/PC function-key
semantics as Vision, persisted in Session Controls, and uses the connected
GCKeyboard's actual key edges. UIKit remains a fallback rather than a second
owner. Mapping aliases retain their original down identity until release; two
Shift keys cannot release each other prematurely. Space is reserved locally
while the canvas owns focus. Starting a Pencil stroke now retires pointer
buttons without releasing held keyboard shortcuts. Local controls, keyboard
removal, focus and mapping changes still release held keys. No media-key or
system-shortcut interception is claimed.

Pencil Pro squeeze sends one balanced right mouse click at its hover location
on gesture end. It is suppressed during tip contact, held pointer buttons,
closed admission or missing/outside hover position. This uses mouse button 3,
not a fabricated Pencil barrel button. Squeeze delivery requires supported
hardware and an iPadOS squeeze preference that delivers the gesture to apps.
Double-tap is unchanged. Focused keyboard mapping/alias/release and squeeze
admission checks plus device compilation pass; live keyboard/squeeze acceptance
is pending. Shared shutdown receipt/Host cleanup remains upstream-owned.

## iPad wheel correction (build 5)

The prior adapter passed UIKit view-point translation directly as high-resolution
wire units. The working Mac adapter and common-C contract use 120 units per
wheel detent. Build 5 separates discrete and continuous UIKit scroll masks: a
nonzero discrete callback emits one bounded detent per axis, while continuous
input retains fractional motion at 32 points per detent (the existing Vision
fallback's nominal scale). Horizontal sign matches the Mac adapter. Cancellation,
focus/geometry reset, controls, letterboxes and active Pencil contact remain
guarded. Aggregate console counts measure submission only. Policy checks and a
device build are required; user wheel speed/direction acceptance is pending.

Build 4 F-key forwarding was accepted with Fn held on the user's separate
Bluetooth Magic Keyboard. Space with Pencil drag and squeeze remain pending.

### Build 6 wheel event admission

The user reported build 5 not working. Its launch process exited normally and
the app was reopened as a new process; the old console contains no test data,
so absence of console counts does not prove absence of callbacks. Comparison
with the maintained Moonlight iOS UIKit adapter identified an empty allowed
touch-type list in PLANK's scroll recognizers. Build 6 explicitly allows
indirect-pointer input and sets maximumNumberOfTouches to zero, admitting wheel
events while excluding contact drags. Both discrete and continuous masks retain
the build 5 unit conversion. Received/submitted/blocked aggregate counts appear
under Session Controls when Show statistics is enabled, independently of the
launch console. Runtime acceptance is pending. Reference: [Moonlight iOS
StreamView](https://github.com/moonlight-stream/moonlight-ios/blob/master/Limelight/Input/StreamView.m).

### Build 7 wheel delivery without synthetic pointer movement

Build 6 received wheel callbacks and a bounded Flame1 XInput capture confirmed
292 wheel notches with balanced button 4/5 edges. The pointer query identified
gedit under the mouse, with no mouse button held. The user confirmed hundreds
of lines in the document, but no scrolling. Host receipt alone was therefore
insufficient to qualify application scrolling.

The accepted Mac adapter already corrected this combination in commit
`5db90ff`: absolute XTEST pointer movement before every uinput wheel event
resets GTK's scroll baseline. Build 7 applies the same correction to the iPad
handler, leaving real hover/contact positioning, UIKit admission, unit
conversion and existing Pencil guards intact. No Host or shared transport
change is required. Device application acceptance remains pending.

### Build 8 stationary mouse-hover suppression

The user reported build 7 mostly ignored wheel input with occasional movement.
The stationary-mouse wheel test still recorded substantially more native input
messages than scroll submissions. This is consistent with hover-source
alternation, but the Host XTEST raw-event and core-motion-history reads did not
establish its cause. Do not describe those empty captures as proof of absent
pointer updates.

Build 8 adds exact remote-pixel deduplication for mouse hover only. Actual
one-pixel movement passes immediately; contacts, button positioning and Pencil
squeeze still send their position even when unchanged. Pen handoff, release,
focus and geometry changes invalidate the stored position. Aggregate pointer
move/stationary-suppression counters accompany the wheel diagnostics for the
next targeted test. No wheel scale, Host or shared transport change is made.
Policy checks cover repeated stationary callbacks, one-pixel movement, forced
contact positioning and position restoration after reset.

On October 8 the user accepted build 8 physical mouse-wheel scrolling in gedit
on Flame1: "Working great now." In the stationary test interval at
13:37:56–13:38:00 Pacific, local pointer moves stayed at 88 while scroll
submissions increased from 15 to 70 and stationary hover suppressions increased
from 30 to 114. These local counters support the stationary-hover explanation;
application acceptance comes from the user's test. Preserve that pass and do
not repeat it. The user subsequently confirmed the remaining Space-with-Pencil
drag and squeeze right-click checks working. Continuous/horizontal scrolling
remains unqualified.

## Build 9 live on-screen keyboard candidate — October 8

The user confirmed the remaining hardware shortcut/Pencil squeeze checks working
and requested direct typing from the toolbar keyboard icon. Build 9 removes the
compose-and-send sheet. The canvas implements UIKeyInput, toggling its system
input view while keeping remote input admitted. Committed characters are sent
immediately without a local editable document or recorded text. Backspace is
always available against remote content; Return, Tab and Escape retain existing
wire keys. The accessory toolbar provides Esc, Tab and Hide Keyboard.

ASCII uses the existing Vision software-character mapping; non-ASCII uses the
existing UTF-8 text command. Autocorrection, automatic capitalization and smart
punctuation are disabled. Hardware key ownership is unchanged: mapped physical
keys are consumed by the existing GCKeyboard/UIKit fallback path. Geometry
changes retire local contact; controls, focus loss, background and disconnect
hide the software keyboard. Shared sender teardown remains an upstream gate.

Focused checks cover mapping/order, controls, CRLF and non-ASCII fallback.
Device compilation/signing and live acceptance are separate gates. Full software
keyboard presentation while a hardware keyboard is connected remains controlled
by iPadOS. Complex IME composition is not qualified by this basic UIKeyInput
adapter. The targeted check is live typing into gedit, deletion of existing
content, Return/Tab/Escape, hide/reopen and absence of duplicate hardware text.

## Build 10 readable typing preview candidate — October 8

The user confirmed build 9's system keyboard appears when the separate Bluetooth
Magic Keyboard is turned off, then requested a larger preview above it. Apple
supports showing the onscreen keyboard with hardware attached via the bottom
Shortcuts button → Show Keyboard ([guide](https://support.apple.com/guide/ipad/ipaddd28d7ed/ipados)).
No public programmatic forcing API was found; no private API is introduced.
Do not label hardware-keyboard suppression as a confirmed PLANK focus defect.

Build 10 adds an accessory preview at 24-point text, displaying the last 160
software-committed characters over two lines and scrolling to recent typing.
Backspace removes one grapheme; Return/Tab preserve order. Escape or keyboard
hide/open/focus loss clears it. The preview cannot edit the remote field and
cannot reconstruct existing remote content, cursor selection or physical-keyboard
text. It remains memory-only, with no typed-text diagnostics or persistence.
The existing immediate wire submission and hardware key ownership are unchanged.
Focused bounded-preview and Unicode deletion checks pass. Device compile/signing
and user preview/layout acceptance remain separate gates.

## Build 11 compact native typing controls — October 8

User accepted the preview, requested smaller Apple styling, and reported that
only the custom preview/Esc/Tab were visible with Bluetooth attached. The system
chooser route documented by Apple was not available in this surface; it is not
an application-qualified workaround. Build 11 reduces the preview to one row
using semantic Body typography/Dynamic Type and native input-assistant groups.

A standard UITextField is now the software responder, returning false from
committed-change delegates after live forwarding and never retaining an editable
remote document. Hardware presses continue through the existing mapped edge
owner. Controlled responder transfer does not cancel the keyboard request;
controls/focus loss/hide/disconnect still retire it. Read-only focus/window/
hardware diagnostics contain no characters. This provides UIKit's normal text
input path, without private keyboard-forcing APIs. Bluetooth chooser/appearance,
compact preview and absence of duplicate hardware characters require targeted
device acceptance. Existing wheel, Pencil and Fn-held F-key acceptance carries
forward. Impeccable product context is recorded in PRODUCT.md; the native app
uses existing Apple semantic colors/components, not a new web theme.


## Build 12 keyboard preview positioning — October 8

Build 11 presentation passed with the Bluetooth Magic Keyboard still connected:
the floating keyboard expanded to full size. The user reported that minimizing
and expanding it again moved the text preview away from the keyboard. The
preview was still a UIKit keyboard accessory with its own frame/autoresizing.

Build 12 removes that accessory and places the read-only preview in the canvas's
Auto Layout hierarchy. The public keyboard layout guide follows undocked/floating
keyboards; its width/center and tracking constraints place the preview above the
keyboard, or below when close to the top. Required canvas bounds prevent it from
leaving the app when floating near an edge. Semantic typography/Dynamic Type and
native Esc/Tab/Hide remain. Preview areas reject remote contact, hover, squeeze
and scroll initiation. Live text/mapping/preview policy and hardware ownership
are unchanged. No private API, manual screen-space offset or extra Host change.

[Apple keyboard layout sample](https://developer.apple.com/documentation/uikit/adjusting-your-layout-with-keyboard-layout-guide)
is the implementation reference. Compilation does not qualify floating/full-size
round trips, movement, rotation or keyboard typing coexistence; these remain the
targeted device gates. Accepted wheel/Pencil tests need not be repeated.

## Build 13 single keyboard layout owner — October 8

Device screenshots showed build 12 still moving the preview across the desktop
when expanding the floating keyboard and leaving unused space under the canvas.
SwiftUI navigation avoidance and UIKit preview positioning were both accounting
for keyboard space. The iPad branch was rebased with its merge structure onto
Alan's accepted integration base before this change; its committed tree was
verified identical before restoring the in-progress patch.

Build 13 gives the active desktop a flexible GeometryReader and disables
SwiftUI keyboard avoidance at both the desktop content and navigation root.
UIKit's one public keyboard layout guide owns preview placement and viewport
space. A full-width keyboard at the bottom reserves its height plus the compact
preview exactly once; a narrow floating keyboard overlays the full viewport.
Video rendering and pointer/Pencil mapping use the same video frame. Connection
and bookmark screens keep normal keyboard avoidance. Native keyboard controls,
text delivery, keyboard ownership and the accepted wheel/Pencil paths remain.

An isolated simulator fixture compiles the actual production layout helper and
compares its preview/video frames with real UIKit keyboard notifications. It
passed docked presentation and hiding/reopening: the owner height stayed 1074
points, preview bottom matched actual keyboard top, video ended at preview top,
and hiding restored the full viewport. Orientation requests did not change the
simulator dimensions, so rotation remains unqualified. There was no Simulator
GUI available for floating gestures. These checks are not acceptance of floating/full-size gestures or Bluetooth keyboard
coexistence on iPadOS 26.5; those remain targeted device gates. No Host, dependency
pin, network setting or app identity change is included.

## Build 14 docked keyboard frame correction — October 8

The user accepted the floating keyboard improvement in build 13, but screenshots
still place the full-size preview across the desktop after expansion. The guide
position appears inconsistent with the visible docked keyboard; no geometry log
from that device establishes the precise UIKit cause yet.

Build 14 observes public keyboard frame notifications from the canvas window's
screen, converts them into canvas coordinates, and uses the intersecting docked
frame for both preview position and viewport space. This is a measured frame,
not a fixed device-height offset. Floating positioning still uses the accepted
tracking guide. Stronger docked constraints override stale floating constraints;
hiding/floating releases the override. Multiple screens are filtered by the
notification's UIScreen, with the canvas screen as the legacy fallback. Geometry
logging records mode only, never typed text or Pencil coordinates.

The simulator fixture retains actual open/hide/reopen checks and adds an
explicit synthetic notification/guide disagreement. That fault injection tests
the authoritative-frame fallback; it is not an on-device expand gesture pass.
Device docking/rotation and typing coexistence remain targeted gates.

The requested iPad-as-Pencil-tablet feature is scoped in
[the Pencil sharing plan](ipad-pencil-relay.md). Its negotiated normalized pen
path is distinct from the existing raw Wacom Relay; no working iPad publisher or
AVP receiver is claimed yet.

## Build 15 remove typing preview — October 8

The user reported build 14 still displacing the preview when expanding the small
keyboard and suggested removing it. Build 15 removes the app-owned preview and
text mirror entirely, together with preview hit regions, tracking constraints,
keyboard notification correction and their obsolete fixture/checks. The native
UITextField responder, live software/hardware mapping, Backspace, Esc/Tab/Hide
assistant and single hardware-key owner remain. There is no extra composition
field and no Send action. Typed output is visible in the remote application.

UIKit's keyboard overlays the unchanged desktop; the active GeometryReader and
keyboard safe-area opt-out keep video/input coordinates stable through keyboard
mode changes. No app-owned preview height or notification rectangle reserves
space. Thus the removed preview cannot be detached or duplicated on expansion;
this source fact is not a device keyboard-transition acceptance claim. The
retained software mapping/input policy checks and fresh device compilation are
the verification gates before targeted keyboard presentation/typing acceptance.
Pencil sharing remains the next scoped feature; no Host or transport changes
are included here.

## Build 16 restore docked keyboard space without preview — October 8

The user reported that build 15's full keyboard covered the desktop and the
keyboard stayed small on subsequent opens. Removing the preview had also
removed desktop accommodation. Build 16 restores that accommodation alone:
the video bottom is constrained to UIKit's own keyboard layout guide, with
floating-keyboard tracking and hidden bottom safe-area reservation disabled.
The root remains exempt from SwiftUI keyboard avoidance, so one owner reserves
space. Input uses the same video rectangle and retires held contact on changes.
No preview, local echo, custom keyboard accessory or keyboard-frame correction
is reintroduced. Geometry logs contain only canvas/video dimensions and the
guide top. Full-size selection remains iPadOS controlled; Apple's floating
keyboard More → Full action is the targeted device check, not a programmatic
mode-setting claim. Simulator dock/hide/reopen checks and a fresh device
compile precede installation; physical expansion and Bluetooth coexistence
remain user acceptance gates.

The isolated production-helper simulator check passed actual dock/hide/reopen: canvas stays 1074pt; video ends at both guide and keyboard top 757pt when docked/reopened, and restores all 1074pt hidden. Retained input policy checks pass. This is not floating/full-size gesture or physical-device acceptance.

## Build 17 absolute keyboard intersection — October 8

The user reports build 16 now expands, but floating-to-full makes the desktop
too small; hide/reopen restores it. Device geometry logs show a stable 746pt
canvas while the guide goes from 334pt on a normal full opening to 172pt after
the floating transition. The logs do not contain the corresponding notification
frame, so they establish inconsistent reservation, not its exact UIKit cause.

Build 17 replaces guide constraints with the public keyboard end-frame report,
filtered to the canvas window's screen and converted into canvas coordinates.
Only full-width bottom-intersecting keyboards reduce the video. The rectangle
is calculated from current canvas bounds on each layout, never the previous
video height or accumulated keyboard deltas. There is no fallback to the guide.
Floating/hide restores the full canvas; resizing continues to retire contact
and the input mapper uses the same video rectangle. Diagnostics record both
reported-frame and guide geometry for the next physical transition. Preview,
custom accessory and local text echo remain removed.

The isolated fixture exercises actual dock/hide/reopen plus synthetic floating,
expanded, repeated-expanded and guide/report disagreement. Those injections
test conversion and nonaccumulation, not iPadOS floating gesture acceptance.
Physical transition acceptance remains pending. No Host, dependency or network
change is included.

Verification: actual simulator dock/hide/reopen passed 757→1074→757pt. Synthetic floating restores1074pt; expanded/repeated-expanded both797pt despite guide757pt; restored report returns757pt. Direct checks cover an already-reduced owner and repeated absolute reports. This verifies the reservation correction under injected disagreement, not the physical iPad gesture.

## Keyboard mode diagnosis and accepted setup — October 8

The user reported build 17 opening full once and then only showing a small
keyboard without a Full action. A controlled comparison established that this
happened while the Pencil was in hand; putting it down and reopening once or
twice restored the normal keyboard. The next comparison left Bluetooth
connected, turned off Settings → Apple Pencil → Scribble, and reopened the
keyboard twice while holding the Pencil. The user confirmed: “That works!”

The Scribble setting is therefore the verified cause/control for the reported
Pencil-dependent mode on this device. Its internal UIKit state transition was
not measured. The accepted setup is Scribble off for full-size typing with
Pencil in use. No additional application build or change was needed for this
resolution. PLANK's normalized Pencil drawing does not use Scribble.
Apple documents the setting in [Enter text with Scribble](https://support.apple.com/en-ph/guide/ipad/-ipad355ab2a7/ipados).

Public UIScribbleInteraction allows rejecting local handwriting, while its
expected-Pencil flag is readonly. That opt-out has no documented guarantee of
forcing a full-size system keyboard, so it was not shipped as a speculative
fix. The bottom shortcuts bar can also occur with a hardware keyboard and was
not sufficient proof by itself. Accept the user's repeated-open result; do not
repeat the accepted mouse, wheel, pressure or typing qualification. The
separate geometry cycle is not independently established by this setting test.

## Build 18 Pencil sharing candidate — October 8

The foreground Share Apple Pencil pad and matching Vision build 47 receiver
are implemented using an independent normalized-pen capability. Both sides
require initial physical transcript-code comparison, store app-owned approvals,
and reuse the pinned relay Noise IK implementation. The pad follows negotiated
primary-display aspect ratio; actual pressure/hover/tilt samples and lifted-tip
squeeze use the existing normalized pen and mouse paths. Rotation, focus,
background and sharing stop retire contact; fresh down is required after pause.
Wacom/raw Relay selection excludes this source. Input mailboxes and receiver
submission are bounded and coalesce consecutive motion only.

Pure policy, sanitizer-backed authentication/replay checks and a real loopback
consent/configuration/pressure-stroke test passed. Device drawing and interruption
acceptance remain pending. Dependency/recipe pins and Host packages are unchanged.
The detailed contract is [iPad Pencil sharing](ipad-pencil-relay.md).

## Build 20 Pencil pad customization — October 9

iPad 19 / Vision 47 has user acceptance for corrected tip/drag, desktop reconnect
without pad reselection, and fresh strokes after rotating during contact.
Build 20 introduces locally persisted independent edge margins, optional full-pad
mapping and a dark charcoal sharing surface with adjustable fill shading. It
retains the accepted aspect-fit default and direct desktop mapping. Geometry
changes and opening Pad Options retire contact; admission pauses while the
options are open. Pure geometry, persistence and contact checks pass. The user
accepted these build 20 options on October 9. Vision 47 and Host packages remained
unchanged. See [Pencil sharing](ipad-pencil-relay.md) for the current contract.

## Build 21 / Vision 48 floating shortcut candidate — October 9

Finger-held Shift, Ctrl, Option, Command and Space coexist with Pencil drawing
in a movable sharing-pad overlay. Input edges use the version 2 authenticated
capability and separate v2 approval accounts, retaining device identity and old
pen-only consent records. Key ownership merges with the receiver's local keyboard;
bounded queue admission preserves modifier/pen ordering. Pure ownership/codec
checks, sanitizer-backed crypto purpose/version/replay checks and a real TCP
modifier-held pressure-stroke loopback passed. Physical simultaneous input and
release acceptance are pending. No Host/network/dependency changes. Dimmed image
overlay remains the next planned feature.

## Build 27 local credential keyboard candidate — October 9

After accepting iPad26's custom-control naming revision, the user reported that
the workstation sign-in keyboard covered Username and Password. The login
already uses normal SwiftUI keyboard avoidance: `hasActiveDesktopSession` is
false during credential entry and authentication. The fields sit below the
workstation controls and header, without explicit focus scrolling.

Build27 keeps the existing local fields and adds focus-aware native scrolling
after focus, viewport and keyboard-frame changes. Username's Next moves to
Password; Password's Go submits the existing Sign In operation when filled and
idle. Connect and authentication clear credential focus. Password clearing
before the authentication request and on connection/background is preserved.
This does not use the remote text responder or alter streaming input admission,
the desktop keyboard viewport, protocol, Host or dependency pins.

An isolated fixture with the actual production root and native docked keyboard
passed portrait visibility. A controlled 400-point scroll viewport reproduced
the clipping: the baseline password and Sign In were outside its visible area.
With the correction, both credential fields and Sign In are visible. The
reduced viewport is a layout experiment, not physical landscape acceptance;
the Simulator could not supply a matching landscape system keyboard. Physical
keyboard presentation, field progression and sign-in remain the targeted
device gate for this candidate. No new pen, wheel or transport test is required.
