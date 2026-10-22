import Foundation
@preconcurrency import Network

// Real Network sockets and production framing/parser. Only the physical HID
// worker is fake; no network/permission changes or tablet capture in this test.
@MainActor final class MacRelayTestPeers { var peers: [MacRelayPeer] = []; var drawingReady=false }
@main struct MacRelaySocketTests {
    @MainActor static func main() async throws {
        let root = FileManager.default.temporaryDirectory.appendingPathComponent("plank-relay-socket-\(UUID())")
        try FileManager.default.createDirectory(at: root,withIntermediateDirectories:true,attributes:[.posixPermissions:0o700])
        defer { try? FileManager.default.removeItem(at: root) }
        let reopenedRoot = root.appendingPathComponent("reopen")
        try FileManager.default.createDirectory(at:reopenedRoot,withIntermediateDirectories:false,attributes:[.posixPermissions:0o700])
        let firstKeys = stateKeys(reopenedRoot)
        precondition(stateKeys(reopenedRoot) == firstKeys, "App restart must reopen the same private identities")
        guard let native = MacRelayNative(root: root) else { fatalError("state") }
        let listener = try NWListener(using:.tcp,on:.any)
        let peers = MacRelayTestPeers()
        listener.newConnectionHandler = { connection in Task { @MainActor in
            let peer = MacRelayPeer(connection:connection,native:native,setup:true,routes:["192.0.2.10"],drawingPort:28990,setupPort:28991,inventory:{ [.init(group:7,name:"Wacom test USB",serial:nil)] },event:{ event in
                // The external Setup fixture must receive three pending replies
                // before modeling an explicit local approval. No immediate
                // approval can hide a broken first-use waiting screen.
                if CommandLine.arguments.contains("--serve-discovery"), case .approval = event {
                    Task { @MainActor in
                        let pendingPeer = peers.peers.last
                        let approvalFile = CommandLine.arguments.last! + ".approve"
                        for _ in 0..<500 {
                            if FileManager.default.fileExists(atPath: approvalFile) { pendingPeer?.approve(); return }
                            try? await Task.sleep(for:.milliseconds(100))
                        }
                    }
                }
            })
            peers.peers.append(peer);peer.start()
        } }
        try await withCheckedThrowingContinuation { (c:CheckedContinuation<Void,Error>) in
            listener.stateUpdateHandler = { state in switch state {case .ready:c.resume();case .failed(let error):c.resume(throwing:error);default:break} }
            listener.start(queue:.main)
        }
        listener.stateUpdateHandler = nil
        guard let port=listener.port else { fatalError("port") }
        // Optional local interoperability probe uses the real Setup client in a
        // separate process, avoiding its C symbol namespace colliding with raw.
        if CommandLine.arguments.contains("--serve-discovery") {
            let portFile = URL(fileURLWithPath: CommandLine.arguments.last!)
            try String(port.rawValue).write(to:portFile, atomically:true, encoding:.utf8)
            try await Task.sleep(for:.seconds(60))
            for peer in peers.peers { peer.close("Interoperability probe finished") }
            listener.cancel(); return
        }
        let connection=NWConnection(host:"127.0.0.1",port:port,using:.tcp)
        try await connect(connection)
        let json=Data(#"{"version":1,"id":1,"op":"status","drawingHandoffVersion":2}"#.utf8)
        var record=Data("PLTRTCP1".utf8);record.append(1);record.append(UInt8(json.count));record.append(0);record.append(json)
        // Channel 1 matches the actual Setup client bootstrap (connection(.setup)).
        // Fragment the preface, length and body. No unauthenticated mutation or
        // drawing metadata may be returned by this discovery channel.
        for byte in record { try await send(connection,Data([byte])) }
        var response=Data()
        while true {
            let bytes=try await receive(connection);precondition(!bytes.isEmpty);response.append(bytes)
            if response.count>=2 {
                let length=Int(response[0])|Int(response[1])<<8
                if response.count==length+2 { break }
            }
        }
        let object=try JSONSerialization.jsonObject(with:Data(response.dropFirst(2))) as! [String:Any]
        precondition((object["usbTablets"] as? [[String:Any]])?.isEmpty == true, "Public discovery must not expose tablet inventory")
        precondition(object["headsetAuthorized"] as? Bool == false)
        precondition(object["canManage"] as? Bool == false && object["drawingHandoff"] == nil)
        precondition(object["relayKey"] as? String == native.setupKey.map {String(format:"%02x",$0)}.joined())
        precondition(object["id"] as? Int == 1)
        connection.cancel()
        let duplicate=NWConnection(host:"127.0.0.1",port:port,using:.tcp);try await connect(duplicate)
        let bad=Data(#"{"version":1,"id":1,"op":"status","op":"drawing-enrollment"}"#.utf8)
        var invalid=Data("PLTRTCP1".utf8);invalid.append(1);invalid.append(UInt8(bad.count));invalid.append(0);invalid.append(bad)
        try await send(duplicate,invalid)
        let rejected=try await receive(duplicate);precondition(rejected.isEmpty)
        duplicate.cancel()
        let fractional=NWConnection(host:"127.0.0.1",port:port,using:.tcp);try await connect(fractional)
        let fraction=Data(#"{"version":1,"id":1.0,"op":"status"}"#.utf8)
        var fractionalRecord=Data("PLTRTCP1".utf8);fractionalRecord.append(1);fractionalRecord.append(UInt8(fraction.count));fractionalRecord.append(0);fractionalRecord.append(fraction)
        try await send(fractional,fractionalRecord)
        let fractionalReply=try await receive(fractional);precondition(fractionalReply.isEmpty)
        fractional.cancel()
        precondition(!peers.peers.isEmpty)
        let pendingStatus = peers.peers[0].status(id:2,access:.awaitingApproval)!
        let pendingObject = try JSONSerialization.jsonObject(with:pendingStatus) as! [String:Any]
        precondition(pendingObject["phase"] as? String == "verifying", "Existing Setup must display the local approval instruction")
        precondition((pendingObject["message"] as? String)?.contains("Approve Relay Setup") == true)
        precondition(pendingObject["canManage"] as? Bool == false && pendingObject["headsetAuthorized"] as? Bool == false && pendingObject["drawingHandoff"] == nil)
        let candidates = pendingObject["usbTablets"] as! [[String:Any]]
        precondition(candidates.count == 1 && candidates[0]["active"] as? Bool == false && candidates[0]["serial"] == nil)
        precondition(pendingObject["attached"] as? Bool == false && pendingObject["captureActive"] as? Bool == false)
        let unapproved = try JSONSerialization.jsonObject(with:peers.peers[0].status(id:2,access:.unapproved)!) as! [String:Any]
        precondition((unapproved["usbTablets"] as? [[String:Any]])?.isEmpty == true && unapproved["drawingHandoff"] == nil)
        let authenticatedStatus = peers.peers[0].status(id:2,access:.approved)!
        let approvedObject = try JSONSerialization.jsonObject(with:authenticatedStatus) as! [String:Any]
        let usb = approvedObject["usbTablets"] as! [[String:Any]]
        precondition(usb.count == 1 && usb[0]["name"] as? String == "Wacom test USB" && usb[0]["active"] as? Bool == true)
        precondition(approvedObject["attached"] as? Bool == false && approvedObject["captureActive"] as? Bool == false,
                     "Presence must not claim a Setup preview capture")
        precondition(MacRelayUSBTablet.records([]).isEmpty)
        let dedup = MacRelayUSBTablet.records([.init(group:7,name:"one",serial:nil),.init(group:7,name:"one interface",serial:nil),.init(group:3,name:"first",serial:nil)])
        precondition(dedup.count == 2 && dedup[0]["id"] as? String == "usb:3" && dedup[1]["active"] as? Bool == false)
        let many = MacRelayUSBTablet.records((1...12).map { .init(group:UInt64($0),name:String(repeating:"é",count:100),serial:String(repeating:"x",count:100)) })
        precondition(many.count == 8 && many.allSatisfy { ($0["name"] as! String).utf8.count <= 64 && ($0["serial"] as! String).utf8.count <= 64 })
        let handoff = approvedObject["drawingHandoff"] as! [String:Any]
        precondition(handoff["supported"] as? Bool == true && handoff["state"] as? String == "ready")
        let descriptor = handoff["descriptor"] as! [String:Any]
        precondition(Set(descriptor.keys) == ["version","drawingIdentity","drawingProtocol","routes"])
        let protocolObject = descriptor["drawingProtocol"] as! [String:Any]
        precondition(protocolObject["rawHID"] as? Int == 1 && protocolObject["linkType"] as? Int == 2)
        let endpoints = descriptor["routes"] as! [[String:Any]]
        precondition(endpoints.count == 1 && endpoints[0]["port"] as? Int == 28990)
        precondition(approvedObject["headsetAuthorized"] as? Bool == true)
        for peer in peers.peers {peer.close("Test finished")};listener.cancel()
        try await Task.sleep(for:.milliseconds(100))
        try await drawingBurst(native)
        print("Mac Relay real socket: fragmented discovery, bounded public metadata and duplicate-field refusal passed")
    }
    @MainActor static func drawingBurst(_ native:MacRelayNative) async throws {
        let listener=try NWListener(using:.tcp,on:.any)
        let peers=MacRelayTestPeers()
        listener.newConnectionHandler={ connection in Task { @MainActor in
            let peer=MacRelayPeer(connection:connection,native:native,setup:false,routes:[],drawingPort:28990,setupPort:28991,event:{ event in
                if case .ready=event { Task { @MainActor in peers.drawingReady=true } }
            })
            peers.peers.append(peer);peer.start()
        } }
        try await withCheckedThrowingContinuation { (c:CheckedContinuation<Void,Error>) in
            listener.stateUpdateHandler={state in switch state {case .ready:c.resume();case .failed(let error):c.resume(throwing:error);default:break}}
            listener.start(queue:.main)
        };listener.stateUpdateHandler=nil
        let connection=NWConnection(host:"127.0.0.1",port:listener.port!,using:.tcp)
        guard let client=mac_relay_socket_client(native.store) else {fatalError("client")}
        defer { connection.cancel(); listener.cancel(); for peer in peers.peers {peer.close("Burst test finished")}; mac_relay_socket_destroy(client) }
        let timeout=Task { try await Task.sleep(for:.seconds(5)); connection.cancel() }
        defer {timeout.cancel()}
        try await connect(connection)
        var output=[UInt8](repeating:0,count:17_000),written=0
        precondition(mac_relay_socket_start(client,&output,output.count,&written)==0)
        try await send(connection,Data(output.prefix(written)))
        while !mac_relay_socket_ready(client) {
            let data=try await receive(connection);precondition(!data.isEmpty)
            precondition(data.withUnsafeBytes { mac_relay_socket_receive(client,$0.bindMemory(to:UInt8.self).baseAddress,data.count,&output,output.count,&written) }==0)
            if written>0 {try await send(connection,Data(output.prefix(written)))}
        }
        precondition(mac_relay_socket_session_ready(client,&output,output.count,&written)==0)
        try await send(connection,Data(output.prefix(written)))
        while !peers.drawingReady {try await Task.sleep(for:.milliseconds(2))}
        let started=DispatchTime.now().uptimeNanoseconds
        precondition(mac_relay_socket_burst(128))
        precondition(mac_relay_socket_ping(client,&output,output.count,&written)==0)
        try await send(connection,Data(output.prefix(written)))
        while mac_relay_socket_reports(client)<128 || !mac_relay_socket_pong(client) {
            let data=try await receive(connection);precondition(!data.isEmpty)
            precondition(data.withUnsafeBytes { mac_relay_socket_receive(client,$0.bindMemory(to:UInt8.self).baseAddress,data.count,&output,output.count,&written) }==0,"Every raw report must arrive intact and in order")
            if written>0 {try await send(connection,Data(output.prefix(written)))}
        }
        let elapsed=Double(DispatchTime.now().uptimeNanoseconds-started)/1_000_000
        FileHandle.standardOutput.write(Data("Mac Relay 128 ordered raw reports plus bidirectional ping: \(elapsed) ms\n".utf8))
        precondition(elapsed<300,"Queued raw reports must drain without a 5ms wait per write")
    }
    static func stateKeys(_ root:URL) -> [Data] {
        guard let native = MacRelayNative(root:root) else { fatalError("Existing state could not reopen") }
        return [native.setupKey,native.drawingKey]
    }
    static func connect(_ connection:NWConnection) async throws {
        try await withCheckedThrowingContinuation { (c:CheckedContinuation<Void,Error>) in
            connection.stateUpdateHandler={state in switch state {case .ready:c.resume();case .failed(let error):c.resume(throwing:error);default:break}}
            connection.start(queue:.main)
        };connection.stateUpdateHandler=nil
    }
    static func send(_ connection:NWConnection,_ data:Data) async throws {
        try await withCheckedThrowingContinuation { (c:CheckedContinuation<Void,Error>) in connection.send(content:data,completion:.contentProcessed {error in if let error {c.resume(throwing:error)}else{c.resume()}}) }
    }
    static func receive(_ connection:NWConnection) async throws -> Data {
        try await withCheckedThrowingContinuation { c in connection.receive(minimumIncompleteLength:1,maximumLength:8192){data,_,_,error in if let error {c.resume(throwing:error)}else{c.resume(returning:data ?? Data())}} }
    }
}
