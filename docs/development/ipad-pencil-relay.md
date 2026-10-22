# iPad Pencil sharing to PLANK Vision

Status: development implementation October 8, 2026. Publisher, receiver,
versioned records and physical comparison approval are implemented. Compilation,
local protocol checks and physical device acceptance are separate gates.
This is not yet a qualified drawing transport.

### Current browser action — Vision55

At the user's request, **Show Workstations** is now under **Window** in the
desktop's **Session Controls** menu. Selecting it closes the menu and opens the
existing single workstation browser. The separate button above the desktop is
removed. Vision54's unique browser and launch selection are retained.

The production menu, app scenes, ornament and registered-window helpers pass a
native Simulator check: the action is absent outside the menu, remains reachable
inside it, closes the menu and reopens one browser while the stand-in session
stays available. The fresh full device build, exact signed package/dependencies
and independent installed app/process checks pass. Vision55 is installed under
the user's disconnected authorization. The user subsequently reports it worked
great, accepting this placement. Input, focus, tablet and image/transport
behavior are unchanged.

### Image control placement — Vision56

At the user's request, Vision56 moves **Share desktop image with this iPad** and
its sending status into **Session Controls → iPad image**. Vision's Apple Pencil
setup retains discovery, selection and comparison approval. The session toggle
is disabled until an iPad is connected and uses the existing consent setter;
opening the menu does not enable sharing. Native Mac's active settings source
is byte identical and its installed client is unchanged.

Two exact-production-menu Simulator cases pass disconnected admission, absence
from connected setup, on/off consent, status updates and retention across menu
reopening, with one stand-in peer/session and no disconnect. Show Workstations
remains reachable. All receiver, image and input lifetime/protocol sources and
425 protected native/recipe paths remain exact. Fresh device compilation and
independent signature/profile/resources/dependency checks pass. The user then
confirmed the AVP desktop was disconnected and authorized installation. Vision56
is installed and launched; separate app and process inventories verify56/0.1.0
and the exact running executable. Signed55 remains available for rollback.
The user subsequently accepts the56 menu placement. native Mac build 31 and iPad46 were
unchanged by56. Native checks and installation do not establish
new physical image delivery or latency acceptance.

### Automatic image reception — iPad47

The user accepts Vision56's placement and requests automatic image readiness on
the Sharing pad. iPad47 is built/signed, independently verified and installed after the user's
disconnected authorization. Exact47/0.1.0/container inventory passes. After the
user unlocked and opened PLANK, separate process inventory verifies the exact
installed47 executable. The existing process was preserved. The iPad's receive switch is
removed. The existing **Share desktop image with this iPad** choice on Mac/AVP
is the sole image on/off control and remains default off. Dimming and local
zoom/navigation remain on the iPad; installed senders need no update.

Only foreground Sharing listens, using the registered Pencil name and exact
current approved pen binding. Automatic readiness does not itself change
mapping. An authenticated image session selects proportional image/Pencil
geometry; ending it restores the saved input-only mapping. Contact and Artist
holds retire before either geometry change. Picture expiry alone does not
remap. Sharing stop/background removes image authority. Exact lifecycle and
contact fixtures pass. The user reports automatic reception working nicely,
accepting that ordinary-use trial; sender transitions and duration were not
separately described, and sustained image latency remains open. See the
[automatic-reception boundary](ipad-live-image.md#automatic-reception--ipad47).

### October18 AVP trial and browser recovery

On installed Vision53, the user reports that the tablet worked perfectly in
their trial. Reported interruptions involved losing window focus or accidentally
disconnecting the iPad. Accept the reported tablet trial; its duration, image
toggle state and final Host release receipt were not independently measured.
This does not resolve the earlier Mac-to-workstation transport failures.

The same report identifies a missing way to reopen the main workstation browser
after closing it during a session. Vision54 adds a **Workstations** button above
the desktop, alongside Session Controls. It opens the single browser window
without starting, replacing or disconnecting the current session. The browser is
the preferred launch scene and the desktop is suppressed from default launch.
An explicit recovery control is still required: the isolated native trial found
that reactivating an already-running app alone did not recreate a closed browser.
Apple describes the distinction between launch selection and explicit scene
reopening in its [window life cycle guidance](https://developer.apple.com/documentation/visionos/handling-the-window-life-cycle-with-multiple-scenes).

Two focused native Simulator workflows pass repeated close/reopen with one
browser and one stand-in session, and explicit reopening after Home/reactivation.
Production input, focus, tablet and transport bodies remain unchanged. The fresh
device build, unchanged recipe/dependencies, exact signature/profile/resources
and independent installed app/process checks pass. Vision54 is installed under
the user's disconnected authorization. The user confirms that Workstations
reopens the main window and the desktop session stays available, accepting this
targeted recovery check. Automatic reactivation alone remains unqualified.
The mouse/keyboard focus-return delay and disjoint
settings/transparency controls are recorded follow-ups, with no system-wide
cause or focus repair claimed.

### First device result and contact correction — October 9

The user reports hover and squeeze right-click working on iPad 18 / Vision 47,
but tip clicks and dragging failing on Flame01 and Flame03, with some lag.
These are partial results, not drawing acceptance. The sharing pad omitted the
hover retirement performed by the accepted direct iPad contact path. A production
policy reproduction shows hover stamped at callback delivery can exceed a fresh
touch's acquisition timestamp: down is rejected, no touch is retained, and its
move/up events are ignored while hover continues working.

iPad 19 validates a fresh contact on a temporary policy, retires old hover, then
commits and emits leave before down. Invalid/margin starts preserve prior state;
duplicate active downs and stale motion remain rejected. Aggregate contact
counts are logged at most once per second, without coordinates or identities.
The user reports iPad 19 working well on the requested Flame03 tap/held-drag
test. The correlated Host capture contains balanced tip presses/releases and
varying pressure; publisher counters also show accepted downs, moves and ups.
This accepts the targeted contact correction, not the complete interruption
or latency qualification. A read-only Flame03 check also found the
normalized `PLANK Wacom Tablet` using Pressure Recalibration=1 despite Host
1.1.030; its packaged policy currently targets the raw mirror's udev tag.
The successful targeted test required no Host configuration/package change.
The normalized pressure-policy distinction remains recorded for investigation
if another failure needs it; this pass does not require changing it. Lag and
video decoder recovery remain separate observations. The user subsequently
confirmed desktop disconnect/reconnect recovery with the iPad sharing pad kept
open, on iPad 19 / Vision 47. Accept that new-source reconnect check without
repeating it. The user also accepted the requested rotation during contact,
lift and fresh stroke in both orientations on iPad 19 / Vision 47. Focus,
lock/background and held-contact final release qualification remain pending.

### Build 20 pad options — October 9

The sharing pad now defaults to neutral charcoal rather than a system background
that can become white in light mode. **Pad Options** offers independent left,
right, top and bottom margins (0–40% of the pad inside its 16-point safety inset),
**Match desktop** or **Use full pad**, and charcoal/warm-gray tone with adjustable
pad glow. The whole sharing surface and its options use dark appearance.
Glow changes only the app's shading; device brightness remains a Control Center
setting. These choices persist locally and do not change peer identity or consent.

Match desktop retains the accepted aspect-fit mapping inside the chosen margins.
Use full pad maps the whole chosen rectangle to the desktop and can scale the
two axes differently. Percentages follow the current iPad orientation. The visible
boundary, Pencil contact/hover and squeeze share one mapping. Opening options or
changing geometry retires contact; drawing is paused while options are open,
then requires a fresh contact. The direct iPad desktop retains its existing fit.
Focused geometry, persistence and contact checks pass. The user accepted the
build 20 options on October 9: “The options here worked well on 20.”

### Build 21 / Vision 48 shortcut pad candidate — October 9

The keyboard button in Share Apple Pencil toggles a compact floating shortcut
pad: Shift, Ctrl, Option, Command and Space. Hold a button with a finger while
drawing with the Pencil; sliding out or lifting releases it. Drag its header
to reposition it, or close it to release all pad-owned keys. It does not open
the system keyboard or require Scribble. Accessibility activation explicitly
toggles a held key and announces its state. The user subsequently reported
that the hotkeys work well. Lock/background and final Host release receipt
remain separate qualification gates.

The version 2 authenticated capability adds only allowlisted shortcut key edges.
It requires updated apps on both ends. Existing private identities and version 1
approval accounts are retained; version 2 uses a separate approval account and
requires comparing codes once for the expanded capability. Old authenticated
version 1 handshakes and records are rejected. No raw Relay approval is reused.
The receiver merges local keyboard and pad ownership so only the last owner
releases a key, including Space; local keyboard repeat remains available.
Modifier flags accompany existing Host key events. Focus, desktop lifetime,
geometry/options, pad hiding, background and peer failure retire pad ownership.
Local retirement does not establish final Host receipt. Image overlay is next
planned work, not implemented by this candidate.

## October 9 installed shortcut candidates

iPad21 and Vision48 from `0eac6ba` are installed and their executable paths
independently verified. The simultaneous finger-held shortcut/Pencil check is
accepted by the user's October 9 hotkey report. The same report identifies a
margin issue: the cropped area appears not to span the remote desktop. This
supersedes the earlier general mapping acceptance; charcoal appearance remains
accepted. Mac build25 is
a separate candidate using the same version2 wire and approval boundary.

## Build 22 shortcut ordering and margin investigation

Pad Options now provides up/down ordering controls for all five shortcut keys.
The first four fill the palette's top row; the last fills its bottom row.
Order persists locally, with existing margin/mapping/appearance settings
preserved. Reordering retires held input before replacing controls.

The margin report remains unresolved. Both mapping modes pass corner checks,
including the actual UIKit outline, window-to-pad conversion, contact policy
and wire encoding in an isolated simulator fixture. This is local component
evidence, not physical end-to-end acceptance. A read-only device preference
check found Match desktop with left 6%, right/top/bottom 5%. Build22 records
changed pad geometry only (no stroke positions or typed content) to distinguish
local remapping from downstream workstation/application mapping. No coordinate
compensation or Host change is made without that evidence. Image overlay remains
planned after these controls; it is not implemented.

Build22 from `40029d7` is installed under the user's disconnected authorization.
Independent device inventories verify version22 and its exact executable path.
Vision48 stays installed. Saved order and outlined-corner desktop reach remain
the next targeted device checks; the margin issue is not claimed fixed.

The user subsequently confirmed a full editor for freely positioned,
independently sized, assignable controls, available in both the direct iPad
client and Pencil sharing. See [the shared controls design brief](ipad-custom-controls.md).
This is the next design scope; build22 still uses the fixed slot layout.

## Custom controls candidate — October 9

The user selected the bottom editor sketch: freely placed and independently
sized keys with a compact neighboring inspector. **Custom Controls** now opens
the same saved library/editor from the direct iPad desktop and Pencil sharing.
The fixed slot palette and up/down ordering UI are superseded in the new source;
the legacy order imports once without changing pad margins, tone or consent.
Keys can hold or tap a supported key with optional Shift/Ctrl/Option/Command.

The new source uses version3 `normalized-pen-keys` and matching iPad/AVP/Mac
candidates. Existing identities and old approval accounts stay stored, but the
expanded capability requires a fresh comparison approval. New local component
evidence does not supersede installed22/48 physical acceptance. See
[shared controls](ipad-custom-controls.md) for editing, ownership and device gates.

## Intended use

### Mac receiver extension

The build25 Mac candidate receives the same authenticated version-2 Pencil
and held-shortcut records from iPad21. It exposes **Apple Pencil shared from
iPad** as an explicit next-session tablet source. That session does not create
the physical USB Wacom worker or a registered raw Relay connection; source
changes cannot alter the running session's snapshot. Mac Settings and Session
Controls reuse the same discovered-pad, comparison and approval view.

The Mac's existing union-of-desktop-windows focus drives admission. Local
controls separately pause Pencil and retire its keys/contact, while preserving
the historical raw Wacom controls behavior. Accepted Pencil samples select
the remote Host cursor; physical mouse movement returns the native cursor.
Composite-stream coordinates and existing output crops are retained. No new
Host contract, dependency pin or physical-Wacom payload change is introduced.

Pencil is one receiver at a time: end the previous receiver's Pencil link or
stop/restart iPad sharing before changing between Mac and Vision. Mac physical
pressure/alignment, simultaneous shortcuts, focus/controls retirement and
reconnect are pending separate acceptance. Image overlay remains planned.

Enable **Share Apple Pencil** on the iPad, approve the headset, then select that
iPad in PLANK Vision. The iPad becomes an absolute tablet surface while the AVP
shows the workstation. First transport is an authenticated local network link;
Bluetooth advertising/drawing is a separate platform/transport qualification.
No workstation package change is planned: use the existing type-7 normalized
pen capability only when the workstation advertises support.

## Existing paths and required extension

- `ios-native/Sources/PlankIPadPencilPolicy.swift` already converts real UIKit
  Pencil samples to `PlankNormalizedPen`: hover/contact/up/cancel/leave,
  normalized position, pressure or hover distance, tilt and tilt direction.
  Its `rotation` is tilt direction, not Pencil barrel roll.
- `visionos-native/Sources/Services/PlankCoreClient.swift` already sends those
  packets to the workstation. It currently refuses normalized pen when a raw
  Relay is selected. Add an explicit source/capability distinction; do not
  relax that guard for every Relay.
- `PlankRelayLiveLink` and the Mac Relay advertise `pltr-raw-hid`, transferring
  Wacom messages and workstation replies. Apple Pencil is not that device.
  Add a versioned normalized-pen capability and bounded authenticated record;
  unknown versions and older raw-only clients must reject it explicitly.
- Reuse reviewed identity, pairing, enrollment and authenticated framing
  boundaries. Isolate portable peer/crypto code from Mac IOKit capture before
  reuse on iPad; never import the Mac USB driver or manufacture Wacom identity.

## Implementation order

1. Define the normalized-pen capability/record and exclusive source policy,
   with schema/codec checks, finite-range validation and unsupported-version
   rejection. Preserve terminal edges; bound/coalesce replaceable hover/move
   samples without dropping contact transitions.
2. Add iPad foreground sharing with a clear pad surface, persistent app-owned
   identity and explicit physical approval. Direct desktop Pencil capture and
   sharing are mutually exclusive. Rotation/active-area changes cancel contact;
   require a fresh stroke. Lock/background/disable ends sharing and unregisters
   discovery. No claim of background Pencil capture.
3. Add AVP discovery/registration/selection for the negotiated pen capability,
   and forward accepted samples through the existing normalized pen sender.
   Raw Wacom and normalized pen own the tablet path exclusively. Gate input
   on active desktop focus and workstation pen support; retire stale peer
   generations on disconnect/reconnect. Never reuse another app's approvals.
4. Map squeeze to right-click through the existing admission/ordering policy;
   reject it during an active stroke. Do not invent unsupported eraser, barrel
   roll or ExpressKeys. Support model-dependent hover/pressure only when UIKit
   supplies them; mark unavailable features plainly.
5. Build signed iPad and AVP candidates and qualify pressure, alignment, pen
   controls, reconnect, rotation, lock/background and held-contact disconnect.
   Use the existing Host unchanged. Local cancellation is not proof of Host
   receipt: shared sender teardown remains an upstream qualification gate.

## Geometry and acceptance

Map one visible active pad area to the selected remote display using normalized
coordinates. Match desktop preserves the target aspect ratio; Use full pad
fills the user-selected area. Margins and aspect letterboxes do not start strokes.
Do not add global coordinate compensation for Flame Tablet Margins; the direct
Pencil test already established zero margins. A working standalone iPad desktop
Pencil does not qualify iPad-to-AVP forwarding, and the accepted raw Wacom Relay
checks do not qualify this new source. Initial scope is one approved headset,
one active pen owner and one selected remote display.

## Development implementation

The iPad pilot's **Share Apple Pencil** starts a foreground pad and advertises
`_plank-pencil._tcp` with `version=3`, `capability=normalized-pen-keys` and its public
identity. PLANK Vision Settings has a separate Apple Pencil section. The raw
Tablet Relay must be Off. The independent capability is deliberately not a
`pltr-raw-hid` descriptor; legacy raw clients do not discover or decode it.

Both apps use their own device-only, when-unlocked Keychain identity and peer
pins. Initial connection requires comparing the same twelve hexadecimal digits
on both devices and approving locally on each. The code is the first six bytes
of the authenticated IK first-message transcript hash, including the initiator
identity, ephemeral key and pinned advertised responder identity. Discovery
alone is untrusted. Existing approved identity pairs skip the comparison; a
changed public identity or expanded version3 capability requires a new comparison. Pins are not shared with
Setup, Wacom Relay, Mac or another app. One pending/active headset owns the pad.

`apple-native/Shared/PlankPencilCrypto.c` calls the existing pinned relay Noise
IK implementation without changing it. It authenticates the exact
`PLANK-NORMALIZED-PEN-KEYS/3` capability payload in both handshake messages. The
underlying fixed TCP Noise prologue is reused; application purpose is bound in
the encrypted handshake payload, not a newly claimed prologue. Empty raw-drawing
handshakes and mismatched purpose/version are rejected. This direct physical
comparison path does not use Setup-mediated enrollment, which remains the
existing Wacom registration path.

Records have a two-byte little-endian length, capped at 256 bytes. Handshakes
start with `PLPN` and byte version3. Secure application plaintext also starts
with `PLPN`, version3 and a one-byte message kind. Configuration carries two
UInt32 display dimensions and one Boolean; pen carries phase, three Float32
values, UInt8 tilt and UInt16 tilt direction; right-click carries two Float32
coordinates. Modifier edges carry an allowlisted UInt16 virtual key and one
Boolean, with strict press/release ordering. Generic key batches add a one-byte
count and up to48 ordered code/pressed/modifier entries; masks and codes are
validated against the finite key catalog. All multibyte values are little-endian. Ping, pong and end have
no payload. Unknown kinds/versions, trailing bytes, nonfinite or out-of-range
values, inconsistent contact transitions, authentication failures and replay
close the peer. Transport nonce ordering comes from the existing Noise codec.

The peer serial executor owns crypto and one socket write at a time. Bounded
incoming/outgoing mailboxes admit before scheduling work, coalesce consecutive
same-phase motion only, and fail closed on edge overflow. The receiver also uses
a bounded, coalescing source-specific admission method at the workstation input
queue; refusal closes and retires the Pencil source. This does not change other
input sources or establish final Host receipt. Approval has one
absolute 60-second deadline; a connected peer has a five-second heartbeat
limit. The iPad retains actual samples only. Direct iPad desktop input and the
sharing pad are separate, mutually exclusive surfaces. The receiver requests
pad geometry from its primary desktop's actual frame dimensions and pauses
admission outside supported active sessions. Mapping/focus/background changes
retire contact; a late move cannot start a new stroke. A new tip-down releases
local mouse buttons, and pointer/buttons/wheel cannot interrupt that stroke.
Squeeze is a right-click only while the tip is lifted. Rotation ends the old
contact and requires a fresh stroke.

First acceptance must establish iPad → AVP → workstation drawing, pressure,
alignment and right-click, followed by focus, rotation, lock and reconnect.
Compilation, a working direct iPad Pencil or a working raw Wacom Relay does not
qualify this chain. Bluetooth and background capture remain outside this slice.
Host sender teardown and final receipt remain the existing upstream gate.

### Local verification

The focused Swift checks passed finite/range validation, unknown-version and
trailing-byte rejection, contact ordering, fresh-down after cancellation and
10,000 motion samples coalesced without losing stroke edges. The C adapter
passed under AddressSanitizer/UndefinedBehaviorSanitizer: matching transcript
codes, authenticated capability negotiation, empty raw handshake rejection,
same-length wrong purpose rejection, preapproval rejection and encrypted replay
failure. A real TCP loopback fixture with injected in-memory consent storage
passed two-sided comparison, no durable approval before comparison, configuration
acknowledgment and ordered down/move/up pressure delivery. These tests do not
qualify the physical iPad, AVP, Keychain entitlement or workstation drawing.
Each desktop lifetime retires its peer generation. The selected iPad can reconnect
on the next supported desktop with its existing verified identity; callbacks from
the previous peer cannot enter that desktop. An unexpected network failure
requires selecting the pad again rather than creating an unbounded retry loop.

### Client-screen mode — iPad29 candidate

The user approved moving Pencil Sharing into the normal iPad client screen.
Sharing status, comparison approval, Pad Options and Stop Sharing move into
the client controls and menus. Desktop and sharing use the same full-window
artist-control canvas and editor reference, independently of local toolbar
height. The drawing pad keeps its own margins and normalized coordinate
mapping. This local UI change retains existing identities, physical approval,
version3 wire authority and matching Vision49/native Mac build 26 receivers.

Direct desktop input and sharing remain mutually exclusive. Entering the
editor or pad options retires contacts and pauses sharing input while keeping
the authenticated peer. Explicit stop and a non-active scene end sharing.
The new menu flow and saved-layout placement remain physical device gates.

The targeted native UI fixture passed shared control-canvas/editor geometry,
foreground local-control priority, peer-retaining editing/settings, source
retirement and Pencil forwarding. It uses the user's unchanged saved library,
controlled landscape bounds and stubbed network/Host callbacks; it does not
qualify physical rotation, new end-to-end input or final Host release receipt.


## Optional live reference image — October10 development slice

The separate image source for iPad34 / Vision50 / native Mac build 27 adds a dimmable full-desktop reference inside the existing Pencil viewport, enabled by explicit image choices on both devices. It uses its own bounded authenticated media lane tied to the complete current pen transcript; the accepted v3 pen/key wire, approvals, mapping and input queues stay unchanged. Image failure clears the picture and preserves Pencil operation. Device compilation/signing and physical mapping/input-under-image-load qualification are distinct from earlier accepted input tests. See [the current image boundary](ipad-live-image.md). Zoom/pan and focused output crops remain later coordinated-transform work. Installed iPad33/Vision49 and staged native Mac build 26 are unchanged at this source checkpoint.


## Intermittent sharing latency diagnostics — candidates44/53

The iPad43/Vision52 trial reported intermittent severe Pencil delay on Flame-01.
The initial recovery with reference images disabled did not reproduce on the
subsequent image-on/off comparison, and enabling AVP Tailscale made no difference.
The cause remains unconfirmed; accepted earlier contact/input tests are retained.

App-only diagnostics use existing peer ticks, receiver synchronization and
session polling, with at most one aggregate per reporting path every two seconds.
They retain bounded scalar counters and timestamps beside existing bounded
mailbox entries. They log no coordinates, pressure, key contents or peer identity.
Mailbox slot residence is distinct from the age of the latest coalesced value.
Receiver task wait/drain, current submitted batch, native C submission and QUIC
handoff are distinct boundaries. Timings use each device's local monotonic clock;
unsynchronized endpoint clocks must not be subtracted. Network send completion
and `nativeInputSent` do not prove delivery to the Host. Snapshot reads across
separate ledgers are not atomic, and instrumentation adds unqualified overhead.
No protocol, consent, source admission, queue capacity, ordering, timer cadence,
QoS, Host, network or dependency-pin change is included.
