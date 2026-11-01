# PLANK for Apple Vision Pro

This target is PLANK's native visionOS client. It owns the host browser,
bookmarks, settings, pairing and session presentation using SwiftUI, with
transport, decoding and input managed through `PlankCoreClient`.

For a complete unsigned build, use the
[pinned public input recipe](../scripts/apple-native/README.md). The
[integration guide](../docs/development/apple-native-integration.md) distinguishes
the accepted checkpoint from the refreshed upstream dependency combination and
lists the production reconciliation gates. The commands below are the original
project-generation example, not a complete dependency bootstrap.

## Virtual displays

In Add/Edit Workstation, choose **One Virtual Display** or **Two Virtual
Displays**. Each display has an independent resolution. Display 1 is left,
Display 2 is right, with top edges aligned. Both appear together in one spatial
window, using one composite stream and decoder. Window resizing preserves the
combined desktop's aspect ratio; unused space below a shorter display belongs
to the Host canvas. There is no separate-window option in this implementation.

The combined width must not exceed 8192 pixels. Examples include
3840x2160 + 1280x2160, two 2560x2160 displays, and two 3840x2160 displays.
The editor shows the combined size and prevents saving an oversized layout.
Changing either resolution uses the existing session-close confirmation and
applies on the next connection. Start and Retry Session retain both modes.

This uses the Linux Host's existing virtual-display support. It does not add
multiple virtual displays to a macOS Host. Actual headset decoding, visual
quality, Wacom and session acceptance remain required for each tested canvas;
an allowed size is not a hardware performance guarantee.

## Tablet session lifecycle

Once a tablet has passed attachment checks in a desktop session, a later
tablet outage retains Relay recovery even after the availability grace
expires. Mouse and keyboard input become available during that wait. When
tablet ownership returns, input is guarded until fresh descriptors and the
Host acknowledgement pass. An explicit choice to continue without the tablet
still disables it for that session; the initial absent-tablet startup policy
is unchanged.

The first raw tablet attachment on a new desktop transport queues DEVICE,
DETACH, and the identical DEVICE on the ordered input lane, before forwarding
the original descriptors. The preparatory DEVICE establishes the generation
required by the Host's existing DETACH handler; it creates no tablet interfaces.
DETACH destroys any retained Host tablet endpoints so the real attachment can
create a fresh group. Identity, descriptors, reports and control replies remain
unchanged. Focus suspension and Relay reconnects inside the same desktop
transport retain their existing behavior.

This reset uses the existing protocol. Focused bridge checks cover ordering,
a new transport, and failures at every reset stage. The current field checks
and their limits are recorded in [the build 46 release checkpoint](TestFlight/0.1.0-46-preflight.md).
The reset does not address the separately observed native input receive queue
overflow. The tested Host also has the separately qualified packaged Wacom
pressure policy; do not infer that policy is present on older Hosts.

Generate the Xcode project:

```bash
cmake -G Xcode \
  -S visionos-native \
  -B build/visionos-native \
  -DCMAKE_SYSTEM_NAME=visionOS \
  -DCMAKE_OSX_SYSROOT=xros \
  -DCMAKE_OSX_DEPLOYMENT_TARGET=26.0
```

Build for a paired Apple Vision Pro:

```bash
xcodebuild \
  -project build/visionos-native/PlankVision.xcodeproj \
  -scheme PlankVision \
  -destination 'platform=visionOS,id=<YOUR_DEVICE_ID>' \
  -configuration Debug build
```
