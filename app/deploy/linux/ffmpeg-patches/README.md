# Identity GBR FFmpeg Requirement

Desktop Client builds that advertise PLANK identity GBR must link against an
FFmpeg build containing `0001-hevc-enable-hwaccel-for-identity-gbr.patch`.
Without it, FFmpeg exposes only the `gbrp10le` software format for HEVC streams
whose VUI uses matrix coefficients 0, even when the VA-API driver supports the
Main 4:4:4 10 profile.

Apply the patch to the FFmpeg source used by the PLANK package. Do not
replace distribution FFmpeg libraries globally; package the patched libraries
with PLANK or build them in an isolated prefix.

The aliases must be in HEVC's `get_format()` hardware-selection switch, not
the similarly shaped alpha-format switch. Bootstrap and package preflight
check that location. The Client tests also decode real identity frames and
require FFmpeg to offer the platform's hardware format before selecting
software for the test. Those tests need no GPU; installed exact-profile
hardware decoding and presentation remain separate acceptance gates.

macOS also applies `0002-videotoolbox-require-hevc-hardware.patch`. VideoToolbox
otherwise permits a software HEVC decoder behind the same hardware-surface API.
The patch requires hardware at session creation, for both test and live decoders.
H.264 can still use the independent FFmpeg software decoder. The second patch
is not a Linux build input; Linux uses VA-API and the same Client codec policy.
