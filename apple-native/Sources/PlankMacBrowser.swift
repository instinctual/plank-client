import SwiftUI

struct PlankMacBrowser: View {
    @EnvironmentObject private var store: HostStore
    @EnvironmentObject private var client: PlankCoreClient
    @EnvironmentObject private var relay: PlankMacRelayHost
    @Environment(\.openWindow) private var openWindow
    @State private var selection: UUID?
    @State private var editing: HostBookmark?
    @State private var add = false
    @State private var username = ""
    @State private var password = ""
    private var selected: HostBookmark? { store.hosts.first { $0.id == selection } }
    var body: some View {
        NavigationSplitView {
            List(store.hosts, selection: $selection) { host in
                Label(host.name, systemImage: "desktopcomputer").tag(host.id)
                    .contextMenu {
                        Button("Edit Workstation") { editing = host }
                        Button("Remove Bookmark", role: .destructive) { store.remove(host); if selection == host.id { selection = nil } }
                            .disabled(client.activeHostID == host.id)
                    }
            }.navigationTitle("Workstations")
            .navigationSplitViewColumnWidth(min: 220, ideal: 250)
            .toolbar { ToolbarItem(placement: .primaryAction) {
                Button { add = true } label: { Label("Add Workstation", systemImage: "plus") }
                    .help("Add Workstation")
            } }
        } detail: {
            if let host = selected {
                VStack(spacing: 18) {
                    Image(systemName: "desktopcomputer").font(.system(size: 52)).foregroundStyle(.cyan)
                    Text(host.name).font(.title)
                    Text("\(host.spatialDisplaySize.title)\(host.secondDisplaySize.map { " + " + $0.title } ?? "") · \(host.streamFrameRate) fps").foregroundStyle(.secondary)
                    if client.activeHostID == host.id {
                        switch client.phase {
                        case .needsCredentials, .authenticating:
                            TextField("Username", text: $username)
                            SecureField("Password", text: $password).onSubmit { authenticate() }
                            Button("Sign In") { authenticate() }.disabled(client.phase.isBusy)
                        case .authenticated:
                            Button("Open Desktop") { start(host) }.buttonStyle(.borderedProminent).disabled(relay.sharing)
                        case .streaming, .frameReceived, .startingSession:
                            Button("Show Desktop") {
                                openWindow(id: "desktop")
                                if client.sessionTopology?.splitPresentation == true { openWindow(id: "desktop-secondary") }
                            }
                        case let .failed(message):
                            Text(message).foregroundStyle(.orange).textSelection(.enabled)
                            Button("Reconnect") { Task { await client.reset()?.value; await client.connect(to: host) } }
                        case .probing: ProgressView("Connecting…")
                        case .idle: connect(host)
                        }
                    } else { connect(host) }
                    Button("Edit Workstation") { editing = host }
                }.frame(maxWidth: 420).padding(30)
            } else {
                VStack(spacing: 12) {
                    ContentUnavailableView("Choose a workstation", systemImage: "desktopcomputer", description: Text("Add a workstation to begin. This pilot keeps its own bookmarks."))
                    Button("Add Workstation") { add = true }.buttonStyle(.borderedProminent)
                }
            }
        }
        .sheet(isPresented: $add) { PlankMacBookmarkEditor(host: HostBookmark(name: "", address: ""), isNew: true) }
        .sheet(item: $editing) { host in PlankMacBookmarkEditor(host: host, isNew: false) }
        .onChange(of: store.hosts) { _, hosts in if selection == nil { selection = hosts.first?.id } }
    }
    private func connect(_ host: HostBookmark) -> some View {
        Button("Connect") { Task { await client.reset()?.value; password = ""; await client.connect(to: host) } }
            .buttonStyle(.borderedProminent).disabled(client.isClosingSession || relay.sharing)
    }
    private func authenticate() { Task { await client.authenticate(username: username, password: password); password = "" } }
    private func start(_ host: HostBookmark) {
        guard !relay.sharing else { return }
        client.setTabletActive(true)
        client.startSession(displaySize: host.spatialDisplaySize, frameRate: host.streamFrameRate,
            videoBitrateKbps: host.videoBitrateKbps, secondDisplaySize: host.secondDisplaySize,
            localDisplayLayout: host.secondDisplaySize == nil ? nil : PlankMacDisplayPriority.snapshot())
        store.markConnected(host); openWindow(id: "desktop")
    }
}
extension ConnectionPhase {
    var isBusy: Bool { switch self { case .probing, .authenticating, .startingSession: true; default: false } }
}
struct PlankMacBookmarkEditor: View {
    @EnvironmentObject private var store: HostStore
    @EnvironmentObject private var client: PlankCoreClient
    @Environment(\.dismiss) private var dismiss
    @State var host: HostBookmark
    let isNew: Bool
    @State private var warning = false
    @State private var saving = false
    var body: some View {
        VStack(spacing: 18) {
            Text(isNew ? "Add Workstation" : "Edit Workstation").font(.title2)
            Form {
                TextField("Name", text: $host.name)
                TextField("Address", text: $host.address)
                TextField("Port", value: $host.port, format: .number.grouping(.never))
                Toggle("Two displays", isOn: Binding(get: { host.secondDisplaySize != nil }, set: {
                    host.secondDisplaySize = $0 ? .standard : nil
                }))
                Picker(host.secondDisplaySize == nil ? "Resolution" : "Left display", selection: $host.spatialDisplaySize) { ForEach(SpatialDisplaySize.allCases) { Text($0.title).tag($0) } }
                if host.secondDisplaySize != nil {
                    Picker("Right display", selection: Binding(get: { host.secondDisplaySize ?? .standard }, set: { host.secondDisplaySize = $0 })) {
                        ForEach(SpatialDisplaySize.allCases) { Text($0.title).tag($0) }
                    }
                    Text("Resolutions follow the Mac's left/right display order. A compatible Host also follows its primary display and connector order.").font(.caption).foregroundStyle(.secondary)
                    if !PlankTopology.validCanvas(first: host.spatialDisplaySize, second: host.secondDisplaySize) {
                        Text("The combined width must be 8192 pixels or less.").foregroundStyle(.orange)
                    }
                }
                Picker("Refresh rate", selection: $host.streamFrameRate) { ForEach(StreamFrameRate.presets, id: \.self) { Text("\($0) fps").tag($0) } }
            }
            HStack {
                Button("Cancel") { dismiss() }.keyboardShortcut(.cancelAction)
                Spacer()
                Button("Save") { if client.requiresSessionCloseForBookmark(host) { warning = true } else { save() } }
                    .keyboardShortcut(.defaultAction).disabled(saving || host.name.isEmpty || host.address.isEmpty || host.port == 0 || !PlankTopology.validCanvas(first: host.spatialDisplaySize, second: host.secondDisplaySize))
            }
        }.padding(24).frame(width: 480)
        .alert("Close the workstation session?", isPresented: $warning) {
            Button("Close and Save", role: .destructive) { save() }
            Button("Cancel", role: .cancel) {}
        } message: { Text("Changing display layout, resolution, timing or the address requires closing this session and signing in again.") }
    }
    private func save() { saving = true; Task {
        await client.closeSessionForBookmarkChange(host)
        if isNew { store.add(name: host.name, address: host.address, port: host.port, displaySize: host.spatialDisplaySize, frameRate: host.streamFrameRate, secondDisplaySize: host.secondDisplaySize) } else { store.update(host) }
        dismiss()
    } }
}
