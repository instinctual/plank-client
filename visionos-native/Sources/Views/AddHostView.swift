import SwiftUI

struct AddHostView: View {
    private enum Field: Hashable { case name, address, port, frameRate }

    @Environment(\.dismiss) private var dismiss
    @Environment(\.dismissWindow) private var dismissWindow
    @ObservedObject var store: HostStore
    @ObservedObject var client: PlankCoreClient
    let editingHost: HostBookmark?
    @FocusState private var focusedField: Field?
    @State private var showingRemoveConfirmation = false
    @State private var pendingEdit: HostBookmark?
    @State private var showingSessionCloseConfirmation = false
    @State private var saving = false

    @State private var name = ""
    @State private var address = ""
    @State private var port = "28989"
    @State private var displaySize: SpatialDisplaySize = .standard
    @State private var useSecondDisplay = false
    @State private var secondDisplaySize: SpatialDisplaySize = .standard
    @State private var frameRateSelection = StreamFrameRate.defaultValue
    @State private var customFrameRate = ""

    init(store: HostStore, client: PlankCoreClient, host: HostBookmark? = nil) {
        self.store = store
        self.client = client
        editingHost = host
        _name = State(initialValue: host?.name ?? "")
        _address = State(initialValue: host?.address ?? "")
        _port = State(initialValue: host.map { String($0.port) } ?? "28989")
        _displaySize = State(initialValue: host?.spatialDisplaySize ?? .standard)
        _useSecondDisplay = State(initialValue: host?.secondDisplaySize != nil)
        _secondDisplaySize = State(initialValue: host?.secondDisplaySize ?? .standard)
        let savedRate = host?.streamFrameRate ?? StreamFrameRate.defaultValue
        _frameRateSelection = State(initialValue:
            StreamFrameRate.presets.contains(savedRate) ? savedRate : 0)
        _customFrameRate = State(initialValue:
            StreamFrameRate.presets.contains(savedRate) ? "" : String(savedRate))
    }

    private var parsedPort: UInt16? { UInt16(port) }
    private var parsedFrameRate: Int? {
        let value = frameRateSelection == 0 ? Int(customFrameRate) : frameRateSelection
        guard let value, (1...240).contains(value) else { return nil }
        return value
    }
    private var canSave: Bool {
        !name.trimmingCharacters(in: .whitespacesAndNewlines).isEmpty &&
        !address.trimmingCharacters(in: .whitespacesAndNewlines).isEmpty &&
        parsedPort.map { $0 > 0 } == true && parsedFrameRate != nil && validCanvas
    }

    private var selectedSecondDisplay: SpatialDisplaySize? {
        useSecondDisplay ? secondDisplaySize : nil
    }
    private var validCanvas: Bool {
        PlankTopology.validCanvas(first: displaySize, second: selectedSecondDisplay)
    }
    private var desktopDimensions: PlankFrameDimensions {
        PlankTopology.canvas(first: displaySize, second: selectedSecondDisplay)
    }

    private var isSessionActive: Bool {
        if client.isClosingSession { return true }
        switch client.phase {
        case .startingSession, .frameReceived, .streaming:
            return true
        default:
            return false
        }
    }

    var body: some View {
        NavigationStack {
            Form {
                Section("Workstation") {
                    TextField("Name", text: $name)
                        .focused($focusedField, equals: .name)
                    TextField("Address or hostname", text: $address)
                        .focused($focusedField, equals: .address)
                        .textInputAutocapitalization(.never)
                        .autocorrectionDisabled()
                    TextField("Port", text: $port)
                        .focused($focusedField, equals: .port)
                        .keyboardType(.numberPad)
                }

                Section {
                    Picker("Host display layout", selection: $useSecondDisplay) {
                        Text("One Virtual Display").tag(false)
                        Text("Two Virtual Displays").tag(true)
                    }
                    .pickerStyle(.menu)

                    Picker(useSecondDisplay ? "Display 1 — Left" : "Resolution", selection: $displaySize) {
                        ForEach(SpatialDisplaySize.allCases) { size in
                            Text(size.title).tag(size)
                        }
                    }
                    .pickerStyle(.menu)

                    if useSecondDisplay {
                        Picker("Display 2 — Right", selection: $secondDisplaySize) {
                            ForEach(SpatialDisplaySize.allCases) { size in
                                Text(size.title).tag(size)
                            }
                        }
                        .pickerStyle(.menu)
                    }

                    if validCanvas {
                        LabeledContent("Desktop canvas",
                            value: "\(desktopDimensions.width) × \(desktopDimensions.height)")
                    } else {
                        Label("The two display widths must total no more than 8192 pixels.",
                              systemImage: "exclamationmark.triangle")
                            .foregroundStyle(.red)
                    }

                } header: {
                    Text("Virtual displays")
                } footer: {
                    if useSecondDisplay {
                        Text("Both displays appear side by side in one spatial window. Display 1 is on the left. Displays are aligned along the top.")
                    }
                }

                Section("Streaming") {
                    Picker("Stream frame rate", selection: $frameRateSelection) {
                        ForEach(StreamFrameRate.presets, id: \.self) { rate in
                            Text("\(rate) fps").tag(rate)
                        }
                        Text("Custom…").tag(0)
                    }
                    .pickerStyle(.menu)
                    if frameRateSelection == 0 {
                        TextField("Custom frame rate (1–240 fps)", text: $customFrameRate)
                            .focused($focusedField, equals: .frameRate)
                            .keyboardType(.numberPad)
                    }
                }

                Section {
                    Text("The default PLANK host port is 28989.")
                        .foregroundStyle(.secondary)
                }

                if editingHost != nil {
                    Section {
                        Button("Remove Workstation", role: .destructive) {
                            showingRemoveConfirmation = true
                        }
                        .disabled(isSessionActive)
                    } footer: {
                        Text("Remove this saved workstation from PLANK.")
                    }
                }
            }
            .navigationTitle(editingHost == nil ? "Add Workstation" : "Edit Workstation")
            .onAppear {
                if editingHost != nil { focusedField = .name }
            }
            .toolbar {
                ToolbarItem(placement: .cancellationAction) {
                    Button("Cancel") { dismiss() }.disabled(saving)
                }
                ToolbarItem(placement: .confirmationAction) {
                    Button(editingHost == nil ? "Add" : "Save") { requestSave() }
                    .disabled(!canSave || saving)
                }
            }
            .disabled(saving)
            .interactiveDismissDisabled(saving)
            .overlay {
                if saving {
                    ProgressView("Closing workstation session…")
                        .padding(24)
                        .glassBackgroundEffect()
                }
            }
            .alert("Close the logged-in session?", isPresented: $showingSessionCloseConfirmation) {
                Button("Cancel", role: .cancel) { pendingEdit = nil }
                Button("Close Session and Save", role: .destructive) {
                    guard let pendingEdit else { return }
                    saveEditedHost(pendingEdit)
                }
            } message: {
                Text("These changes require a new workstation session. PLANK will close this connection before saving. Sign in again to reconnect.")
            }
            .confirmationDialog(
                "Remove Workstation?",
                isPresented: $showingRemoveConfirmation,
                titleVisibility: .visible
            ) {
                Button("Remove Workstation", role: .destructive) {
                    guard let editingHost, !isSessionActive else { return }
                    store.remove(editingHost)
                    dismiss()
                }
            } message: {
                Text("This removes the saved workstation from PLANK.")
            }
        }
        .frame(minWidth: 540, minHeight: 420)
    }
    private func requestSave() {
        guard !saving, canSave, let parsedPort, let parsedFrameRate else { return }
        let cleanName = name.trimmingCharacters(in: .whitespacesAndNewlines)
        let cleanAddress = address.trimmingCharacters(in: .whitespacesAndNewlines)
        if var edited = editingHost {
            edited.name = cleanName
            edited.address = cleanAddress
            edited.port = parsedPort
            edited.spatialDisplaySize = displaySize
            edited.secondDisplaySize = selectedSecondDisplay
            edited.streamFrameRate = parsedFrameRate
            if client.requiresSessionCloseForBookmark(edited) {
                pendingEdit = edited
                showingSessionCloseConfirmation = true
            } else {
                store.update(edited)
                dismiss()
            }
        } else {
            store.add(name: cleanName, address: cleanAddress,
                      port: parsedPort, displaySize: displaySize,
                      frameRate: parsedFrameRate, secondDisplaySize: selectedSecondDisplay)
            dismiss()
        }
    }

    private func saveEditedHost(_ edited: HostBookmark) {
        guard !saving else { return }
        saving = true
        Task { @MainActor in
            let closesCurrentSession = client.requiresSessionCloseForBookmark(edited)
            await client.closeSessionForBookmarkChange(edited)
            if closesCurrentSession && client.activeHostID == nil {
                dismissWindow(id: "plank-desktop")
            }
            store.update(edited)
            pendingEdit = nil
            saving = false
            dismiss()
        }
    }

}
