#!/usr/bin/env python3
"""Exercise automatic Sharing reception and mapping lifetime, without networking.

Production relay/receiver/decoder bodies are compiled with recording network,
codec and UIKit boundaries. No devices, preferences, Host or network are changed.
"""
import argparse, hashlib, importlib.util, json, pathlib, subprocess, sys, tempfile
sys.dont_write_bytecode = True
REPO = pathlib.Path(__file__).resolve().parents[2]
spec = importlib.util.spec_from_file_location('extract', REPO/'ios-native/Tests/run-finger-input-checks.py')
extract = importlib.util.module_from_spec(spec); spec.loader.exec_module(extract)
SUPPORT = r'''
import Foundation
import Combine
import CoreGraphics

enum NWEndpoint: Sendable { case service(name:String,type:String,domain:String,interface:Int?) }
final class NWConnection: @unchecked Sendable { var cancelled=false; func cancel() { cancelled=true } }
struct NWParameters { static let tcp=Self() }
struct NWTXTRecord { subscript(_ key:String)->String? { get { nil } set {} } }
@MainActor final class NWListener {
    struct Port { static let any=Self() }
    struct Service {
        let name:String,type:String,domain:String
        var noAutoRename=false
        init(name:String,type:String,domain:String,txtRecord:NWTXTRecord?=nil) { self.name=name; self.type=type; self.domain=domain }
    }
    enum Registration { case add(NWEndpoint),remove(NWEndpoint) }
    enum State { case ready,failed }
    static var made:[NWListener]=[]
    var service:Service?, newConnectionLimit=0, cancelled=false
    var serviceRegistrationUpdateHandler:((Registration)->Void)?
    var newConnectionHandler:((NWConnection)->Void)?
    var stateUpdateHandler:((State)->Void)?
    init(using:NWParameters,on:Port) throws { Self.made.append(self) }
    func start(queue:DispatchQueue) {}
    func cancel() { cancelled=true }
}
@MainActor final class UIDevice { static let current=UIDevice(); let name="Fixture" }
enum PlankPencilWireError:Error { case invalid }
enum PlankPencilProtocol { static let version="3",capability="normalized-pen" }
enum PlankPencilRelayKeys {
    static func privateKey() throws ->Data { Data(repeating:1,count:32) }
    static func publicKey(_ key:Data) throws ->Data { Data(repeating:2,count:32) }
    static func hex(_ data:Data)->String { "fixture" }
}
struct PlankPencilSessionBinding:Sendable,Equatable { let peerKey:Data,transcriptHash:Data }
struct PlankNormalizedPen:Sendable { let phase:Int }
struct PlankPencilKeyEvent:Sendable,Equatable { let code:UInt16,pressed:Bool,modifiers:UInt8 }
enum PlankPencilMessage:Sendable {
    case configuration(Int,Int,Bool), pen(PlankNormalizedPen), keys([PlankPencilKeyEvent]), rightClick(x:Float,y:Float)
}
enum PlankPencilEvent:Sendable { case ready,verification(String),messagesAvailable,ended(String) }
@MainActor final class PlankPencilIncoming {
    var messages:[PlankPencilMessage]=[]
    func take() throws ->PlankPencilMessage? { messages.isEmpty ? nil : messages.removeFirst() }
}
@MainActor final class PlankPencilRelayPeer {
    static var made:[PlankPencilRelayPeer]=[]
    let incoming=PlankPencilIncoming()
    let event:@Sendable (PlankPencilEvent)->Void
    var binding:PlankPencilSessionBinding?
    var closed=false, sent:[PlankPencilMessage]=[]
    init(connection:NWConnection,privateKey:Data,event:@escaping @Sendable (PlankPencilEvent)->Void) throws {
        self.event=event; Self.made.append(self)
    }
    func start() {}; func approve() {}; func close() { closed=true }
    func offer(_ message:PlankPencilMessage)->Bool { sent.append(message); return !closed }
    func sessionBinding(_ callback:@escaping @Sendable (PlankPencilSessionBinding?)->Void) { callback(binding) }
}
enum PlankPencilImagePeerEvent:Sendable { case ready,frame(PlankPencilImageFrame),ended(String) }
@MainActor final class PlankPencilImagePeer {
    static var made:[PlankPencilImagePeer]=[]
    let binding:PlankPencilSessionBinding
    let event:@Sendable (PlankPencilImagePeerEvent)->Void
    var closed=false
    init(connection:NWConnection,privateKey:Data,binding:PlankPencilSessionBinding,initiator:Bool,
         event:@escaping @Sendable (PlankPencilImagePeerEvent)->Void) throws {
        self.binding=binding; self.event=event; Self.made.append(self)
    }
    func start() {}; func close() { closed=true }
}
@MainActor final class PlankIPadPencilImageView {
    private(set) var image:CGImage?, intensity=0.0
    func present(_ image:CGImage?,intensity:Double) { self.image=image; self.intensity=intensity }
}
enum PlankPencilReferenceImageCodec {
    static func decode(_ data:Data,expectedWidth:Int,expectedHeight:Int)throws->CGImage {
        let provider=CGDataProvider(data:Data(repeating:127,count:expectedWidth*expectedHeight*4) as CFData)!
        return CGImage(width:expectedWidth,height:expectedHeight,bitsPerComponent:8,bitsPerPixel:32,
            bytesPerRow:expectedWidth*4,space:CGColorSpaceCreateDeviceRGB(),
            bitmapInfo:CGBitmapInfo(rawValue:CGImageAlphaInfo.noneSkipLast.rawValue),provider:provider,
            decode:nil,shouldInterpolate:false,intent:.defaultIntent)!
    }
}
struct PlankIPadPencilPadSettings:Equatable {
    static func load()->Self { .init() }; var validated:Self { self }; func save() {}
}
@MainActor final class PlankIPadPencilPadView {
    var onRetire:(()->Void)?, onLayout:(()->Void)?
    var resets=0
    func retireForTransition() { onRetire?() }
    func resetViewAfterTransition(preservingView:Bool=false) { if !preservingView { resets += 1 } }
    func setNeedsLayout() {}; func layoutIfNeeded() { onLayout?() }
}
'''

def main():
    parser=argparse.ArgumentParser(description=__doc__); parser.add_argument('--output',type=pathlib.Path)
    args=parser.parse_args(); out=args.output or pathlib.Path(tempfile.mkdtemp(prefix='plank-image-sharing-'))
    out.mkdir(parents=True,exist_ok=True)
    relay=(REPO/'ios-native/Sources/PlankIPadPencilRelay.swift').read_text()
    image=(REPO/'ios-native/Sources/PlankIPadPencilImage.swift').read_text()
    bodies={
        'relay':extract.declaration(relay,'@MainActor final class PlankIPadPencilRelay:'),
        'receiver':extract.declaration(image,'@MainActor final class PlankIPadPencilImageReceiver:'),
        'weakView':extract.declaration(image,'@MainActor private final class PlankIPadPencilImageWeakView'),
        'workers':image[image.index('private struct PlankIPadPencilDecodedImage:'):],
    }
    fixture=out/'ProductionExtract.swift'
    fixture.write_text(SUPPORT+'\n'+'\n'.join(bodies.values())+'\n'+(REPO/'ios-native/Tests/AutomaticImageChecks.swift').read_text())
    inputs=[REPO/'apple-native/Shared/PlankControlKeyOwnership.swift',REPO/'apple-native/Shared/PlankPencilImageWire.swift']
    cmd=['xcrun','swiftc','-swift-version','6','-strict-concurrency=complete','-Onone','-parse-as-library',str(fixture),*map(str,inputs),'-o',str(out/'Checks')]
    compiled=subprocess.run(cmd,capture_output=True,text=True); (out/'compile.log').write_text(compiled.stdout+compiled.stderr)
    if compiled.returncode: print(compiled.stdout+compiled.stderr); raise SystemExit(compiled.returncode)
    result=subprocess.run([str(out/'Checks')],capture_output=True,text=True); (out/'run.log').write_text(result.stdout+result.stderr)
    checks=json.loads(result.stdout) if result.returncode==0 else {}
    receipt={'status':'PASS' if result.returncode==0 else 'FAIL','checks':checks,'command':cmd,
        'exactBodies':{k:hashlib.sha256(v.encode()).hexdigest() for k,v in bodies.items()},
        'sources':{str(p.relative_to(REPO)):hashlib.sha256(p.read_bytes()).hexdigest() for p in inputs+[REPO/'ios-native/Sources/PlankIPadPencilRelay.swift',REPO/'ios-native/Sources/PlankIPadPencilImage.swift',pathlib.Path(__file__),REPO/'ios-native/Tests/AutomaticImageChecks.swift']},
        'limits':['Network listeners, authenticated peer events, codec and UIKit view are stand-ins; wire crypto unchanged and separately qualified.',
                  'Actual relay, image receiver, decode and mailbox bodies; no physical OS, delivery, latency or Host receipt acceptance.']}
    (out/'verification.json').write_text(json.dumps(receipt,indent=2)+'\n'); print(result.stdout+result.stderr)
    raise SystemExit(result.returncode)
if __name__=='__main__': main()
