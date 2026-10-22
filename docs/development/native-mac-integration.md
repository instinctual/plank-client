# Native Mac integration slices

Tracking: [upstream coordination issue 9](https://github.com/instinctual/plank-client/issues/9).

## October18 current checkpoint

Native native Mac build 31 and iPad46 retain the targeted Sharing operating-view acceptance
recorded below: local 2× selection survives Mac focus changes, Artist modifiers
release cleanly, Fit waits for actual Pencil lift, and Hand pans only the local
view before drawing resumes. The two workstation transport failures remain
unresolved; these checks do not establish sustained reliability or final Host
release receipt.

The user resumed the deferred AVP update on October18. Existing signed Vision53
from `fe965cf1d4f5c9a1502265ecea7ea8768425a30b` was reverified, installed and
launched. Independent app and process inventories confirm build53 and its exact
executable. It adds diagnostics, not a latency fix. The Mac/iPad apps, Qt fallback,
Host, network settings, dependency pins and identities were unchanged.
The subsequent user-accepted AVP tablet trial also reports the closed-browser
recovery gap and an occasional mouse/keyboard focus-return delay. Vision54 is now
installed with an explicit Workstations reopening button; the user confirms it
reopens the main window while the desktop session stays available. native Mac build 31 and
iPad46 remain unchanged. See the
[Vision browser recovery boundary](ipad-pencil-relay.md#october18-avp-trial-and-browser-recovery).
Vision55 subsequently moves that action into **Session Controls → Window → Show
Workstations**, with the standalone button removed. It is installed and verified;
native Mac build 31, iPad46 and their input/transport behavior remain unchanged.
The user accepts that placement. Installed Vision56 then moves image-sharing
consent/status into Vision Session Controls; the Mac-active settings source is
byte identical and no native Mac update is involved.

### October18 Flame-01 AVP startup and lower-resolution recovery

The AVP56 trial on Flame-01 reports **The workstation display layout did not
become ready within 45 seconds**. This is the native display/stream startup
deadline after PLANK authentication. The saved authentication timestamp and
requested 5120×2160 at 60 fps correspond in time and geometry with a captured
Host startup that repeatedly failed HEVC NVENC initialization with
`NV_ENC_ERR_OUT_OF_MEMORY`. That session sent zero video frames. The correlation
does not include an independently captured unique AVP peer identifier.

A read-only GPU snapshot reports an RTX 5000 Ada with 32 GB, 687 MiB available,
and Flame using 28,620 MiB. Together with the allocation errors, this strongly
supports GPU memory pressure during encoder startup. The exact allocation
mechanism and a memory leak are not established. The Host is the observed
1.1.024; neither its service nor Flame was restarted, and network settings,
packages and pins were unchanged. Bounded filtered Host log reads used the
existing noninteractive read permission; no maintenance was performed.

The user changed the AVP **Flame-01-ts → Virtual display → Resolution** to
**1920×1200**, saved and reconnected, then confirmed the desktop picture appears.
Retain that as the working recovery setting. A 5K retry should wait for GPU
headroom; this trial does not qualify sustained performance or explain the two
October15 Mac transport failures. No additional restart or resolution test is
required for this checkpoint.

## Pencil Settings lifetime correction — installed31

After iPad46, the user reports a live picture but no Pencil cursor motion,
including an explicit foreground/Hand-off/fresh-contact check and a reselected
Pencil peer. Physical input qualification remains blocked.

A bounded offscreen AppKit test using the production Settings appearance
modifiers reproduces a stale pause: the window closes, retained SwiftUI content
does not disappear, and its input owner stays held. This establishes a local
lifecycle defect, not the exact cause of the live failure. Candidate31 ties the
Settings owner to native window visibility/close/detachment and restores it on
reopen. Ten native checks pass actual close/hide and independent owners; the
background fixture models reopen's key notification rather than activating a
real Settings scene.

Mac-only **Input Details** shows source/support, desktop focus, local control
pause count and bounded received/queued counts. Counts reuse the existing
two-second report and do not establish final Host receipt. Pen/Artist admission,
packet ordering, image transport and dependency/Host pins are unchanged. The
shared-file additions are compiled only for native Mac; the non-Mac active
source remains identical. The targeted physical Settings-close/Pencil-resumption
check remains separate from the native component checks.

Runtime `f82c2a4dfdd37782ac4baeed46f313a1664c8baf` is freshly compiled and
signed as native Mac build 31. Exact payload
parity across the executable and four FFmpeg libraries, signature/entitlements,
298 dependency entries, native tests and protected delivery bodies pass separate
verification.

The user authorized the disconnected update on October 15. native Mac build 31 is now installed
at `/Applications/PLANK Native.app` and opens normally to the workstation browser.
Independent installation checks confirm the exact signed bundle, running
executable and captured startup stdout/stderr. The original installed native Mac build 30 is
preserved for rollback; the Qt client, iPad46 and Vision52 are unchanged. No
preferences, approvals, network settings or Host packages were changed. Physical
The first physical Settings-close trial still reported no cursor movement.
Captured Mac diagnostics show zero incoming Pencil/Artist application messages
during that trial; they do not establish the iPad-side cause. **Input Details**
distinguishes admission state from received and locally queued samples.

The Mac's optional image consent was off after restarting the app. Restoring it
produces image encoding and a sending status, which does not establish picture
receipt on the iPad.

The user subsequently reported operation working again. Captured Mac diagnostics
corroborate incoming Pencil samples and local input submission without a local
queue backlog. A new peer diagnostic lifetime precedes that recovery; its trigger
was not observed. The separate Mac-to-workstation transport reports delay/lost
packets, but neither those cumulative counters nor the recovery establish the
cause of the earlier missing iPad input. No further runtime change or controlled
Settings repair is claimed. Image, stable zoom/Hand, held-input and final Host
receipt qualification remain separate; the now-obsolete iPad screenshot is not
required.

### October 15 connection loss during dragging

A subsequent native Mac build 31 session failed after about ten minutes with the native data
receiver reporting `Protocol error: connection lost`. The app stayed running.
In the final eight seconds, the Mac-to-workstation transport's reported RTT rose
from about 20 ms to 2.37 seconds. At failure, local input queue depth and native
submission backlog were zero, with no failed input submission. The approved iPad
peer continued exchanging heartbeat traffic after the workstation session ended.

This distinguishes the failed workstation endpoint from the surviving Pencil
peer, but does not identify network, Host or client root cause. Cumulative loss
counters are not a measured loss rate, and an empty local queue does not exclude
a Host receive-queue problem. A bounded unprivileged Host check found services
still running without a service restart; detailed Host logs were unreadable.
The normal Reconnect action reached sign-in, which is left to the user. Sustained
stream and remaining Sharing qualification are held; Host packages, network
settings and all device installations remain unchanged.

A later desktop session lasting about 22 minutes also failed, this time reporting
the native audio receiver's connection loss with zero normalized Pencil samples
in that session. Local queues were again empty. This establishes that a desktop
disconnect can occur without Pencil traffic; it does not identify its cause.

### Pencil discovery after loss of the USB link

With the iPad foreground pad still showing Stop Sharing, both native Settings and
a fresh eight-second browser saw no Pencil advertisement. Restarting Sharing
once did not restore it. Current USB inventory and interface checks showed the
previously present iPad device and USB network interfaces were absent.

After the user reconnected USB, the device and wired diagnostic tunnel returned.
The Pencil service immediately reappeared on those USB interfaces, and selecting
it in native Settings connected with the existing approval. No app, Host,
dependency, identity or network setting changed. This recovers discovery in that
setup; the actual Pencil transport interface and guest Wi-Fi policy were not
measured. It does not resolve the independent workstation stream failures.
The user then confirmed fresh Pencil hover and tap work through the restored
connection. The subsequent iPad46 Sharing check also confirms local 2× View
activates while the Mac chat is foreground and remains selected when PLANK
Desktop returns. The user also confirmed an Artist modifier releases cleanly
after drawing and a Fit request waits for an actual held Pencil contact to lift.
The final Sharing Hand check confirms local panning without remote clicks/drags
and drawing resuming when Hand is turned off. Accept those targeted results;
genuine OS cancellation, sustained streaming and final Host receipt remain
separate checks.

## October 14 local native client switch

At the user's request, native Mac build 30 from
`dd07decfa5f1a26a664cc699d3cfce2ba725b5ae` is installed and open at
`/Applications/PLANK Native.app`. The previous Qt client at
`/Applications/PLANK Client.app` remains available. Fresh public compilation,
independent signed payload/library/dependency verification and exact installed
version/running-executable checks passed. Startup stdout and stderr are captured
in private diagnostic logs; no local client was running before the switch.

The native client retains `la.instinctual.PLANK.NativeMac`, its existing settings
namespace and the pinned native recipe. It opens idle until the user connects;
no Qt credentials or Pencil approvals were copied. native Mac build 30 carries the four shared
bounded diagnostic source updates since staged native Mac build 29, with no input/media behavior
change. The developer build is signed with hardened runtime but is not a notarized
release. Normal launch succeeded without changing Gatekeeper or quarantine policy.

native Mac build 24 display, fullscreen, Wacom, wheel and middle-button acceptance carries
forward. The user subsequently confirmed the requested Pencil tap/drag and
Artist modifier smoke check through the Mac is working. This is targeted
user-reported acceptance; optional reference-image sending, latency under load,
the OS matrix and final Host receipt remain separate gates. iPad45's
hideable/movable map is accepted in both modes; Vision52 and the working
network/Host/dependency pins are unchanged.

### Subsequent Sharing qualification failure

The later Sharing trial reported no iPad image or Pencil cursor. The Mac then
reported a terminated video transport (`native=-3`, state8, connection lost).
This failure held Mac physical qualification at that checkpoint; the earlier
brief user confirmation did not resolve it. The existing signed
native Mac build 30 was reopened with captured startup logs to diagnose the desktop stream
without Pencil or image sharing. No Host, network, recipe or app runtime changed.
No transport, image or input repair is claimed.

### October 15 targeted input recovery

With iPad45 and native Mac build 30 unchanged, USB attached and only guest Wi-Fi available,
the user reports Pencil hover, click/drag and basic Sharing zoom now work on
Flame-01 reached through Tailscale. Input took a reported one or two seconds
to come to life. This confirms targeted operation in that trial, not a measured
startup delay or a repair of the prior video transport/latency failure. The
actual Pencil transport interface was not measured. Held modifiers and view
changes during an active stroke remain the next physical gate; Sharing Hand
isolation, sustained load and final Host receipt remain separately unqualified.

## Baseline and review order

The Vision foundation and transport correction are merged into Client
`apple-native-integration` at `1953cc11a4b281f24286c11301d10fc76601a972`.
Shipping main is a separate baseline. These Mac contributions build on that
integration merge, at the existing paths, as a separate native pilot application.
The eventual `apple/shared` directory move remains a mechanical follow-up.

| Slice | Branch | Original checkpoint retained as an ancestor | Runtime compile source |
| --- | --- | --- | --- |
| Native Mac foundation | `codex/native-mac-foundation` | `da0bb7c3575281b49cb63dfcb90f572cc5f1aeab` | `a30f08e` |
| Optional authenticated USB tablet sharing | `codex/native-mac-tablet-sharing` | `8109c8d6148132af1855a5395c1c1bd20ab29577` | `16ef937` |
| Display geometry, session controls and physical wheel | `codex/native-mac-displays` | `04675c95c9486e8f22d7587348d705bb44aeb1a4` | `fd844b5` |

Each selective history merge keeps original commits/authorship while importing
only the chosen native files and narrow shared seams. The final native tree
matches the accepted Mac build-24 publication, apart from the integration
Vision README. The shared physical Wacom worker is deliberately reconciled
with the maintained integration worker rather than replaced with its older
pilot copy. Build receipts distinguish compiled runtime from later documentation.

The foundation targets upstream `apple-native-integration`. The other two
branches form a dependent stack: sharing targets foundation; displays targets
sharing. Until their bases exist upstream, their review PRs live in the fork.
They must be retargeted for upstream integration in dependency order. A fork
review is not a merge into Alan's maintained repository. The original Mac
PRs remain historical checkpoints, not competing implementation targets.

Review entry points: [upstream foundation PR 11](https://github.com/instinctual/plank-client/pull/11),
[fork sharing PR 11](https://github.com/cnoellert/plank-client/pull/11), and
[fork display/wheel PR 12](https://github.com/cnoellert/plank-client/pull/12).
The foundation merged upstream on October8 at
`e8d8536486bf2eb0d7080aab8965f12d7d87b0af`. Live status checked October10:
sharing and display/wheel remain fork drafts awaiting review in dependency order.
The [iPad/Pencil checkpoint](ipad-client-checkpoint.md) is the next stacked
review slice; it retains the original display tip as an ancestor so its diff
does not duplicate those Mac changes. No shipping branch or package is promoted.

## Boundaries

- The existing Qt/SDL application remains the shipping application. Foundation
  changes its shared Wacom worker only to allow an injected sender and a native
  compile boundary; its maintained permission/presence APIs remain intact.
- Sharing adds a physical worker-owned capture lease, durable Relay generations,
  authenticated TCP drawing/management, explicit local approval and opt-in UI.
  Shipping capture participates in the lease; its permission request remains
  with its existing launcher. This shared-worker change needs shipping CI review.
- The native application remains `la.instinctual.PLANK.NativeMac`, Apple Silicon,
  macOS 15 minimum and SDK 27 build. It has its own bookmark namespace. Mac
  sharing supports USB Wacom and TCP to AVP; it does not advertise Mac Bluetooth
  capture, Mac-to-AVP Bluetooth, or Linux administration/decoded Setup preview.
- Display placement uses the existing Host topology/layout contract. No Host
  contract extension or package is introduced. Global session disconnect and
  fullscreen controls coordinate both windows; native green-button operation
  can remain specific to one window.
- Transport, crypto and Relay codecs remain in their owning repositories with
  the merged public recipe's explicit pins. No new protocol is copied into
  Client. Current-parent trust, precision, audio/input and CI reconciliation
  remains upstream-led through issue 9.

## Build and component evidence

All three runtime slices passed fresh public input preparation, dependency
compilation and unsigned Mac application compilation with integration common-C
`036df96f2d1577af7a1b08c05d87a5218fff7c9b`. Use the
[public recipe](../../scripts/apple-native/README.md), choosing a fresh work
directory for each platform. Signed release staging remains separate.
The final stack also passed a fresh unsigned Vision device compile at
`1bcb595a6f8a9156490186e513a5304fdb072e04` (documentation-only successor to
`fd844b5`), checking the shared session/input/topology source against SDK 27.
It was not installed or launched on a headset.

| Check | Foundation and sharing | Displays |
| --- | --- | --- |
| Input/focus policies | 552 | 568 |
| Cursor/fullscreen composition | 19 | 28 |
| Local controls/fullscreen delegate | 40 | 88 |
| Production C wrapper with Swift foreign-thread callback | 266 | 266 |
| Decoder recovery | 137 | 137 |
| Multi-display policy | Not imported | 86 |

Worker lifetime/barrier/epoch/backlog and tablet outage-policy suites also
passed. Sharing passed 55 managed Setup codec checks, 1,791 raw protocol,
enrollment/generation/lease checks, and real loopback socket tests for fragmented
discovery, ordered raw reports, bidirectional control and malformed public
metadata refusal. Hardware workers are faked in these component suites; there
is no physical tablet or OS Space-animation acceptance claim.

Run focused suites from the corresponding Client checkout:

```sh
bash scripts/test-macos-native.sh "$PLANK_NATIVE_TEST_OUT"
PLANK_RELAY_SOURCE_DIR="$PLANK_NATIVE_WORK/git/raw" \
PLANK_MANAGED_RELAY_SOURCE_DIR="$PLANK_NATIVE_WORK/git/managed" \
PLANK_RELAY_SODIUM_PREFIX="$PLANK_NATIVE_WORK/install" \
  bash scripts/test-macos-relay.sh "$PLANK_RELAY_TEST_OUT"
```

Relay tests apply to the sharing/display slices. Use a different test output
directory per slice. The app bundles are unsigned compile artifacts with local
shared FFmpeg references, not relocatable, notarized release packages.

## Acceptance carried forward and new gates

The original Mac build-24 runtime is `5db90ffeb7568105a664df0f340c5769c79b0603`;
publication is `04675c9`. Carry its targeted primary/secondary geometry,
session/window controls, mouse/pen, physical wheel and balanced middle-button
acceptance. Carry build-16 Mac sharing performance, background focus,
reconnect, USB return on the accepted Flame4 pressure policy, and Mac sleep/wake
evidence from [the tablet-sharing checkpoint](plans/macos-tablet-relay.md).
Earlier Flame3 hover-only USB return and intermittent Wacom startup are explicit
observations, not erased by later passes. Historical Linux Relay tests do not
qualify Mac hardware behavior.

New physical acceptance should target changed inputs and worker reconciliation:

1. With the accepted Host pressure policy verified, connect tablet-first; check
   hover, tip pressure, clicks, held drag and buttons before and after reconnect.
2. Verify local permission behavior and exclusive ownership between direct Mac
   capture and opt-in sharing, including another cooperating process. Stop/quit
   must return ordinary local tablet input. No unknown network peer may prompt
   for permission or acquire capture.
3. For reconciled teardown, disconnect with a held tip/button, then connect
   tablet-first. Separately exercise stream failure: do not infer Host release
   from physical closure or local transport submission alone.
4. Exercise the changed worker with hotplug/focus/fullscreen transitions using
   the same bookmark; repeat broader accepted flows only if these changes fail.
5. Establish the required macOS 15/27 same-package matrix and supported
   Host/profile scope. The development Mac's macOS 26.7 passes are not that matrix.

Compile/component success does not make the merged combination production
qualified. Periodic video pauses, the prior Host input receive-queue overflow
and upstream trust/format/audio gates remain open. Host input-backpressure
package work remains held. This preparation changes no live installation,
device or network settings.

## October 8 foundation review corrections

[Alan's two findings](https://github.com/instinctual/plank-client/pull/11#issuecomment-6052964997)
are addressed in foundation runtime `4a20f40`, with a subsequent test-only commit
covering USB discovery already enabled during opt-out:

- Manual **Continue without tablet** and initial availability expiry use the
  same retirement path. It immediately revokes local USB admission and selection,
  closes capture on its worker queue, permits SUSPEND/DETACH through the still
  live sender, and prevents captured Host/focus callbacks from reactivating that
  instance. A new session can create a fresh capture instance.
- The production wrapper carries the maintained worker's descriptor-filtered
  activity callback into local USB cursor ownership. Background raw input,
  including status traffic, still forwards unchanged but no longer selects
  tablet ownership. Sender and activity callbacks are revoked together before
  their Swift context can disappear. The existing registered-Relay presentation
  path is unchanged; this correction does not introduce a Relay activity contract.

The focused suites pass on all three slices: foundation/sharing policy 553,
display policy 569, and 288 production-wrapper/Swift checks including manual
opt-out, timeout/hotplug, focus/Host callbacks, ordered live-sender release,
fresh capture, background status, real activity and late callback refusal.
Restoring the old unfiltered-input behavior fails the background-status check;
a policy/UI-only opt-out fails the late-hotplug check. Those are negative
fixtures, not new physical hardware reproductions. The production C-wrapper
lifetime suite also passes Clang AddressSanitizer/UndefinedBehaviorSanitizer.

Fresh unsigned application builds passed for foundation Mac and the full
display-stack Mac and Vision device targets. Previously verified public
dependency inputs were reused with fresh app build directories; input pins and
source patches are unchanged. Source/receipt hashes are recorded privately.
No app was installed or launched. Wider unexpected-stream-failure/Host
held-state cleanup, shipping CI and macOS15/27 gates remain open.

## Next independent features

Capture quality remains [the separate original PR 6](https://github.com/cnoellert/plank-client/pull/6),
checkpoint `d264ed4904e2856cead65d7aab48f6c14d0ce063`. It is absent from
accepted Mac build 24 and this stack. Its capability selection and HTTP/session
negotiation touch the same areas as upstream exact-format reconciliation:
integrate it after that contract is settled, preserving explicit rejection
instead of claiming 10-bit presentation from an 8-bit decoder fallback.
The prior 8/10 selection pass proves switching only, not output precision.

Camera source/encoder acceptance is checkpointed separately. Product camera
negotiation, Linux receiver/application, consent and concurrent desktop/audio/
tablet qualification follow the native foundation. iOS follows shared-core
stabilization; it is not an acceptance target of this Mac import.
