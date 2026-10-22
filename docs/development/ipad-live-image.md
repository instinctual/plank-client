# iPad live image and magnification — planned

The October 9 discussion queues this work for the next feature slice. No live
image publisher, zoom runtime or device qualification is implied by this plan.

## Artist experience

Pencil sharing can show the current remote desktop over the charcoal pad, with
an independent Image intensity control. Existing pad margins and artist
controls remain available. The direct iPad client should use the same image
and input geometry. Image intensity changes appearance, not pen mapping or
system brightness.

Offer **Fit / 100% / 200% / 300%** and deliberate pan. Numbered magnification
means source pixels per display pixel: 100% is one source pixel per display
pixel; 200% and 300% enlarge that pixel two and three times. These are not
multiples of Fit. A future offset near-tip magnifier can provide a visual aid
without moving the full canvas during a stroke.

## Geometry and image quality

One immutable transform must drive the image, cursor and inverse pen mapping.
It includes the selected output's source crop and offset, active pad after
margins, zoom/pan, display scale and session/geometry generation. Input adds
crop offsets back and normalizes against the full Host input extent. Define
pixel-center conventions explicitly. Pan/zoom or geometry changes retire an
active stroke before switching transforms; do not move its target underneath
the tip.

Use full pad may stretch in Fit if image and input stretch together. Numbered
pixel magnification should preserve uniform scale. Actual render backing size
must honor the display scale; a source-sized drawable alone does not prove
one-to-one pixels. A downscaled thumbnail enlarged to 200% cannot recover
native detail. Use a sufficiently detailed full frame or native source crop.

## Transport boundary

Direct iPad reuses its decoded desktop frames. For sharing, AVP/Mac forwards
frames from the existing workstation session over a separately authenticated,
bounded media channel, with explicit image-sharing authority. Current Pencil
protocol3 carries pen and keys only; its control lane must not carry image data.
No second workstation session is needed. Encoding and latest-frame dropping
must keep video from delaying tip, release or modifier edges.

Frames identify their desktop and geometry generation, source dimensions/crop
and frame number. Discard stale or mismatched frames, measure local receive and
presentation age, and retain the established input-only pad if images stop.
Remote clocks are not comparable without synchronization. Preserve the source
color/channel path; dimmed reference images are not a qualified color surface.

## Order and targeted qualification

1. Dimmable live image with measured frame age and bounded media transport.
2. Shared Fit/100%/200%/300% transform and deliberate pan.
3. Optional visual-only near-tip magnifier.

Verify transform round trips and crop/margin corners, nonzero output offsets,
native pixel patterns, contact retirement, rotation/keyboard geometry, stale
generations and input edge delivery under image load. Measure physical Pencil
response in both direct and shared contexts. Existing accepted input tests do
not need repeating. Bluetooth media, background capture and Host changes are
outside the initial slice.
