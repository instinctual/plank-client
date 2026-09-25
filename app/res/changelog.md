## 1.1.015 — Workstation session indicator

### Client

- Show In Session when a workstation has a logged-in desktop or a remote session.
- Show the desktop username only when the Linux Host administrator enables it.
- Optionally remember the last successful sign-in username for each bookmark; passwords are never saved.
- Rename the physical-display layout option to Match Host; display behavior is unchanged.
- macOS: center the toolbar below the camera-notch area instead of shifting it sideways.

### Host

- Report session status without account lookups or session cleanup during discovery.
- Include connections to the Mac login screen. Same-user access and takeover rules are unchanged.

## 1.1.014 — Mac virtual audio clock correction

### Host

- Correct the clock timestamp period advertised by PLANK Output and PLANK Microphone to meet Core Audio requirements.
- Keep microphone packets at 10 ms and support larger bounded Core Audio reads.

## 1.1.013 — Mac capture clock

### Host

- Use PLANK Output as the explicit clock for remote audio capture.
- Add bounded capture-timing diagnostics for intermittent audio gaps during camera use.

## 1.1.012 — Lower overhead for Mac session checks

### Host

- Check desktop ownership in the background every 500 ms while keeping user-switch and sleep notifications responsive.
- Expire stale session information after one second without blocking audio or typing.

## 1.1.011 — Mac audio and input scheduling

### Host

- Move repeated macOS session checks off the media queue to reduce audio dropouts and typing delays during application startup and camera use.
- Retain session revocation and enforce a freshness limit when system checks stall.

## 1.1.010 — Camera and microphone timing

### Client

- Default microphone activation to Manual for new configurations; preserve saved preferences.
- Ubuntu: retain capture timing with stereo Opus audio when supported by the Host.
- Preserve microphone compatibility with Hosts using the previous stereo format.

### Host

- Request a recovery frame when another application joins PLANK Camera.
- Use timestamped microphone playback to align camera presentation when both are active.
- Preserve camera-only operation and compatibility with previous Clients.

## 1.1.009 — Remote output routing

### Host

- Forward audio sent to PLANK Output while physical speakers retain normal local playback.
- Fix automatic PLANK Output selection when a session connects.
- Keep remote volume and mute tied to PLANK Output when another device is selected locally.

## 1.1.008 — Camera setup status

### Host

- Recognize an enabled PLANK Camera during macOS setup and stop offering Enable Camera again.
- Update an already-enabled camera extension while retaining the existing approval when macOS permits.

## 1.1.007 — Mac remote audio output

### Host

- Add PLANK Output as the macOS playback destination during remote sessions.
- Restore previous playback and alert devices after disconnect, with routing recovery after Host failures.
- Keep remote volume and mute independent of the physical output after selection.
- Recommend restarting after installation to load updated PLANK audio devices; allow restarting later.

## 1.1.006 — Microphone queue recovery

### Host

- Discard stale microphone audio after scheduling stalls and re-prime with fresh samples.
- Improve stereo microphone clock correction when capture delivers packets in batches.

## 1.1.005 — Mac Client display discovery

### Client

- Start sessions when macOS omits the native display-mode flag, using the current backing-pixel dimensions.
- Show a connection error if the display layout cannot be read.
- Retain independent media negotiation; the 1.1.004 Host can remain installed.

## 1.1.004 — Media compatibility candidate

### Client

- Negotiate each Mac media feature independently, preserving desktop connections when optional features are unavailable.
- Connect to supported older Mac Hosts; disable incompatible microphone or camera forwarding.
- Ubuntu: select a native H.264/MJPEG webcam and enable it from the session toolbar. Camera starts off on each connection.

### Host

- Accept Mac launch schemas 4, 5 and 6 alongside independently versioned media features.
- Keep desktop audio and input available to schema-4 Clients, with their incompatible mono microphone disabled.
- macOS: offer native compressed camera output and decoded NV12 through the PLANK Camera extension.

## 1.1.002 — Stereo microphone candidate

Update Host and Client together for this candidate's microphone protocol.

### Client

- Forward microphone audio in stereo using 192 kbps variable-bitrate Opus.

### Host

- macOS: expose a 48 kHz stereo PLANK Microphone input and preserve both channels through decoding.
- Require the matching stereo driver before offering microphone forwarding.

## 1.1.001

Update Host and Client together. This version is not compatible with the previous release's transport.

### Client

- Add microphone forwarding to macOS Hosts, with automatic/manual selection and a toolbar mute switch.
- Remember Host identity before sending login credentials, and ask before trusting a replacement Host.
- Improve connection cancellation, rendering shutdown and audio/video startup reliability.

### Host

- Upgrade loss recovery to RaptorQ 2.0.1 and remove the application datagram pacer.
- macOS: add the PLANK Microphone sound input and improve first-login startup after reboot.
- Bound malformed network data and stalled authentication work.

## 1.0.157

### Host

- macOS: allow certificate preparation to finish during a slow first login after reboot, while keeping stalled operations bounded.

## 1.0.156 — Microphone candidate

### Client

- Add automatic or manual microphone forwarding to macOS Hosts.
- Add a toolbar microphone switch and recording status; muting closes local capture.
- Remember the microphone switch during session reconnection.
- Deliver already-decoded audio/video promptly when setup packets arrive late.

### Host

- macOS: add the PLANK Microphone virtual sound input for remote applications.
- Offer automatic input selection with restoration of the previous device after disconnect.
- Keep microphone failure isolated from video and speaker audio.
- Prevent a startup identity reply from racing connection cleanup.

## 1.0.154

### Client

- Upgrade loss recovery to RaptorQ 2.0.1; requires a matching updated Host.
- Reject malformed or oversized incoming transport messages safely.
- Cancel connection startup promptly and prevent a render-thread shutdown hang.
- Preserve the correct audio/video packet when receive queues overflow.
- Remove the Experimental label from macOS capture.

### Host

- Upgrade loss recovery to RaptorQ 2.0.1; update Host and Client together.
- Remove the unused application datagram pacer; retain Quinn scheduling and transport headroom.
- Bound malformed incoming data and cancel stalled connection setup.
- Linux: keep stalled operating-system authentication from blocking other requests.

## 1.0.153

### Client

- Remember each Host's identity before sending login credentials.
- Ask before trusting a replacement Host; cancelling keeps the previous identity.
- Keep Host trust when bookmarks are deleted or recreated.

### Host

- macOS: keep one machine identity across the login screen and different desktop users.
- Preserve machine identity when renewing Host certificates.

## 1.0.152

### Client

- Match the primary client monitor when ordering a Linux Host's virtual displays.
- Keep the existing image proportions when host and client monitor sizes differ.
- Allow manual two-display bookmarks when the client has a different monitor layout.

### Host

- Linux: place the first virtual connector on the client's primary side for applications that choose the first monitor.

## 1.0.151

### Client

- Offer Take Over or Cancel when another client is connected to the same Mac account.
- Use the new client's display size after an approved takeover.
- Preserve valid packets when the connection adjusts its network packet size.
- Ubuntu: include the image plugin needed to display dialog icons.

### Host

- macOS: transfer an active session to another client after explicit confirmation, including while locked.
- macOS: avoid a temporary loss of connectivity when switching between the login screen and desktop.
- Preserve valid packets during network packet-size recovery on Linux and macOS.
- Reduce diagnostic logging overhead during streaming.

## 1.0.146

### Client

- Click the version number to see what's new, even when offline.

### Host

- macOS: retry failed desktop-service startup after login or user switching.

## 1.0.143

### Client

- Fix mouse positioning at the right and bottom edges of the remote screen.

### Host

- Linux: deliver mouse clicks immediately and improve high-bitrate sending.
- macOS: keep the connection open when the screen locks.
- macOS: let administrators read the Host's system logs.

## 1.0.137

### Client

- Copy and paste plain text between Mac Clients and Mac Hosts.
- Improve clipboard transfers when the connection is busy.

### Host

- macOS: add plain-text clipboard sharing with Mac Clients.
- macOS and Linux: limit shared text to 512 KiB and reject invalid text safely.

## 1.0.135

### Client

- macOS: send Command-Tab and Command-Space to the Host, including after switching Spaces.
- macOS: request keyboard-capture permission before starting a session.
- macOS: improve multi-monitor and Wacom support; support macOS 15 and newer.
- Add network round-trip time to the toolbar and reduce toolbar flicker on macOS.
- Copy and paste plain text between Mac Clients and Linux Hosts.

### Host

- Linux: add plain-text clipboard sharing with Mac Clients.
- Improve mouse-button ordering and keyboard release handling.
