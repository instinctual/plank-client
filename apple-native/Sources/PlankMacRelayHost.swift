import AppKit
import SwiftUI
@preconcurrency import Network
import Darwin

// Optional process-owned Relay. No listener, capture or consent survives quit.
@MainActor final class PlankMacRelayHost: ObservableObject {
    @Published private(set) var sharing = false
    @Published private(set) var message = "Sharing is off"
    @Published private(set) var approvalPending = false
    private var drawing: NWListener?
    private var management: NWListener?
    private var native: MacRelayNative?
    private var sessions: [UUID: MacRelayPeer] = [:]
    private var managementPeer: UUID?
    private var drawingPeer: UUID?
    private var generation = UUID()
    private var sleepObserver: NSObjectProtocol?
    private var wakeObserver: NSObjectProtocol?
    private var resumeAfterSleep = false

    init() {
        sleepObserver = NSWorkspace.shared.notificationCenter.addObserver(forName: NSWorkspace.willSleepNotification, object: nil, queue: .main) { [weak self] _ in
            Task { @MainActor in
                guard let self else { return }; let wasSharing = self.sharing
                self.stop(); self.resumeAfterSleep = wasSharing
            }
        }
        wakeObserver = NSWorkspace.shared.notificationCenter.addObserver(forName: NSWorkspace.didWakeNotification, object: nil, queue: .main) { [weak self] _ in
            Task { @MainActor in
                guard let self, self.resumeAfterSleep else { return }
                self.resumeAfterSleep = false; self.start()
            }
        }
    }
    func start() {
        guard !sharing else { return }
        do {
            let root = try FileManager.default.url(for: .applicationSupportDirectory, in: .userDomainMask, appropriateFor: nil, create: true)
                .appendingPathComponent("PLANK/TabletRelay", isDirectory: true)
            try FileManager.default.createDirectory(at: root, withIntermediateDirectories: true, attributes: [.posixPermissions: 0o700])
            guard let state = MacRelayNative(root: root) else { throw RelayHostError.state }
            let draw = try NWListener(using: .tcp, on: NWEndpoint.Port(rawValue: 28990)!)
            let setup = try NWListener(using: .tcp, on: NWEndpoint.Port(rawValue: 28991)!)
            plank_mac_relay_request_capture_permission()
            native = state; drawing = draw; management = setup
            sharing = true; message = "Starting tablet sharing…"; generation = UUID()
            let current = generation
            draw.newConnectionHandler = { [weak self] connection in Task { @MainActor in self?.accept(connection, setup: false, generation: current) } }
            setup.newConnectionHandler = { [weak self] connection in Task { @MainActor in self?.accept(connection, setup: true, generation: current) } }
            draw.stateUpdateHandler = { [weak self] state in Task { @MainActor in
                guard let self, self.generation == current else { return }
                if case .ready = state { self.message = "Sharing enabled. Register this Mac in Relay Setup." }
                if case .failed(let error) = state { self.stop(); self.message = "Sharing unavailable: \(error.localizedDescription)" }
            } }
            setup.stateUpdateHandler = { [weak self] state in Task { @MainActor in
                guard let self, self.generation == current else { return }
                if case .ready = state {
                    var txt = NWTXTRecord(); txt["protocol"] = "1"; txt["id"] = Self.hex(stateKey: self.native?.setupKey ?? Data())
                    txt["hostname"] = "\(Host.current().localizedName ?? "Mac") Tablet Relay"
                    setup.service = .init(name: "PLANK Mac Tablet Relay", type: "_plank-avp-relay._tcp", domain: "local.", txtRecord: txt)
                }
                if case .failed(let error) = state { self.stop(); self.message = "Sharing unavailable: \(error.localizedDescription)" }
            } }
            draw.start(queue: .main); setup.start(queue: .main)
        } catch { stop(); message = "Could not start sharing: \(error.localizedDescription)" }
    }
    private func accept(_ connection: NWConnection, setup: Bool, generation current: UUID) {
        guard sharing, generation == current, let native, let drawingPort = drawing?.port,
              let setupPort = management?.port, sessions.count < 4 else { connection.cancel(); return }
        let id = UUID()
        // One authenticated management owner and one drawing/proof owner. Public
        // probes close promptly; admission never transfers an active capture.
        guard setup ? managementPeer == nil : drawingPeer == nil else { connection.cancel(); return }
        if setup { managementPeer = id } else { drawingPeer = id }
        let peer = MacRelayPeer(connection: connection, native: native, setup: setup,
            routes: Self.addresses(), drawingPort: drawingPort.rawValue, setupPort: setupPort.rawValue) { [weak self] event in
            Task { @MainActor in
                guard let self, self.generation == current else { return }
                switch event {
                case .approval:
                    guard self.managementPeer == id, self.sessions[id] != nil else { return }
                    self.approvalPending = true; self.message = "Relay Setup requests approval. Approve only the headset you are registering."
                case .approved:
                    guard self.managementPeer == id else { return }
                    self.approvalPending = false; self.message = "Setup approved. Continue registration on the headset."
                case .ready: self.message = "Approved headset connected; waiting for USB tablet input"
                case .tablet(let state): self.message = state
                case .ended(let reason):
                    self.sessions.removeValue(forKey: id)
                    if self.managementPeer == id {
                        let awaitingApproval = self.approvalPending
                        self.managementPeer = nil; self.approvalPending = false
                        if awaitingApproval && self.drawingPeer == nil {
                            self.message = "Setup connection closed before approval. On the headset, open Set up a tablet again, then approve here while Setup remains open."
                        }
                    }
                    if self.drawingPeer == id { self.drawingPeer = nil; self.message = "Sharing enabled. \(reason)" }
                }
            }
        }
        sessions[id] = peer; peer.start()
    }
    func approveSetup() {
        guard let id = managementPeer, let peer = sessions[id], approvalPending else { return }
        peer.approve(); message = "Saving Setup approval…"
    }
    func stop() {
        resumeAfterSleep = false
        generation = UUID(); sharing = false; approvalPending = false
        drawing?.stateUpdateHandler = nil; management?.stateUpdateHandler = nil
        drawing?.newConnectionHandler = nil; management?.newConnectionHandler = nil
        drawing?.cancel(); management?.cancel(); drawing = nil; management = nil
        let old = sessions; sessions.removeAll(); managementPeer = nil; drawingPeer = nil
        for peer in old.values { peer.close("Sharing stopped") }
        native = nil; message = "Sharing is off"
    }
    private static func hex(stateKey: Data) -> String { stateKey.map { String(format: "%02x", $0) }.joined() }
    private static func addresses() -> [String] {
        var first: UnsafeMutablePointer<ifaddrs>?
        guard getifaddrs(&first) == 0 else { return [] }
        defer { freeifaddrs(first) }
        var result: [String] = []; var cursor = first
        while let node = cursor {
            defer { cursor = node.pointee.ifa_next }
            guard let address = node.pointee.ifa_addr, address.pointee.sa_family == UInt8(AF_INET),
                  node.pointee.ifa_flags & UInt32(IFF_UP) != 0, node.pointee.ifa_flags & UInt32(IFF_LOOPBACK) == 0 else { continue }
            var host = [CChar](repeating: 0, count: Int(NI_MAXHOST))
            if getnameinfo(address, socklen_t(address.pointee.sa_len), &host, socklen_t(host.count), nil, 0, NI_NUMERICHOST) == 0 {
                let value = String(decoding: host.prefix { $0 != 0 }.map { UInt8(bitPattern:$0) }, as:UTF8.self)
                if !result.contains(value), !value.hasPrefix("169.254.") { result.append(value) }
            }
        }
        return Array(result.prefix(8))
    }
}
private enum RelayHostError: LocalizedError { case state
    var errorDescription: String? { "Relay identity storage is busy or unavailable." }
}
final class MacRelayNative: @unchecked Sendable {
    let store: OpaquePointer
    let setup: UnsafeMutableRawPointer
    let setupKey: Data
    let drawingKey: Data
    init?(root: URL) {
        for part in ["drawing","setup"] {
            do { try FileManager.default.createDirectory(at: root.appendingPathComponent(part), withIntermediateDirectories: true, attributes: [.posixPermissions:0o700]) }
            catch { return nil }
        }
        guard let store = root.appendingPathComponent("drawing").path.withCString({ plank_mac_relay_store_create($0) }) else { return nil }
        guard let setup = root.appendingPathComponent("setup").path.withCString({ plank_mac_setup_create($0) }) else { plank_mac_relay_store_destroy(store); return nil }
        var first = [UInt8](repeating: 0, count: 32), second = first
        guard plank_mac_setup_key(setup, &first) == 0, plank_mac_relay_public_key(store, &second) else {
            plank_mac_setup_destroy(setup); plank_mac_relay_store_destroy(store); return nil
        }
        self.store = store; self.setup = setup; setupKey = Data(first); drawingKey = Data(second)
    }
    deinit { plank_mac_setup_destroy(setup); plank_mac_relay_store_destroy(store) }
}
enum MacRelayEvent: Sendable { case approval, approved, ready, tablet(String), ended(String) }
enum MacRelayStatusAccess { case discovery, unapproved, awaitingApproval, approved }
// A single executor owns each codec and socket. Sends are one-at-a-time;
// backpressure leaves raw reports in the bounded native inbox, never in an
// unbounded stack of Network completions. The HID worker never calls Swift.
final class MacRelayPeer: @unchecked Sendable {
    private let connection: NWConnection
    private let native: MacRelayNative
    private let setupMode: Bool
    private let routes: [String]
    private let drawingPort: UInt16, setupPort: UInt16
    private let event: @Sendable (MacRelayEvent) -> Void
    private let queue = DispatchQueue(label: "la.instinctual.plank.mac-relay.peer", qos: .userInitiated)
    private var buffer = Data()
    private var draw: OpaquePointer?
    private var proof: OpaquePointer?
    private var reportedTablet: UInt32?
    private var mode = 0 // 0 prefix, 1 bootstrap, 2 management, 3 drawing, 4 proof
    private var reading = false
    private var sending = false, closed = false, didReportReady = false, didAsk = false
    private var deadline: UInt64 = 0, lastReceive: UInt64 = 0, sendStarted: UInt64 = 0
    private var timer: DispatchSourceTimer?
    private var managementRequest: Data?
    private let inventory: @Sendable () -> [MacRelayUSBTablet]
    private static var now: UInt64 { DispatchTime.now().uptimeNanoseconds / 1_000_000 }
    @MainActor init(connection: NWConnection, native: MacRelayNative, setup: Bool, routes: [String], drawingPort: UInt16, setupPort: UInt16, inventory: @escaping @Sendable () -> [MacRelayUSBTablet] = MacRelayUSBTablet.connected, event: @escaping @Sendable (MacRelayEvent) -> Void) {
        self.connection = connection; self.native = native; setupMode = setup; self.routes = routes
        self.drawingPort = drawingPort; self.setupPort = setupPort; self.inventory = inventory; self.event = event
        // The physical worker starts inactive and requests OS permission in the
        // normal UI context. No remote peer can open interfaces before READY.
        if !setup { draw = plank_mac_relay_connection_create(native.store) }
    }
    func start() {
        queue.async { [self] in
            deadline = Self.now + 10_000; lastReceive = Self.now
            connection.stateUpdateHandler = { [weak self] state in
                guard let self else { return }
                switch state { case .ready: self.read(); case .failed: self.finish("Connection ended"); case .cancelled: self.finish("Connection ended"); default: break }
            }
            connection.start(queue: queue)
            let tick = DispatchSource.makeTimerSource(queue: queue); timer = tick
            tick.schedule(deadline: .now(), repeating: .milliseconds(5))
            tick.setEventHandler { [weak self] in self?.pump() }; tick.resume()
        }
    }
    func close(_ reason: String) { queue.async { [self] in finish(reason) } }
    func approve() { queue.async { [self] in
        guard !closed, mode == 2, plank_mac_setup_pending(native.setup) != 0 else { return }
        guard plank_mac_setup_accept(native.setup) == 0 else { finish("Setup approval failed"); return }
        didAsk = false; event(.approved); processManagement()
    } }
    private func finish(_ reason: String) {
        guard !closed else { return }; closed = true
        timer?.cancel(); timer = nil; connection.stateUpdateHandler = nil; connection.cancel()
        if let draw { plank_mac_relay_connection_destroy(draw); self.draw = nil }
        if let proof { plank_mac_relay_enrollment_destroy(proof); self.proof = nil }
        if mode == 2 { plank_mac_setup_disconnect(native.setup, Self.now) }
        buffer.removeAll(); managementRequest = nil
        NSLog("PLANK Mac Relay session ended: %@", reason); event(.ended(reason))
    }
    private func read() {
        guard !closed, !reading, !sending else { return }
        reading = true
        connection.receive(minimumIncompleteLength: 1, maximumLength: 16_384) { [weak self] data, _, complete, error in
            guard let self, !self.closed else { return }
            self.reading = false
            if let data, !data.isEmpty {
                self.lastReceive = Self.now
                guard self.buffer.count + data.count <= 32_768 else { self.finish("Receive bound exceeded"); return }
                self.buffer.append(data); self.process()
            }
            if error != nil || complete { self.finish("Connection ended"); return }
            if !self.sending { self.read() }
        }
    }
    private func send(_ data: Data, then: @escaping @Sendable () -> Void = {}) {
        guard !closed, !sending, !data.isEmpty else { finish("Invalid send ordering"); return }
        sending = true; sendStarted = Self.now
        connection.send(content: data, completion: .contentProcessed { [weak self] error in
            guard let self, !self.closed else { return }
            self.sending = false
            guard error == nil else { self.finish("Write failed"); return }
            then(); self.process()
            if !self.closed && !self.sending {
                // Keep a receive armed before draining more output, so Host HID
                // replies and heartbeat requests still make progress mid-burst.
                self.read()
                // Continue queued raw frames as each bounded write completes.
                // The timer finds newly arrived frames; it must not pace every
                // frame to 5ms (about the tablet's entire 200Hz report budget).
                if self.mode == 3 { self.pump() }
            }
        })
    }
    private func process() {
        guard !closed, !sending else { return }
        if mode == 0 {
            if setupMode {
                guard buffer.count >= 9 else { return }
                guard buffer.prefix(8) == Data("PLTRTCP1".utf8), buffer[8] <= 1 else { finish("Invalid Setup channel"); return }
                // Existing Setup TCP contract: 0 = authenticated management,
                // 1 = read-only status discovery. Keep parity with RelayPairingClient.
                mode = buffer[8] == 1 ? 1 : 2; buffer = Data(buffer.dropFirst(9))
            } else {
                guard buffer.count >= 5 else { return }
                if buffer.prefix(5) == Data([80,76,69,78,1]) {
                    proof = plank_mac_relay_enrollment_create(native.store, Self.now); mode = 4
                } else { mode = 3 }
            }
        }
        if mode == 1 {
            guard buffer.count >= 2 else { return }
            let length = Int(buffer[0]) | Int(buffer[1]) << 8
            guard (2...1024).contains(length), buffer.count <= length + 2 else { finish("Invalid status record"); return }
            guard buffer.count == length + 2 else { return }
            guard let command = parse(Data(buffer.dropFirst(2))), command["op"] as? String == "status",
                  let id = integer(command["id"]), Set(command.keys).isSubset(of: ["version","id","op","drawingHandoffVersion"]) else { finish("Invalid public status request"); return }
            guard let payload = status(id: id, access: .discovery) else { finish("Status unavailable"); return }
            var record = Data([UInt8(truncatingIfNeeded: payload.count),UInt8(payload.count >> 8)]); record.append(payload)
            buffer.removeAll(); send(record) { [weak self] in self?.finish("Discovery checked") }; return
        }
        while !buffer.isEmpty && !sending && !closed {
            var output = [UInt8](repeating: 0, count: 17_000), used = 0, written = 0
            let result = buffer.withUnsafeBytes { bytes -> Int32 in
                let base = bytes.bindMemory(to: UInt8.self).baseAddress!
                switch mode {
                case 2: return plank_mac_setup_receive(native.setup, base, buffer.count, &used, Self.now, &output, output.count, &written)
                case 3: guard let draw else { return -1 }; return plank_mac_relay_receive(draw, base, buffer.count, &used, &output, output.count, &written)
                case 4: guard let proof else { return -1 }; return plank_mac_relay_enrollment_receive(proof, base, buffer.count, &used, &output, output.count, &written, Self.now)
                default: return -1
                }
            }
            guard result >= 0, used > 0, used <= buffer.count, written <= output.count else { finish("Authentication or protocol refused"); return }
            buffer = Data(buffer.dropFirst(used))
            if mode == 3, let draw, plank_mac_relay_ready(draw), !didReportReady { didReportReady = true; deadline = 0; event(.ready) }
            if mode == 2, plank_mac_setup_pending(native.setup) != 0 || plank_mac_setup_authorized(native.setup) != 0 { deadline = 0 }
            if written > 0 {
                if mode == 4 && result == 2 { send(Data(output.prefix(written))) { [weak self] in self?.finish("Registration complete") } }
                else { send(Data(output.prefix(written))) }
                return
            }
            if mode == 2 { processManagement() }
        }
    }
    private func pump() {
        guard !closed else { return }
        let now = Self.now
        if (deadline != 0 && now >= deadline) || now - lastReceive > (mode == 2 ? 30_000 : 10_000) || (sending && now - sendStarted > 5_000) {
            finish("Session deadline exceeded"); return
        }
        if mode == 3, let draw, didReportReady {
            let state = plank_mac_relay_tablet_state(draw)
            if reportedTablet != state {
                reportedTablet = state
                let message = state == 3 ? "Sharing USB Wacom with the approved headset" : state == 2 ? "Attaching USB Wacom…" : state == 7 ? "Workstation rejected the tablet" : "Waiting for USB tablet; check Input Monitoring permission and other tablet sessions"
                event(.tablet(message))
            }
        }
        guard !sending else { return }
        if mode == 2 { processManagement(); if sending || closed { return } }
        var output = [UInt8](repeating: 0, count: 17_000), written = 0
        let result: Int32
        if mode == 2 { result = plank_mac_setup_tick(native.setup, now, &output, output.count, &written) }
        else if mode == 3, let draw { result = plank_mac_relay_next(draw, &output, output.count, &written) }
        else { return }
        if result < 0 { finish("Tablet queue or transport ended"); return }
        if written > 0 { send(Data(output.prefix(written))) }
    }
    private func processManagement() {
        guard !sending, !closed else { return }
        if managementRequest == nil {
            var bytes = [UInt8](repeating: 0, count: 4096)
            let length = plank_mac_setup_request(native.setup, &bytes, bytes.count)
            guard length >= 0 else { finish("Invalid management request"); return }
            if length > 0 { managementRequest = Data(bytes.prefix(Int(length))) }
        }
        guard let data = managementRequest else { return }
        guard let command = parse(data), let id = integer(command["id"]), let operation = command["op"] as? String else { finish("Invalid management request"); return }
        let authorized = plank_mac_setup_authorized(native.setup) != 0
        let pending = plank_mac_setup_pending(native.setup) != 0
        if !authorized && pending && !didAsk {
            didAsk = true; event(.approval)
        }
        managementRequest = nil
        let payload: Data?
        if operation == "status", Set(command.keys).isSubset(of: ["version","id","op","drawingHandoffVersion"]) {
            payload = status(id: id, access: authorized ? .approved : pending ? .awaitingApproval : .unapproved)
        } else if operation == "drawing-enrollment" && authorized {
            var reply: [String: Any] = ["version":1,"id":id,"ok":false]
            let expected: Set<String> = ["version","id","op","action","requestID","clientIdentity","drawingIdentity"]
            if Set(command.keys) == expected, let action = command["action"] as? String, ["prepare","cancel"].contains(action),
               let request = hex(command["requestID"], count: 16), let client = hex(command["clientIdentity"], count: 32),
               let target = hex(command["drawingIdentity"], count: 32) {
                let success = request.withUnsafeBytes { r in client.withUnsafeBytes { c in target.withUnsafeBytes { t in
                    plank_mac_relay_grant(native.store, action == "cancel", r.bindMemory(to: UInt8.self).baseAddress,
                        c.bindMemory(to: UInt8.self).baseAddress, t.bindMemory(to: UInt8.self).baseAddress, Self.now)
                } } }
                if success { reply["ok"] = true; reply["requestID"] = command["requestID"]; reply["state"] = action == "prepare" ? "pending" : "canceled"; reply["expiresIn"] = action == "prepare" ? 120 : 0 }
                else { reply["error"] = "Registration expired, canceled, used or refused." }
            } else { reply["error"] = "Invalid registration request." }
            payload = try? JSONSerialization.data(withJSONObject: reply)
        } else {
            payload = try? JSONSerialization.data(withJSONObject: ["version":1,"id":id,"ok":false,"error":"This Mac shares a USB tablet. Tablet pairing and network configuration are managed on the Mac."])
        }
        guard let payload, payload.count <= 4096 else { finish("Management response bound exceeded"); return }
        var output = [UInt8](repeating: 0, count: 17_000), written = 0
        let result = payload.withUnsafeBytes { plank_mac_setup_reply(native.setup, $0.bindMemory(to: UInt8.self).baseAddress, payload.count, &output, output.count, &written) }
        guard result == 0, written > 0 else { finish("Management reply failed"); return }
        send(Data(output.prefix(written)))
    }
    func status(id: Int, access: MacRelayStatusAccess) -> Data? {
        let authorized = access == .approved
        let pending = access == .awaitingApproval
        let identity = native.drawingKey.map { String(format:"%02x",$0) }.joined()
        var object: [String:Any] = ["version":1,"id":id,"ok":true,"hostname":"Mac Tablet Relay","phase":authorized ? "ready" : pending ? "verifying" : "idle",
            "message":"USB tablet sharing is enabled on the Mac. Approve this headset on the Mac to register it.",
            "canManage":authorized,"initialSetup":true,"attached":false,"captureActive":false,"captureBusy":false,
            "secondsRemaining":0,"tablets":[],"candidates":[],"usbTablets":[],"bluetoothAvailable":false,
            "enrollmentVersion":1,"headsetAuthorized":authorized,
            "relayKey":native.setupKey.map { String(format:"%02x",$0) }.joined(),"tcpPort":setupPort,"networkAddresses":routes]
        if pending {
            // Current Setup displays message while verifying, but hides it in
            // idle management. This is an authenticated first-use Noise link;
            // physical presence does not authorize it or expose drawing routes.
            object["message"] = "On the Mac, open PLANK Settings and choose Approve Relay Setup. Keep this Setup screen open until approval completes."
            object["usbTablets"] = MacRelayUSBTablet.records(inventory()).map { tablet in
                var candidate = tablet
                candidate["active"] = false
                candidate.removeValue(forKey: "serial")
                return candidate
            }
        }
        if authorized {
            object["usbTablets"] = MacRelayUSBTablet.records(inventory())
            object["message"] = "USB tablet presence is reported without capture. Use in PLANK to test drawing; Setup preview is not provided by this Mac Relay."
            // Keep attached/captureActive false: Setup does not capture this
            // tablet for decoded preview. USB presence is a separate inventory.
            object["drawingHandoff"] = routes.isEmpty ? ["supported":true,"state":"unavailable","reason":"routes.unavailable"] :
                ["supported":true,"state":"ready","descriptor":["version":1,"drawingIdentity":identity,
                    "drawingProtocol":["name":"pltr-raw-hid","version":1,"rawHID":1,"linkType":2],
                    "routes":routes.map { ["address":$0,"port":drawingPort] as [String:Any] }]]
        }
        return try? JSONSerialization.data(withJSONObject: object)
    }
    private func integer(_ value: Any?) -> Int? {
        guard let value = value as? NSNumber, CFGetTypeID(value) != CFBooleanGetTypeID(), value.doubleValue == Double(value.intValue), (1...1_000_000).contains(value.intValue) else { return nil }
        return value.intValue
    }
    private func parse(_ data: Data) -> [String:Any]? {
        // Reject duplicate members before Foundation can collapse them. The
        // shared scanner also bounds depth and rejects malformed Unicode.
        guard data.count <= 4096 else { return nil }
        guard let document = PlankJSONReader.parse(data), unique(document), let members = document.members,
              members.first(where: { $0.name == "version" })?.value.integerValue == 1,
              let request = members.first(where: { $0.name == "id" })?.value.integerValue, (1...1_000_000).contains(request),
              let object = try? JSONSerialization.jsonObject(with: data) as? [String:Any],
              let version = object["version"] as? NSNumber, CFGetTypeID(version) != CFBooleanGetTypeID(), version.intValue == 1, version.doubleValue == 1 else { return nil }
        return object
    }
    private func unique(_ value: PlankJSON) -> Bool {
        switch value {
        case .object(let members): return Set(members.map(\.name)).count == members.count && members.allSatisfy { unique($0.value) }
        case .array(let values): return values.allSatisfy { unique($0) }
        default: return true
        }
    }
    private func hex(_ value: Any?, count: Int) -> Data? {
        guard let text = value as? String, text.utf8.count == count*2, text.utf8.allSatisfy({ (48...57).contains($0) || (97...102).contains($0) }) else { return nil }
        let bytes = Array(text.utf8)
        return Data(stride(from:0,to:bytes.count,by:2).compactMap { UInt8(String(decoding: bytes[$0..<$0+2],as:UTF8.self),radix:16) })
    }
}
