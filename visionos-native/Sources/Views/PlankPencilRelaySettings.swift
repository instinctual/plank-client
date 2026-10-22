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
