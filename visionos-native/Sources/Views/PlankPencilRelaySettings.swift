import SwiftUI

struct PlankPencilRelaySettings: View {
    @ObservedObject var receiver: PlankPencilRelayReceiver
    @State private var discoveryOwner = UUID()
    var body: some View {
        Section("Apple Pencil") {
            Text("On your iPad, open PLANK and choose Share Apple Pencil. Keep both devices on a reachable local network.")
                .font(.footnote).foregroundStyle(.secondary)
            if let name = receiver.selectedName {
                LabeledContent("iPad",value:name)
                Button("Disconnect Pencil") { receiver.disconnect() }
            } else {
                ForEach(receiver.pads) { pad in Button(pad.name) { receiver.connect(pad) } }
                if receiver.pads.isEmpty { Text("No Pencil pads found").foregroundStyle(.secondary) }
            }
            Text(receiver.status).font(.footnote).foregroundStyle(.secondary)
#if PLANK_NATIVE_MAC_WACOM
            if receiver.connected {
                DisclosureGroup("Input Details") {
                    Text(receiver.inputDetails).font(.caption).foregroundStyle(.secondary)
                    Text(receiver.inputCounts).font(.caption.monospacedDigit()).foregroundStyle(.secondary)
                }
                Toggle("Share desktop image with this iPad",isOn:Binding(
                    get:{ receiver.imageSharingEnabled },set:{ receiver.setImageSharingEnabled($0) }))
                PlankPencilImageStatus(sender:receiver.imageSender)
            }
#endif
            if let code = receiver.verification {
                Text(code).font(.title2.monospaced().bold())
                Text("Compare the code on the iPad before approving on either device.").font(.footnote)
                Button("Codes Match — Approve iPad") { receiver.approve() }.buttonStyle(.borderedProminent)
                Button("Reject",role:.cancel) { receiver.disconnect() }
            }
        }
        .onAppear { receiver.discover(owner: discoveryOwner) }
        .onDisappear { receiver.stopDiscovery(owner: discoveryOwner) }
    }
}
#if !PLANK_NATIVE_MAC_WACOM
/// Live image consent belongs with the running desktop's controls. Peer
/// selection and comparison approval remain in Apple Pencil setup.
struct PlankPencilImageSessionControls: View {
    @ObservedObject var receiver: PlankPencilRelayReceiver

    var body: some View {
        VStack(alignment: .leading, spacing: 8) {
            Text("iPad image").font(.headline)
            Toggle("Share desktop image with this iPad", isOn: Binding(
                get: { receiver.imageSharingEnabled },
                set: { receiver.setImageSharingEnabled($0) }
            ))
            .disabled(!receiver.connected)
            .accessibilityIdentifier("plank.share-ipad-image")
            if receiver.connected {
                PlankPencilImageStatus(sender: receiver.imageSender)
            } else {
                Text("Connect an iPad in Settings → Apple Pencil to share the desktop image.")
                    .font(.footnote)
                    .foregroundStyle(.secondary)
            }
        }
    }
}
#endif
private struct PlankPencilImageStatus: View {
    @ObservedObject var sender: PlankPencilImageSender
    var body: some View {
        VStack(alignment:.leading,spacing:4) {
            Text(sender.status)
            Text("Enable Receive desktop image in the iPad’s Pad Options. The dimmable image is a reference for painting.")
        }.font(.footnote).foregroundStyle(.secondary)
    }
}
