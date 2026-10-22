# Native Mac display matching — requirement correction

## October 7 resolution

The operator referred to the earlier virtual-display primary and creation-order
work. That behavior is already covered by the existing Host contract; a new
Host extension is unnecessary for this slice. The previous answer conflated
this request with arbitrary pixel-exact logical dimensions. Build 19 implements
the bounded arrangement/primary scope below with the selected qualified modes.
Exact arbitrary dimensions remain outside this implementation, not a dependency
for restoring the earlier accepted workflow.

## Required behavior

The remote virtual desktop must follow the local Mac display arrangement:
left/right (or other supported relative placement), dimensions, and primary
identity. Moving the primary Host crop onto the primary Mac display is
insufficient: a left-primary Host would appear reversed on a right-primary Mac.
Build 18 implements role-based placement only and does not satisfy this request.

Take one local display snapshot for the connection. Keep display ID, logical
bounds, backing pixel dimensions, and the system primary identity separate.
Do not use the focused screen as the primary, mix Retina pixels into logical
positions, or redefine the Host's authenticated source rectangles locally.
Video crops and mouse coordinates must consume the returned Host topology;
Wacom remains one session-owned, byte-preserving raw-HID path.

## Read-only observation on the development Mac

The October 6 query used `NSScreen.screens`, `CGMainDisplayID()` and
`CGDisplayCopyDisplayMode()` with no display or network mutations.

| Local display | Primary | AppKit bounds in points | Backing pixels |
| --- | --- | --- | --- |
| CG279X | Yes | x=0, y=0, 2560×1440 | 2560×1440 |
| Built-in Retina Display | No | x=-2056, y=111, 2056×1329 | 4112×2658 |

AppKit's y axis points upwards. Both displays have their top edge at y=1440.
In a normalized top-left logical desktop, the built-in display is at (0,0),
the Eizo is at (2056,0), and the canvas is 4616×1440. The primary spatial
index is 1. The selected bookmark modes are a separate input; neither query
nor source audit establishes a new live Host layout.

## Earlier Client behavior and existing contract

The previous virtual-primary Client series discovers the local primary,
sorts displays into spatial left/right order, and passes `plankPrimaryOutput`
only for negotiated `0x2000000` on a virtual-startup Host. Manual modes remain
the chosen bookmark modes. Match Client requires detected native mode sizes
to be in the qualified mode list and requires a horizontal arrangement.
It does not submit arbitrary rectangles or reproduce all local offsets.

Source anchors in the earlier Client series are
`app/backend/outputtopology.cpp` (`resolveClientDisplayLayout`,
`clientPrimaryIndex`), `app/streaming/session.cpp`
(`configurePlankHostLayout`) and `app/backend/nvhttp.cpp`.
Before build 19 the native launch builder omitted `plankPrimaryOutput` and the
topology model did not retain `layout.startup_kind` for that capability gate.
Build 19 retains the startup policy and sends the hint through a tested shared
query builder. The retry path verifies primary state and `x11:DP-0` on the
requested spatial side, in addition to both mode sizes. The Host's existing
worker/supervisor code handles connector assignment and MetaMode creation order.

The pinned root's `protocol/output-topology.md` permits `single` and
`dual-horizontal` virtual layouts, qualified resolutions, and optional
primary-side binding. Dual horizontal rectangles are adjacent and top-aligned.
The qualified list does not include 2056×1329 or 4112×2658. Merely querying
the Mac cannot make these modes acceptable to the existing Host contract.

## Two bounded implementation scopes

### Arrangement and primary with selected resolutions

This is a Client change using the existing Host contract:

1. Snapshot Mac identities, bounds and primary before launch. Validate exactly
   two horizontally separated, vertically overlapping displays before
   associating them with a two-output request.
2. Keep requested resolutions in spatial left/right order. Send the correct
   primary-side hint only when the authenticated topology advertises the
   existing capability and virtual startup. Carry it through layout retries
   and verify the accepted primary side along with both mode sizes.
3. Present each Host source rectangle on its corresponding spatial Mac display.
   Keep primary/secondary window lifetime independent of spatial index.
4. Clearly retain selected resolutions as selected resolutions. Do not describe
   this as exact-size or arbitrary-offset matching.

### Exact Mac geometry

This requires a reviewed Host contract extension in addition to Client work:

1. Define the requested common coordinate space, usable fullscreen/notch
   viewport policy and representation of per-display backing scale explicitly.
2. Negotiate bounded explicit output rectangles and primary identity, with
   capability validation before launch. Do not invent unnegotiated query fields,
   silently choose nearby modes, or send an unsupported enum to old Hosts.
3. Extend Host validation and supervised virtual layout application together,
   retain generation binding, and return actual source rectangles. Preserve
   sign-in/ownership policy, topology restoration and raw tablet semantics.
4. Verify primary-right, mixed-scale, offsets, negative origins, unsupported
   geometry, retry/generation changes, mouse seams and held pen input before
   any deployment.

No additional Host implementation, build, installation or network changes are
part of this slice. The prior scope question is superseded by the operator's
clarification to reuse the earlier virtual-primary behavior. Do not stage
another role-only build as a complete arrangement fix.
