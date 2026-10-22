# Native iPad and Pencil sharing checkpoint

## October18 current state

The installed development stack is iPad47, native Mac build 31 and Vision56. The user
resumed the deferred headset update; Vision53 was installed in place and its
exact version/container/running executable independently verified. It adds
bounded diagnostics, not a latency repair. native Mac build 31 and iPad46 were not restarted.

The later Vision53 trial reports tablet operation working perfectly, with
interruptions on lost focus or accidental iPad disconnection. It also identifies
an unreachable main browser after closing it during a session. Vision54 adds an
explicit Workstations button and a unique preferred browser window; native
reopen workflows and exact signed/installed checks pass. It is installed under
fresh disconnected authorization; the user confirms the main window reopens
and the desktop session stays available. The
reported mouse/keyboard focus-return delay and settings organization remain
follow-ups; input and transport behavior were not changed.

The user then requested the action inside Session Controls. Vision55 moves
**Show Workstations** into its **Window** section, closes the menu on selection
and removes the separate desktop button. Native production-menu checks and fresh
signed/device installation checks pass. The disconnected update is installed;
the user subsequently reports the placement worked great. Vision54's browser
and launch policy remain unchanged.

Vision56 implements the next requested UI refinement: image-sharing
consent and status move from Vision setup to **Session Controls → iPad image**.
The same receiver setter and explicit consent rules are retained, with the
toggle disabled before iPad connection. Two native menu cases and exact signed
artifact/source/dependency checks pass. After the user confirmed the disconnected
headset update, Vision56 was installed and its exact app version/container/running
executable independently verified. Signed55 remains for rollback; the Mac-active
settings source is byte identical and native Mac build 31/iPad46 are unchanged.

The user accepts Vision56's image placement. The next authorized update is
iPad47 (`ecfd6ad0d1a4607efa88cf09e4c0f5764fb69e1d`): foreground Sharing is automatically
ready to receive from its approved client, with image on/off controlled solely
by the Mac/AVP transmitter. The iPad keeps intensity and zoom. An authenticated
image lifetime controls shared proportional mapping; ending it restores the
saved input-only mapping and Fit after retiring held input. The fresh signed
candidate,17 bounded lifecycle cases, native menu and existing contact checks
pass. The authorized in-place update is installed with exact47/0.1.0 and its
container independently verified. After the user unlocked and opened PLANK,
independent process inventory verifies the exact installed47 executable. No
reinstall or relaunch was needed. The user reports the automatic reception is
working nicely; this accepts that ordinary-use trial, without separately
described sender transitions or sustained latency measurements.
Senders/Host/network/pins are unchanged.

A separate Flame-01 AVP startup failure reports the display layout not ready
within 45 seconds. Captured Host startup logs show repeated
`NV_ENC_ERR_OUT_OF_MEMORY` at 5120×2160 with zero video frames sent, alongside
very low available GPU memory. The user changed the AVP virtual display to
1920×1200 and confirms the desktop picture appears. This is a working
lower-resolution workaround, not a restored 5K or sustained-stream gate. See
[the startup assessment](native-mac-integration.md#october18-flame-01-avp-startup-and-lower-resolution-recovery).

Targeted Sharing qualification is complete for local 2× availability through Mac
focus changes, clean Artist modifier release, Fit deferral until actual Pencil
lift, and Hand navigation moving only the local view with drawing resuming.
Direct Pencil alignment and the optional movable map remain accepted. Two Mac
workstation connection losses remain unexplained, including one session with
zero Pencil samples. Sustained latency, genuine OS cancellation, final Host
release receipt and the broader platform matrix remain open. Image promotion
stays held; this is a development review checkpoint, not a release.

Live upstream refs checked October18 remain main
`58f4df097c1cdf11bf6b4b429701f156e2b10109` and Apple integration
`e8d8536486bf2eb0d7080aab8965f12d7d87b0af`, both already ancestors of this branch.
The unpublished suffix was previously rebased after merging main into the
published checkpoint. Its 17 upstream Qt/Linux/test paths remain unchanged from
main; the native recipe retains its explicitly reviewed common-C pin. No new
rebase or dependency change is required at this snapshot.

The user requests publication of the completed work and treats Flame-01's GPU
exhaustion as an operational issue they can handle separately. It does not block
this review checkpoint. The current iPad47, native Mac build 31 and Vision56 work is
collected in [draft PR14](https://github.com/cnoellert/plank-client/pull/14), with
the accepted controls checkpoint preserved in PR13. Recorded component/device
checks and targeted user acceptance accompany the review; no CI status checks
are listed at the publication snapshot. This remains a development review,
with sustained reliability and the other explicit limits retained.

## Development history

October10, 2026: the user accepted the installed iPad31 editor refinement and
requested publication of the completed development work. This checkpoint is
for review; it does not change the shipping platform matrix or release packages.

Subsequent feedback found menu overlap and remote Pencil interruption on editor
entry. Development build33 was accepted for duplicate movement, corner resizing
and landscape main/editor toolbar clearance and published in the existing draft
PR13 at `c3dfc42bb4272ab18616e022b441fdbbe9497bc1`. Editor peer continuity remains
a separate physical gate. See
[the current controls boundary](ipad-custom-controls.md#menu-protection-and-editor-continuity--ipad32).

The authored editor aspect and editing check from42 remain accepted. Local
Fit/2× operating-view build43 is now installed and open on the new M5 after
the user's disconnected installation authorization. App inventory and its exact
running executable verify43/0.1.0; physical zoom qualification remains pending.
The subsequent user trial reports severe intermittent Pencil latency: normal
speed for a period followed by very slow response. This is not an accepted43
result. Retained logs and an unsynchronized, largely idle device profile do not
yet identify the delayed stage or establish that zoom caused it. Earlier editor
and input acceptance remains recorded; new zoom qualification is held during
the investigation.

The failure was in Pencil Sharing through Vision52 to Flame-01. Disabling the
reference image initially recovered drawing, but the user's later image-on and
image-off trials both worked normally; enabling AVP Tailscale made no difference.
These observations do not establish an image or Tailscale cause. A read-only
Host snapshot identified Flame-01 package1.1.024, with active services and a
normalized tablet present; unavailable logs do not prove a healthy input queue.

Diagnostic candidates44/53 add bounded, content-free mailbox, local send
completion, receiver scheduling and native submission counters using existing
execution paths. They do not change input/media behavior or dependency pins and
are not a latency repair. Local send completion and QUIC handoff are not Host
receipt. Fresh device compilation, focused diagnostic checks and independent signed-package
verification pass; candidates44/53
are staged, not installed. The current43/52 session remains untouched. The later
connection through Tailscale also worked with the image both on and off.

The next iPad45 UI slice makes the zoom map optional and movable: the View menu
has Show Zoom Map and Reset Map Position; a separate header moves it and × hides
it. Both iPad modes share persistent local visibility/placement. No receiver or
Host change accompanies this slice. Candidate persistence/native view/menu checks and independent signed-package
verification pass;45 was installed as an update on the M5 with its exact running
executable independently inventoried. The user accepts hide/button access, menu
restore and header movement in Desktop and Pencil Sharing. Vision52 is unchanged.

The user-authorized local Mac switch now installs and opens native Mac build 30 from
`dd07decfa5f1a26a664cc699d3cfce2ba725b5ae` at `/Applications/PLANK Native.app`,
retaining the previous Qt client. Fresh public compilation, independent artifact
checks and installed/running identity checks pass. It includes the bounded shared
diagnostics newer than staged native Mac build 29. The user confirms the targeted Mac Pencil
and Artist modifier smoke check is working; optional image sharing remains
pending. See [the Mac switch](native-mac-integration.md#october-14-local-native-client-switch).

The user subsequently reports the direct iPad45 2× trial stays under the Pencil.
The direct check is resolved; Sharing image/zoom/Hand and held-contact/modifier
view changes remain separate physical checks. See [the zoom feedback](ipad-live-image.md#october-14-direct-zoom-feedback).

On October15, the user reports native Mac Sharing hover, click/drag and basic
zoom working, after a one-to-two-second startup delay. The current input question
is resolved. Later targeted checks accept clean Artist modifier release, Fit
deferral until actual Pencil lift and local Hand pan with drawing resumption;
the prior transport failure has no confirmed cause or repair. See
[the current Mac result](native-mac-integration.md#october-15-targeted-input-recovery).

## Review order

The accepted upstream Apple/Mac foundation is
`e8d8536486bf2eb0d7080aab8965f12d7d87b0af`. Optional Mac tablet sharing and
display/wheel changes remain separate dependent reviews:

1. [Mac sharing](https://github.com/cnoellert/plank-client/pull/11).
2. [Mac displays and wheel](https://github.com/cnoellert/plank-client/pull/12).
3. [iPad and Pencil controls](https://github.com/cnoellert/plank-client/pull/13),
   `codex/ios-ipados-readiness`, based on `codex/native-mac-displays`.
4. [Live reference image, operating zoom, editor refinements and diagnostics](https://github.com/cnoellert/plank-client/pull/14),
   `codex/ipad-pencil-image-overlay`, based on `codex/ios-ipados-readiness`.
   This separate draft preserves PR13's accepted checkpoint and includes the
   upstream refresh described above.

The iPad branch was rebased onto the accepted upstream foundation during
development. Publication also merges the original display review tip
`b45921542e9e7224fc6787ee0abc09e2d10e5e8a` into its ancestry. The merge tree
is exactly identical to the accepted pre-merge tree. This preserves both
histories and keeps the stacked review diff specific to the iPad/Pencil slice.
Retarget these slices upstream in dependency order after their bases are accepted.

## Runtime provenance

| Target | Compiled runtime source | State |
| --- | --- | --- |
| iPad47 | `ecfd6ad0d1a4607efa88cf09e4c0f5764fb69e1d` | Automatic foreground Sharing image readiness; transmitter-only image choice; authenticated image geometry/zoom lifetime with ordered input retirement; built/signed/verified and installed with exact47/0.1.0/container independently checked; exact running47 executable independently verified after unlock; user reports ordinary-use reception working nicely, without a separate sustained-latency result |
| Vision56 | `5fdf2d897f9324e8a40396c3012afd1bfebc9f7c` | Installed/open after disconnected authorization; image sharing moved into Session Controls > iPad image with unchanged consent/receiver and Mac-active UI; two native cases, artifact/dependency checks and exact installed app/process verification pass; user accepts menu placement; sustained image/latency remains separate |
| Vision55 | `6e8248816c6f1acb3230c24b04bd0a977d217321` | Installed/open under disconnected authorization; Show Workstations moved to Session Controls > Window and standalone button removed; production-menu native check and artifact/installation checks pass; user reports placement worked great; input/transport unchanged |
| Vision54 | `2405b065ba0f679df432c29e9514e6245153a5f4` | Installed/open after disconnected authorization; explicit Workstations reopening, single preferred browser and secondary launch suppression; two native reopen cases and package/install checks pass; user accepts main-window reopening with the desktop session still available; no automatic reactivation or mouse/keyboard focus repair claimed |
| native Mac build 31 | `f82c2a4dfdd37782ac4baeed46f313a1664c8baf` | Installed native client with Settings window pause lifetime correction and Input Details; focused AppKit and signed/runtime checks pass; targeted Sharing view checks accepted, controlled Settings repair and sustained transport reliability not established |
| iPad46 | `4c7731272c68f53b4272f959ca695d1803b7080d` | Installed on M5; local Sharing zoom remains available while remote input is paused; user accepts preserved 2× selection, clean modifier release, held-contact Fit deferral and local Hand/resume |
| Vision53 | `fe965cf1d4f5c9a1502265ecea7ea8768425a30b` | Installed and launched October18 after resumed authorization; exact signed package/current source and independent installed app/process checks pass; bounded diagnostics only, no latency repair or physical diagnostic qualification claimed |
| native Mac build 30 | `dd07decfa5f1a26a664cc699d3cfce2ba725b5ae` | Fresh native build with bounded shared diagnostics; signed and independently verified, installed/open alongside retained Qt client; user confirms targeted Pencil tap/drag and Artist modifier smoke check; reference-image sending and broader qualification pending |
| iPad45 | `9dcde0af10dd11f6259d0d1bee088dff4f345466` | Optional movable zoom map with shared remembered preferences; focused persistence, native routing/menu and independent package gates pass; installed/open on M5 with exact45/running executable verified; targeted hide/button access/restore/header movement accepted in both modes |
| iPad44 diagnostic candidate | `fe965cf1d4f5c9a1502265ecea7ea8768425a30b` | Focused checks and fresh device compile/signing pass; iPad44 remained staged and its diagnostics carry forward in installed46; the corresponding Vision53 is now installed as recorded above; no latency repair claimed |
| iPad43 | `5a2ea686a3bab92df91af69c17a4ca4de1e31dca` | Shared local Fit/2× crop, matching image/cursor/input, Hand and fixed-footprint navigator in direct Desktop and Sharing; policy/contact, native rendering and independent signed package checks pass; installed/open on M5 with exact43/0.1.0 and matching running executable observed; physical zoom check pending |
| iPad42 | `cdfbe5f004019c8e67102cc6d83a0cc6b3f76cbb` | Rebased after Alan main refresh; editor outline, keys, labels, guides and gesture inversion share the authored portrait/landscape canvas; whole rendered-editor checks, old41 tall-entry negative control and independent package review pass; installed/open on M5 with exact42/0.1.0 and running executable observed; user accepts requested physical outline/arrangement through window resize and selected control movement/resizing |
| iPad41 | `910c52fff4264c42a5fb666b1ed01aedb2f346f9` | Installed/open on M5 after authorization, exact41/0.1.0 and running executable observed; physical fixed-canvas gate failed: tall editor window keeps a tall outline around fitted landscape keys; selected-edit gate not separately answered |
| iPad40 | `44f29cebd5a97290bccffcdc331e59a3a328234f` | Drawing-only editor preview chrome correction; installed/open on M5 after disconnected authorization, exact40/0.1.0 and running executable observed; reserved-band check superseded by artist-approved whole-canvas policy41 |
| iPad39 | `549bd8e4fef1f8cfdc46fc2624eb352fdbc44957` | Installed/open on M5; user accepts fullscreen naming and general button scaling/placement; smaller-window reserved top/bottom bands remain visually odd |
| iPad38 | `000c72132e9d21c5bbff8d06443e1ad7fe0fb966` | Installed on new M5; artist reports fullscreen connected naming still fails and smaller-window controls scale incorrectly; physical naming gate failed |
| iPad37 | `535c48667d35bafcd322f692ae07703d579040cc` | Local inline layout rename and chrome-safe editor rows; installed/open on new M5 after disconnected authorization, exact version/running path verified; top-row snap accepted, connected Rename still fails/switches back |
| iPad36 | `7a5fe82d14f47c073e308af63a73ac6c6cb42014` | Built, signed and installed/open on new M5; exact version/running path independently verified; physical finger-filter acceptance pending |
| Vision52 / native Mac build 29 | `e8b928ffd8b253edc959c7669b2cc23b50aa99f2` | Image-only recovery: Vision52 installed/running; artist reports picture visible with better latency, below expectations; image promotion held; native Mac build 29 staged/unlaunched |
| iPad35 / Vision51 | `a102d8c8460fc851b4c55e84fc9acdafda17a3a9` | Optimized conversion and proportional shared image/Pencil mapping: iPad35 installed/open and independently verified on new M5/iPadOS26.6; Vision51 staged; physical image latency/alignment unqualified |
| native Mac build 28 | `a102d8c8460fc851b4c55e84fc9acdafda17a3a9` | Signed and independently verified, staged and unlaunched; physical Pencil reception/image sending pending |
| iPad34 / Vision50 | `4a14a47e8f8fb62413f4a8c60f85fb5ecf72329a` | Separate dimmable reference-image slice: installed and exact running executables independently verified; user rejected picture latency/stretching as unsuitable, image promotion held |
| native Mac build 27 | `4a14a47e8f8fb62413f4a8c60f85fb5ecf72329a` | Signed and staged; physical Pencil reception and image sending pending |
| iPad33 | `e7c8ecc354ae62890c2607e8735c92d103291fd6` | Signed, installed and independently verified; targeted move/resize and landscape toolbar clearance accepted |
| iPad32 | `387d0d73ebbbc03fe3aa6506eaa63c3cee5bb83b` | Historical installed candidate, superseded by33; signed rollback retained |
| iPad31 | `1a788801d8e48ebb3498e3c029ddee2d44afe227` | Signed, installed, exact running executable independently verified; targeted editor acceptance recorded |
| Vision49 | `0a71d0c0cef026e74844dde495e186f799b32068` | Signed and installation verified; subsequent Pencil sharing accepted by the user |
| native Mac build 26 | `0a71d0c0cef026e74844dde495e186f799b32068` | Signed and staged; physical Pencil reception pending |

The original iPad31 publication recorded history and documentation only.
iPad32 contains the subsequent local menu/admission correction; signed iPad31
is retained for its rollback. Signed iPad30 is also retained. The public recipe and dependency pins are unchanged
from the compiled candidates; no Host, network or app identity change accompanies
publication. Private signing profiles, device receipts and logs are excluded.

## Included behavior

- Isolated single-display iPad client: playback, audio, login, saved workstation,
  aspect-fit modes, mouse/pointer input, physical wheel and hardware keyboard.
- Direct normalized Apple Pencil pressure, navigation, hover where supported,
  and lifted-tip squeeze right-click; Apple/PC mapping for received keyboard keys.
- Native live software keyboard without a local typing preview. The tested
  full-size Pencil-held setup uses **Settings → Apple Pencil → Scribble off**.
  Credential and control-label editing remain local native forms.
- Foreground authenticated local-network Pencil publisher with separate physical
  comparison/approval, normalized pen records and bounded allowlisted shortcut
  batches. Vision and Mac receivers select one tablet source; this does not
  fabricate a raw Wacom identity.
- Normal client-screen Pencil Sharing, neutral charcoal pad, independent margins
  and mapping choices. Saved artist controls serve Desktop and Sharing together.
- Full control editor: position, dimensions, edge/neighbor snapping, key/combo
  binding, generated or custom labels, layout library and undo. Quick layout
  selection, touch-and-hold editing, visibility and transparency are in the toolbar.
  Empty-space taps dismiss the inspector; tapping a control reopens it; moves
  and resizes hide it without dropping selection or gesture state.
- Candidate36 adds persistent **Ignore finger input** for remote desktop
  finger clicks/drags. Artist controls, Pencil and physical peripherals remain
  available; local controls/forms retain their touch behavior.

## Recorded evidence

Direct iPad playback, mouse, supported resolution changes, Pencil pressure and
navigation, Space during Pencil drawing, squeeze right-click and physical-wheel
scrolling are accepted. Function keys are accepted with Fn held on a separate
Bluetooth Magic Keyboard; ordinary top-row brightness/media keys remain handled
by iPadOS. The observed Flame cursor offset resolved with Flame Tablet Margins
disabled; no client coordinate compensation was added.

Pencil sharing tip/drag/pressure, desktop reconnect, fresh strokes after rotation,
pad options and simultaneous finger shortcuts/Pencil drawing are accepted.
Subsequent user checks accepted naming, credential visibility, transparency,
shared placement/menu flow, layout shortcuts and the iPad31 inspector refinement.
These are targeted development passes, not the full platform/interruption matrix.

New M5 installation of35 is independently verified; it does not transfer the
older device's complete hardware qualification. Candidate36 checks execute the
production router and contact bodies with recording Host/stand-in touch delivery,
covering persistence, finger-drag release and stale callbacks, re-enabling input
and preservation of Artist/hardware keys, Pencil and physical mouse/wheel. Its
native device app compiles; actual OS touch delivery and Host receipt are pending.

Focused layout/migration/contact/ownership, wire/atomic admission, sanitizer
crypto and consented TCP loopback checks passed during development. Fresh target
builds and signing/profile checks passed for the candidates above. The iPad31
native editor fixture passed foreground priority, dismissal/reopening,
move/resize and one-step undo restoration. Its 77 protected source boundaries
and complete non-editor controls body match accepted30. Simulator injected
gestures do not establish physical Pencil or Host receipt; narrow-sheet swipe
was source reviewed only.

The iPad32 pure geometry checks passed menu exclusion, grouped translation,
unchanged point sizes, recoverable unavailable controls and exact Undo of
explicit draft geometry edits. Four focused native simulator cases passed
production-root admission through actual editor Done/Cancel, injected inactive
and background transitions, and genuine offer refusal. An authenticated loopback
peer remains approved across editing and admits a fresh stroke after resumption.
Actual visible/compact toolbar and consent rectangles match cached/editor
geometry and live control frames; the consent reservation clears after approval.
A preinstallation check caught and corrected an older Environment snapshot
reopening input during inactive dismissal. The corrected runtime was freshly
rebuilt and signed; the earlier candidate was never installed. Injected scene
events and simulated contacts do not establish the reported physical cause or
final Host receipt. The targeted physical iPad32 check remains pending.

## Separate remaining work

- Genuine finger cancellation and held-input release; focus/lock/background and
  final Host receipt. Local retirement or sender submission is not Host proof.
- Physical custom-control OS rotation/window sizing and four-corner pad margins;
  latency, remote tilt and continuous/horizontal scrolling.
- Native Mac build 30 optional image sending, latency under load, interruption/final Host receipt and the required OS/Host/profile matrix.
- iPhone, registered external-Wacom routes and direct iPad Wacom feasibility;
  the tested PTH-660 produced no direct USB/Bluetooth input.
- Upstream [issue9](https://github.com/instinctual/plank-client/issues/9) trust,
  exact precision, audio/input lifetime and maintained-parent reconciliation.
  Periodic video pauses and the prior Host receive-queue overflow remain open;
  further Host input-backpressure package work remains held.
- [Live reference image and dimming](ipad-live-image.md) remains a separate
  development slice. Installed iPad34 / Vision50 failed the picture usability
  trial. iPad35 is installed on the new M5; its missing-picture report is not
  isolated by current sender/receiver status yet. Vision52 / native Mac build 29 add recovery
  from the reproduced startup latch defect; source fixtures pass but candidates
  are installed on iPad/Vision; native Mac build 30 is now installed with the same image behavior
  and newer bounded diagnostics. Image responsiveness remains below artist expectations
  and promotion is held. Local Fit/2× is implemented, with separate physical qualification
  pending; sharper region media and Bluetooth Pencil transport remain later work.

Build and focused check entry points are in the [iPad README](../../ios-native/README.md),
[public native recipe](../../scripts/apple-native/README.md),
[controls record](ipad-custom-controls.md) and [Pencil record](ipad-pencil-relay.md).
