# Native iPad and Pencil sharing checkpoint

October10, 2026: the user accepted the installed iPad31 editor refinement and
requested publication of the completed development work. This checkpoint is
for review; it does not change the shipping platform matrix or release packages.

Subsequent feedback found menu overlap and remote Pencil interruption on editor
entry. The local iPad32 correction is freshly compiled, signed and staged;
its targeted device check is pending and separate from the accepted iPad31
inspector gestures. See
[the current controls boundary](ipad-custom-controls.md#menu-protection-and-editor-continuity--ipad32).

## Review order

The accepted upstream Apple/Mac foundation is
`e8d8536486bf2eb0d7080aab8965f12d7d87b0af`. Optional Mac tablet sharing and
display/wheel changes remain separate dependent reviews:

1. [Mac sharing](https://github.com/cnoellert/plank-client/pull/11).
2. [Mac displays and wheel](https://github.com/cnoellert/plank-client/pull/12).
3. [iPad and Pencil controls](https://github.com/cnoellert/plank-client/pull/13),
   `codex/ios-ipados-readiness`, based on `codex/native-mac-displays`.

The iPad branch was rebased onto the accepted upstream foundation during
development. Publication also merges the original display review tip
`b45921542e9e7224fc6787ee0abc09e2d10e5e8a` into its ancestry. The merge tree
is exactly identical to the accepted pre-merge tree. This preserves both
histories and keeps the stacked review diff specific to the iPad/Pencil slice.
Retarget these slices upstream in dependency order after their bases are accepted.

## Runtime provenance

| Target | Compiled runtime source | State |
| --- | --- | --- |
| iPad32 | `387d0d73ebbbc03fe3aa6506eaa63c3cee5bb83b` | Signed and staged; menu-clearance and editor-continuity device check pending |
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
- Physical native Mac build 26 Pencil reception and the required OS/Host/profile matrix.
- iPhone, registered external-Wacom routes and direct iPad Wacom feasibility;
  the tested PTH-660 produced no direct USB/Bluetooth input.
- Upstream [issue9](https://github.com/instinctual/plank-client/issues/9) trust,
  exact precision, audio/input lifetime and maintained-parent reconciliation.
  Periodic video pauses and the prior Host receive-queue overflow remain open;
  further Host input-backpressure package work remains held.
- [Live image, dimming and magnification](ipad-live-image.md), plus Bluetooth
  Pencil transport, remain separate planned features.

Build and focused check entry points are in the [iPad README](../../ios-native/README.md),
[public native recipe](../../scripts/apple-native/README.md),
[controls record](ipad-custom-controls.md) and [Pencil record](ipad-pencil-relay.md).
