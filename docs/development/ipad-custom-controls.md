# Shared iPad custom controls — design brief

## Confirmed direction — October 9

The user confirmed freely positioned keys across the drawing surface and
assignable keys/shortcut combinations. The same editor and saved layouts must
be available in the direct iPad desktop client and Share Apple Pencil. This
supersedes the proposed five-key-only, palette-confined editor scope.

iPad24/Vision49 introduced the revised custom controls. On October 9 the
user tested both Pencil sharing to AVP and the direct iPad desktop, reporting
that placement and operation work considerably better. Intermittent button
releases remain unresolved; this feedback does not qualify all interruption
or final Host release gates.

## Purpose and visual direction

An artist drawing on an iPad, often wearing an AVP, needs finger-accessible
shortcuts around the drawing area while the Pencil remains in use. The dark
charcoal surface remains quiet under the headset; the client variant overlays
controls on its remote canvas. Native iPad controls and semantic typography
follow PRODUCT.md. References are native iPad editing controls, the existing
PLANK pad options, and the accepted compact held-key presentation.

One **Custom Controls** entry opens the same layout library and editor from
both contexts. Showing controls is independent of opening the software keyboard.
Existing keyboard, Pencil, mouse and aspect-fit desktop input remain separate
input paths. Layout geometry must not alter remote coordinate mapping.

## Interaction contract

- Each control owns its identity, binding, label, width, height and position.
  Moving Space or changing traversal order never changes its size.
- Drag controls anywhere on the available canvas. Edges and neighboring
  controls snap into rows/columns with a six-point gutter and alignment guides.
  Resize mode exposes corner handles; native width/height adjustments remain
  available without precise dragging.
- Each control binds one supported keyboard key with optional Shift, Ctrl,
  Option and Command. Initial actions are **Hold** (down while held, release
  when lifted) or **Tap** (one complete shortcut). Hold does not generate
  synthetic repeated presses. Sequential macros are outside this brief.
- Labels may name the artist's action, such as Undo; bindings remain visible
  in the editor. Defaults retain the five currently accepted controls and a
  wide Space. Adding, duplicating and removing controls is part of the editor.
- Layouts have names and portrait/landscape arrangements, with copy/reset and
  undo for edits. Saved layouts are shared between client and Pencil sharing.
- **Edit** works on a draft. Entering editing releases active controls and
  ends drawing contact. Done saves; Cancel leaves the previous layout intact.
  Editing pauses drawing while retaining the approved foreground sharing peer
  and desktop session. Done and Cancel restore input admission to that source.
  Actual app backgrounding stops foreground sharing. Working mode fixes
  control positions and shows held states.
- Finger and accessibility input can use controls. Pencil never activates a
  shortcut control. A finger hold survives movement outside the key until lift
  or genuine cancellation. Provide accessible labels, held-state announcements
  and controls for position/size that do not require precise dragging.

Controls keep at least a 44-point hit area. Point sizes and normalized anchors
preserve useful sizes across contexts; clamp placement to the current surface.
Do not scale touch targets below that minimum to fit a smaller window. Validate
nonfinite/out-of-bounds geometry, bounded control counts and duplicate IDs.
Drawing margins and control placement are independent settings.

## Editor direction probes

Two generated sketches compare inspector topology, not shipped UI:

- **A — Edge controls:** modifier cluster near an edge, with a fixed side
  inspector while editing.
- **B — Split controls:** independent clusters around the canvas, with a
  compact inspector beside the selected control. Recommended to retain canvas
  space. It can use a native sheet in narrow windows.

Both permit free placement and the same bindings. The user selected **B**, the
bottom sketch, on October 9. Use the neighboring compact inspector and split
control arrangement. The probe's incidental text is illustrative: Hold sustains key state,
not automatic repeat. Image overlay is still separately planned.

## Source boundaries and implementation

Use one transport-neutral layout store, editor, overlay and action controller.
Direct desktop and Pencil sharing supply different sinks for the same key
edges; the renderer must not own transport or workstation policy.

The earlier five-key layout used slot-dependent widths: indexes0–3 shared an equal
row and index4 fills the lower row. Its position is transient SwiftUI State.
Import the existing sanitized key order when migrating, and preserve margin,
tone and consent settings. Stable per-control IDs own independent geometry.

Before connecting generic actions, introduce ownership per control/touch and
physical keyboard source. The direct router currently owns keys by code only;
the sharing receiver currently tracks only five modifier identities plus right
Command. First owner sends down, last owner sends up. Snapshot a control's
binding at contact start so changing its assignment cannot change its release.
Duplicate controls and overlapping combos must not release another owner's key.
Physical repeat retains its existing behavior.

Press a combo's modifiers before its trigger and release the trigger before
owned modifiers. Reserve capacity for an entire edge batch before updating
ownership. Hide, edit, geometry replacement, focus/background and disconnect
retire the relevant owners. Local retirement is not final Host receipt.

Sharing version2 is authenticated for the five existing key identities only.
General letters/function keys need an explicit new capability/version and
separate comparison/approval, preserving old identities and approval records.
Update the AVP and shared Mac receiver together. The expanded version3 capability requires matching new publisher/receivers and a fresh physical comparison. Version2 clients are not discovered by this capability; existing v1/v2 approval records remain intact. There is no partial or silent combo delivery. Reuse the existing Host
key transport; no Host package change is part of this feature.

Suggested coherent slices:

1. Shared layout/action model, legacy migration and bounded key ownership.
2. Native editor/library and overlay in both iPad contexts.
3. Expanded authenticated sharing capability and matching AVP/Mac receivers.
4. Signed candidates, then targeted direct-client and shared-Pencil acceptance.

## Verification and open gates

Focus tests on migration, independent geometry, invalid layouts, duplicate
bindings, hardware-plus-control holds, overlapping combos, cancellation and
batch admission. Include compatibility/replay/consent checks for the expanded
sharing capability and actual UIKit editing/hit-testing evidence.

Device checks must cover resizing and moving Space without size changes,
saved layouts in both contexts, simultaneous finger/Pencil use, tap/hold combos
with hardware keys, rotation and safe editor entry/exit. Preserve accepted
input tests; do not repeat qualification unrelated to these changed paths.

Build22 was reported generally working, and hotkeys were already accepted.
Its Space-width limitation is confirmed. The earlier margin report has local
corner evidence but no explicit four-corner physical result; do not claim that
gate complete from general feedback. Focus/lock/background and final Host
release remain separate Pencil source gates.

## Candidate implementation — October 9

The iPad editor and saved library now serve both direct desktop and Pencil
sharing. Each control retains independent size, normalized placement, label,
binding and hold/tap behavior. Space is initially selected for editing. Native
size/position controls supplement drag and corner resizing; the compact inspector
sits beside the selection, with a native sheet on narrow canvases. Editing a
draft sends no remote keys. Done stores a valid library; Cancel preserves the
saved layout. Layout copy/reset and bounded undo are local.

Protocol3 advertises `normalized-pen-keys` and authenticates
`PLANK-NORMALIZED-PEN-KEYS/3`. Its bounded key batches reserve all input edges
before changing ownership. Old identities and v1/v2 consent records are retained;
expanded key authority requires a fresh comparison approval. AVP and native Mac
receivers use the same capability. No Bluetooth sharing or image overlay is added.

The direct input controller merges hardware, software and finger ownership for
all supported keys. A rejected nonempty key batch ends the session and clears
local ownership only after session invalidation; it does not continue with an
unknown held-key state. This is local cancellation, not final Host release proof.

Focused layout/migration/ownership, codec/queue, sanitizer-backed crypto and
consented TCP loopback checks pass. Actual UIKit control hit-testing, two held
controls, cancellation on disable/rotation and layout replacement, and independent
Space width pass in an isolated simulator fixture. Physical editing gestures and
changed direct/shared shortcuts remain device gates. Historical input acceptance
is preserved but does not qualify the new editor or expanded capability.

## Editor and contact revision — iPad24

The user supplied editor/live screenshots and reported that the positions did
not match, Pencil drawing sometimes illuminated controls, and finger-held
controls sometimes released while drawing. These are reported failures, not a
completed custom-controls acceptance.

The editor now fills the screen and previews the last measured live surface.
Both the canvas and the resolved button rectangles use one uniform scale;
drag/resize deltas convert back into live points. Saved button sizes and
normalized centers remain unchanged. Landscape and portrait arrangements use
their measured surface sizes when available, cached separately for Desktop and
Pencil Sharing. Six-point edge/neighbor snapping
supports aligned rows and columns, with resize snapping to neighboring sizes.
The inspector toggle stays within the preview; separate Resize mode keeps
corner hit areas from intercepting ordinary movement.

Shortcut ownership now admits actual direct finger contacts only, using a
snapshot of the binding and touch owner. Pencil contact over a control forwards
the real touch and coalesced samples to the underlying drawing surface without
highlighting or pressing the shortcut. Pencil hover and squeeze use that
surface's coordinates. A finger hold survives drift outside the control;
lift, genuine cancellation, disable, geometry/layout replacement and teardown
still release it. Direct-client Pencil cancellation retires the pen alone,
preserving independently held hardware and custom-control keys. Editing pauses
sharing input and preserves the authenticated peer; background/explicit stop
retain their existing retirement behavior.

Global input retirement advances an input epoch in both iPad paths. The overlay
retires its visible finger/accessibility owners on that same epoch even if its
bounds and layout are unchanged. Stable refreshes retain the current hold.
This prevents a blue Held control from outliving a released remote modifier
after video geometry or remote configuration changes.

The existing version3 protocol, identities, consent, Host and dependency pins
are unchanged. Vision49 and native Mac build 26 remain matching receivers. Physical Pencil
and palm cancellation, saved placement on both live contexts, editor entry/exit,
and final Host release receipt are separate gates. Aggregate contact lifecycle
counts are bounded; typed text, bindings and positions are not logged by them.

## Follow-up — quick visibility and remaining cancellations

The user accepted the improved controls in both contexts, with intermittent
held-button releases still reported. The retained sharing log includes real
UIKit cancellation of admitted direct contacts, without layout, geometry or
explicit retirement. The later direct-client segment contains no corresponding
recorded cancellation. Palm rejection and a system/ancestor gesture remain
possible causes, not established diagnoses. A genuine cancellation continues
to release the hold; silently latching the modifier would change its contract.

Candidate iPad25 makes the grid icon a one-tap Show/Hide Controls button in both contexts.
The direct desktop retains it beside the toolbar restore button when bars are
hidden. Long-press the grid icon for Edit Controls, also exposed as an
accessibility action. Hiding controls releases their owners. No physical
keyboard shortcut is assigned by this slice.

On real admitted finger cancellation, bounded diagnostics record concurrent
Pencil contacts, public touch/ancestor recognizer categories and states, and
scene activity. These are correlations, not an OS cancellation reason. Counts
are saturated and logging is rate limited; no text, binding, touch identity or
coordinates are logged. Cancellation still releases the key normally.

## Naming revision — iPad26

The user reports iPad25's editor and onscreen controls working better, but the
keyboard made naming unusable. The embedded inspector existed only while the
uniformly scaled preview stayed wide enough. A docked keyboard could shrink
that preview past its threshold and remove the focused text field. Local
editor text is not remote input; the direct router remains disabled in editing.

Naming now uses a native Cancel/Save text popup outside preview geometry, from
the active editor or narrow inspector presentation. **Button label** is the
only naming action in the Control inspector. The layout title identifies the
saved arrangement and is renamed explicitly through **Rename Layout** in the
layout menu. Each popup changes the editor draft once; the editor's Done saves
the library and Cancel preserves the previous saved layout.

Changing a key or its modifiers updates a label that still follows the key
name. Duplicating Space and assigning Ctrl therefore names the new button Ctrl,
without changing the original button or either placement. Artist action names
such as Undo remain intact. **Use Key Name** restores automatic naming. Existing
saved labels and the legacy Command glyph are recognized without a storage or
wire migration; generated names stay within the existing 40-character limit.

The label/model checks cover copied controls, custom names, legacy glyphs,
invalid bindings, unchanged geometry and every supported key/modifier name.
The user accepted the installed iPad26 naming revision on October9: “Works
really well.” This is targeted naming acceptance; no new pen, wheel or transport
qualification is implied. Intermittent real finger cancellation remains a
separate open issue. The subsequently reported credential-screen keyboard
occlusion is a separate local login layout issue.

The next image feature is separately planned in
[live image and magnification](ipad-live-image.md): adjustable image intensity,
Fit and true pixel magnification, with a shared image/input mapping. It is not
implemented by the visibility change.

## Transparency and sharing surface revision — iPad28

The user accepted the installed iPad27 credential keyboard revision: “That
works great.” That local login gate is resolved; earlier naming, input and
transport acceptance remains unchanged.

Long-press the Show/Hide Controls grid icon for **Edit Controls…** and
**Transparency…**. The transparency popup provides one persistent global
slider shared by iPad Desktop and Pencil Sharing. It changes control color
layers only. Hit targets, saved geometry and finger ownership are unchanged;
labels remain readable and held controls retain a clear highlight at maximum
transparency. This is artist-control appearance, separate from the planned
live-image overlay.

The reported sharing overlap has a measured cause: the sharing-only pad was
shorter than the whole available client surface, and status/approval content
could shorten it further. Absolute button dimensions remained correct while
normalized centers moved closer together. The actual pad, UIKit overlay and
reported GeometryReader agreed, so this was not an incorrect UIKit scale.
The revised sharing controls and editor use the whole available content
surface, independently of the drawing pad's margins and status content. Pen
coordinate mapping remains owned by the pad. Saved labels, bindings and
placements are preserved.

Sharing instructions now say: “On the PLANK client, choose this iPad in
Settings → Apple Pencil.” Approval and focus instructions also refer to the
client, supporting the existing Vision and Mac receivers. No wire capability,
identity, approval, Host, network or dependency change is part of this slice.

Focused appearance/persistence/contact and actual portrait sharing-surface
component checks pass. Status/approval transitions preserve the control
surface and held finger ownership; resizing that surface still releases held
keys. Pencil coordinates remain relative to the actual pad, and pad resizing
retires the stroke independently. Physical transparency and the reported
sharing layout are not yet accepted; orientation and window sizing remain
device gates. Real finger cancellation remains a separate open issue; this
revision does not latch a cancelled key.

Build28 was freshly compiled, signed and installed as an update on October9
after the user confirmed disconnection. Independent app/process inventories
verify build28 open. Physical acceptance of the new menu and sharing placement
is pending. The component checks include actual portrait geometry and a
controlled landscape-sized production View, with matching editor/runtime
frames, exact eight-point test gutters and foreground approval hit priority.
This does not qualify actual system rotation. Different whole-client window
sizes can still compress normalized positions with fixed point-sized keys;
this revision corrects the sharing-only, status-dependent pad contraction.

## Shared client surface — iPad29

The user accepted iPad28's transparency and client-neutral instructions, but
reported that Pencil Sharing controls still overlap. The prior component
fixture used a layout seeded for its measured content bounds; it did not
establish placement for the user's existing saved library.

A read-only copy of that library establishes the remaining difference. Its
landscape button stacks have six-point gutters on a 1180 × 820-point window.
The sharing screen's 1180 × 756-point navigation content produces overlaps
of approximately three to four points with those same normalized centers and
fixed button sizes. No saved control sizes or positions need to be rewritten
to address this same-window mismatch.

The user confirmed that Pencil Sharing should use the normal client screen
and existing menus. Candidate iPad29 therefore makes it a working-surface
mode, with sharing status, physical comparison approval, pad options and stop
actions outside the artist-control canvas. Desktop and Pencil Sharing use one
full-window control reference, including editor entry before a desktop session.
Visible or hidden local chrome must not shorten that control reference. The
video and Pencil pad retain their own input-coordinate transforms.

Saved libraries, independent point sizes, labels, bindings, transparency and
the version3 authenticated capability remain unchanged. Switching input
authority retires old contacts; editor and options pause input while retaining
the approved peer. Sharing stops when the scene becomes inactive, as before.
Physical placement and the revised menu flow require a new device check.
Intermittent real finger cancellation remains separate.

The native component check loads the exact saved library without rewriting it.
Desktop and Sharing, with local bars shown or hidden, resolve a controlled
1180 × 820-point canvas with the four original six-point gutters and no
overlaps. Their actual editor entry points, including idle entry, use that
same reference. Native window hit tests prioritize settings and comparison
approval over artist controls. Status/consent changes retain held controls;
settings and editing retire input while preserving the peer. Stop and mode
change retire the old source, and Pencil forwarding uses the actual pad's
coordinates. A local presentation mirror also refreshes controls after editor
dismissal without changing the router's admission authority.

This is controlled landscape hosting with production views and stubbed runtime
callbacks. The simulator's native window remained portrait; system rotation,
physical device placement and workstation receipt are not inferred from it.

iPad29 was freshly compiled, signed and installed as an update on October9
after explicit disconnected authorization. Independent inventories verify
the exact build29 application and running executable. A read-only preferences
comparison confirms the saved control library is semantically unchanged.
The user accepted the installed build29 layout and menu flow: “That's looking
great. Functionality is there. Super quick and easy.” This resolves the targeted
saved-layout placement and client-screen sharing check. Actual OS rotation and
final Host release receipt remain separate; they are not inferred from this
acceptance.

## Quick layout access — iPad30

A dedicated stack button opens the saved layout chooser on tap. The selected
layout is marked; choosing another applies it immediately. Touch and hold
offers **Edit Layouts**, which opens the existing editor. The chooser also
includes that action for discoverability, and accessibility exposes the current
layout name and a named edit action.

This button is available in Desktop and Pencil Sharing, including the floating
controls when the local toolbar is hidden, and on the idle client toolbar.
The grid button continues to show or hide artist controls; its appearance menu
retains **Transparency**. Layout editing now belongs to the layout button.

Selecting a different valid layout retires the active source's held keys and
pen contact before publishing the new bindings, then restores existing input
admission. Selecting the current layout does nothing. The desktop session or
approved sharing peer remains connected. Saved layouts, point sizes, mapping,
opacity, input transport and receiver builds remain unchanged. Device tap and
touch-and-hold acceptance are separate from compilation and component checks.

Bounded native simulator checks exercise layout choice, the selected mark,
retirement before publishing new bindings, peer retention, and editor entry in
both working modes with visible or hidden local chrome. The idle chooser caught
a presentation conflict: dismissing the popover and opening the editor in the
same transaction could leave the idle screen visible. Editor entry now waits
for chooser disappearance and a new main-actor transaction. Native replay
confirms a visible editor with usable Cancel; ordinary dismissal and choosing
the current layout never queue editing.

These checks use production root/components with stubbed transport and seeded
contacts. Native simulator taps and presses do not establish physical-device
gestures, VoiceOver custom actions or workstation receipt. The corrected build30
was freshly compiled and signed with the unchanged dependencies; signed build29
is retained for rollback. Build30 is staged, not installed at this checkpoint.

The user authorized installation on October10. Build30 was updated in place
and launched; independent app and process inventories verify version30 and its
exact running executable. Signed build29 remains available for rollback. The
new physical tap chooser and touch-and-hold editor check is next; accepted
build29 placement/menu and previous input qualification are carried forward.

The user accepted the installed build30 layout shortcut on October10:
“Working well.” Its targeted chooser/editor check is resolved. The next editor
refinement is dismissing the selected control inspector on empty-space taps
and while moving/resizing, with normal control taps reopening it. This does
not reopen accepted input, layout placement or sharing qualification.

## Inspector dismissal and reopening — iPad31

Tapping empty editor space clears the selected control and hides its inspector.
Tapping a control selects it and reopens its inspector. The explicit close
button retains the selection, so the control and its resize handles remain
available. Beginning a move or corner resize hides the inspector without
clearing that gesture or changing the selected control.

Only background fill siblings recognize empty-space taps. Foreground controls,
resize handles and inspector widgets keep their own touch priority. In the
narrow inspector sheet, Done or swipe dismissal synchronizes the hidden state;
the selected control or its inspector button can reopen it. Inspector toggles
expose their current Show/Hide state to accessibility.

Dismissal changes transient editor presentation only. Draft layouts, naming,
undo history, bindings and saved point sizes are retained. Actual placement or
resize edits continue through the existing snapping and undo paths. Live artist
controls, layout shortcuts, input admission and sharing transport are unchanged.
The new dismissal/reopening gestures require their own device check.

Focused native simulator checks passed empty-space deselection, control-tap
reopening, explicit close with selection retained, and inspector naming taps
without background interception. Moving and resizing hide the inspector while
retaining selection; a single undo restores the original placement or size.
Narrow-sheet Done and reopening also passed. An initial resize
probe hit the rounded background at the edge; replay inside the existing
44-point handle passed without a source change.

These checks use the production editor, model and store with simulated native
gestures. They do not establish physical Pencil gestures or workstation receipt.
The fresh device build31 is signed and staged, with signed build30 retained for
rollback. The 77 protected source boundaries and the entire non-editor controls
body match accepted30. Build31 has not been installed at this checkpoint;
installation requires the desktop disconnected and Pencil Sharing stopped
because the update closes the iPad app.

The user confirmed disconnection on October10. Build31 was installed as an
update and opened; independent inventories verify version31 and its exact
running executable. Signed build30 remains available for rollback. The next
physical check was empty-space dismissal, control-tap reopening, and unobstructed
move/resize in the editor. The user accepted it on October10: “Perfect.”
Accepted30 shortcuts and previous input, layout and
sharing qualification are carried forward.

## Menu protection and editor continuity — iPad32

After the inspector check, the user reported two separate gaps: artist keys
could overlap menu controls, and entering the editor could drop remote Pencil
input until reconnection. The iPad32 candidate addresses these local boundaries;
the new physical check remains pending.

Editor and live keys use one measured full-window placement policy. The visible
toolbar, safe edges, compact toolbar controls and first-use consent panel are
reserved. Existing point sizes remain unchanged. Touching or snapped groups
translate together when possible; a group that cannot fit is marked **Needs
room** in the editor and is unavailable for live input until made smaller or
repositioned. Passive resolution does not rewrite the saved library. An explicit
geometry edit first captures Undo and materializes the currently displayed
positions in the local draft, so moving a translated key does not jump its
neighbors. Cancel preserves the saved arrangement. An orientation without a
matching menu measurement is unavailable for live keys; the editor asks the
artist to rotate the iPad to preview its menu clearance. Preview content is
clipped inside the preview, and consent reservations end when the panel closes.

Transient scene inactivity now retires input without closing the approved
sharing peer. Input remains paused while inactive or editing; both Done and
Cancel restore admission after dismissal. Backgrounding still stops sharing,
and genuine transport refusal or loss still follows the existing failure path.
Admission uses the latest delivered scene phase, so a retained dismissal callback
cannot reopen input using an earlier active Environment snapshot.
The earlier code stopped sharing for every inactive scene. Whether the physical
editor transition triggered that scene event was not captured, so the reported
disconnect's precise device cause is not claimed. No receiver, Host, identity,
approval, network or dependency change is part of this candidate.

The corrected runtime `387d0d73` passed focused pure geometry checks and four
native simulator cases. The native fixture observes the production root,
editor, pad, publisher, peer, working surface and control views: actual menus
exclude live keys and match the store's geometry, consent reservations clear,
and Done/Cancel retain the approved loopback peer. Inactive dismissal stays
paused; a new active event resumes input and accepts a fresh stroke. Background
still stops the source, and genuine offer refusal remains a failure.

These are controlled simulator events and contacts, with the direct router and
remote Host stubbed. They do not establish the physical editor-disconnect cause,
real OS interruption matrix or final Host release. The corrected iPad32 device
target was freshly compiled, signed and independently verified; signed iPad31
is retained for rollback. Build32 is staged, not installed, and its menu-clearance
and editor-continuity physical check remains pending.

Build32 was subsequently installed and its exact version and running executable
were independently verified. Its new physical check exposed a regression:
editor controls, including copies, stayed still during attempted moves and
corner resizing. Landscape editor Cancel/Done and the idle client toolbar also
overlapped system status indicators. Menu-clearance and editor-continuity
acceptance are therefore still open; build31's accepted inspector result is
retained separately.

## Device editor investigation — iPad33

The current saved seven-control library permits movement and resizing in pure
geometry checks. Native simulator replay also passes moving, copying and
resizing with Undo/Done using the production editor and the complete production
root. Stable native navigation bars sit below the simulator's measured system
safe area. These checks run on iPadOS27; the physical iPad uses26.5. They have
not reproduced the reported physical failure or established its cause.

The candidate gives the root one stable layout container across the idle and
drawing screens. The editor explicitly shows system status indicators and the
presenter does not hide them while editing. This is a compatibility correction
candidate, not a confirmed fix for physical gesture delivery or idle spacing.

Bounded passive diagnostics record anonymous editor gesture callbacks, changed
or unchanged placement proposals, native frame updates and geometry changes
during a gesture. Navigation probes record system/window/controller/bar frames
and unavailable measurements explicitly. Counters do not publish view changes,
intercept touches, retain touch objects or record labels, key assignments,
typed text, identities or artist control coordinates. Saved layout bodies,
snapping, menu protection, input admission and sharing transport remain intact.
The next physical run must distinguish missing callbacks and unchanged draft
proposals, with visible movement checked separately, before any further gesture
or resolver change is selected. The local `frameChanges` counter does not
qualify rendered movement: native replay moved a control while that counter
stayed unchanged. No physical acceptance is claimed for this candidate.

Final runtime `e7c8ecc354ae62890c2607e8735c92d103291fd6` passes the three
scoped native movement/copy/resize cases with the current saved library. Undo
and Done remain usable, and each case publishes its saved draft exactly once.
Nine stable idle/editor toolbar observations place buttons below the measured
system safe area. Anonymous diagnostic payloads were checked in the simulator;
the isolated simulator was shut down afterward. The fresh iPad device build
compiled and was signed as build33. Signed build32 remains available for
rollback. Build33 is staged and has not been installed; the physical26.5 check
for movement, resizing and landscape toolbar spacing remains open.

Build33 was subsequently installed after the artist confirmed disconnection.
Device inventories verify version33 and its exact running executable. The
passive navigation sampler is active. Installation and logging do not establish
physical movement or spacing acceptance; the targeted landscape check remains
open.

The artist subsequently accepted build33 and requested commit/push, resolving
the requested duplicate movement, corner resizing and landscape main/editor
toolbar check. The precise build32 device cause remains uncorrelated; this
acceptance does not qualify editor peer continuity, OS cancellation or final
Host release. Live image reference for Pencil Sharing is the next feature
slice, described in [the image plan](ipad-live-image.md).
