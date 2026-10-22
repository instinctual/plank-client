import SwiftUI

/// The session menu: live Host-audio volume and mute, and the running
/// session's encoder target. Its ornament window is registered with the
/// desktop session, so using it keeps the tablet attached (PlankSessionFocus).
struct PlankSessionControls: View {
    @ObservedObject var client: PlankCoreClient
    @Binding var volume: Double
    @Binding var muted: Bool
    @Binding var allowWindowResizing: Bool
    @Binding var mouseSensitivity: Double
    let onDone: () -> Void
    @Environment(\.openWindow) private var openWindow
    @State private var bitrateMbps: Double?

    var body: some View {
        VStack(alignment: .leading, spacing: 18) {
            HStack {
                Text("Session controls").font(.headline)
                Spacer()
                Button("Done", action: onDone)
            }
            Divider()
            VStack(alignment: .leading, spacing: 8) {
                HStack {
                    Text("Audio").font(.headline)
                    Spacer()
                    Text(muted ? "Muted" : "\(Int((volume * 100).rounded()))%")
                        .monospacedDigit()
                        .foregroundStyle(.secondary)
                }
                HStack(spacing: 12) {
                    Button {
                        muted.toggle()
                    } label: {
                        Image(systemName: muted || volume == 0 ?
                              "speaker.slash.fill" : "speaker.wave.2.fill")
                            .frame(width: 24)
                    }
                    .buttonStyle(.borderless)
                    .accessibilityLabel(muted ? "Unmute workstation audio" : "Mute workstation audio")
                    Slider(value: $volume, in: 0...1)
                        .disabled(muted)
                        .accessibilityLabel("Workstation audio volume")
                }
            }

            VStack(alignment: .leading, spacing: 8) {
                if let status = client.liveBitrate, status.supported {
                    HStack {
                        Text("Video bitrate").font(.headline)
                        Spacer()
                        Text(StreamBitrate.megabitsLabel(Int((displayedMbps(status) * 1000).rounded())))
                            .monospacedDigit()
                    }
                    Slider(
                        value: Binding(
                            get: { displayedMbps(status) },
                            set: { value in
                                bitrateMbps = value
                                client.chooseLiveBitrate(Int((value * 1000).rounded()), final: false)
                            }
                        ),
                        in: Double(StreamBitrate.minimumKbps) / 1000...Double(StreamBitrate.maximumKbps) / 1000,
                        step: Double(StreamBitrate.stepKbps) / 1000
                    ) { editing in
                        if !editing, let bitrateMbps {
                            client.chooseLiveBitrate(Int((bitrateMbps * 1000).rounded()), final: true)
                        }
                    }
                    .accessibilityLabel("Live video bitrate")
                    Text(acceptance(status))
                        .font(.footnote)
                        .foregroundStyle(.secondary)
                } else {
                    Text("Video bitrate").font(.headline)
                    Text(client.liveBitrate == nil ?
                         "Available while connected." :
                         "This workstation does not support changing the bitrate during a session.")
                        .font(.footnote)
                        .foregroundStyle(.secondary)
                }
                Text("Applies to this session only.")
                    .font(.caption)
                    .foregroundStyle(.secondary)
            }

#if PLANK_PENCIL_RELAY_RECEIVER
            PlankPencilImageSessionControls(receiver: client.pencilRelay)
#endif

            VStack(alignment: .leading, spacing: 8) {
                HStack {
                    Text("Mouse speed").font(.headline)
                    Spacer()
                    Text("\(Int((PlankMouseSensitivity.normalized(mouseSensitivity) * 100).rounded()))%")
                        .monospacedDigit()
                }
                Slider(value: Binding(
                    get: { PlankMouseSensitivity.normalized(mouseSensitivity) },
                    set: { mouseSensitivity = PlankMouseSensitivity.normalized($0) }
                ), in: PlankMouseSensitivity.minimum...PlankMouseSensitivity.maximum, step: 0.05)
                .accessibilityLabel("Desktop mouse speed")
                Text("Applies immediately to mouse movement. Pen speed is unchanged.")
                    .font(.caption)
                    .foregroundStyle(.secondary)
            }

            Divider()
            VStack(alignment: .leading, spacing: 8) {
                Text("Window").font(.headline)
                Button {
                    onDone()
                    openWindow(id: "plank-browser")
                } label: {
                    Label("Show Workstations", systemImage: "macwindow.on.rectangle")
                }
                .accessibilityIdentifier("plank.open-workstations")
                Toggle("Allow resizing", isOn: $allowWindowResizing)
                Text("Turn off after resizing to keep mouse control at the corners.")
                    .font(.caption)
                    .foregroundStyle(.secondary)
                    .fixedSize(horizontal: false, vertical: true)
            }
        }
        .padding(20)
        .frame(width: 380, alignment: .leading)
    }

    private func displayedMbps(_ status: PlankLiveBitrateStatus) -> Double {
        bitrateMbps ?? Double(status.currentKbps) / 1000
    }

    private func acceptance(_ status: PlankLiveBitrateStatus) -> String {
        let accepted = status.acceptedKbps.map {
            "Applied: \(StreamBitrate.megabitsLabel($0))"
        } ?? "Applied: \(StreamBitrate.megabitsLabel(status.startupKbps))"
        return status.pending ? accepted + " · applying…" : accepted
    }
}
