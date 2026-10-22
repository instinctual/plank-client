import Foundation
import CoreGraphics

@main enum PencilPadSettingsChecks {
    static func main() {
        let bounds = CGRect(x:0,y:0,width:1032,height:832)
        var settings = PlankIPadPencilPadSettings()
        let original = PlankIPadViewport(bounds:bounds.insetBy(dx:16,dy:16),width:1920,height:1080)!
        precondition(settings.viewport(bounds:bounds,width:1920,height:1080)!.rect == original.rect,
                     "Default mapping must preserve the accepted aspect-fit pad")
        settings.mapping = .fullPad
        settings.left = 0.1; settings.right = 0.2; settings.top = 0.05; settings.bottom = 0.1
        let full = settings.viewport(bounds:bounds,width:1920,height:1080)!
        precondition(abs(full.rect.minX - 116) < 0.001 && abs(full.rect.minY - 56) < 0.001)
        precondition(abs(full.rect.width - 700) < 0.001 && abs(full.rect.height - 680) < 0.001)
        for (point,x,y) in [(CGPoint(x:116,y:56),0,0), (CGPoint(x:816,y:56),1919,0),
                            (CGPoint(x:116,y:736),0,1079), (CGPoint(x:816,y:736),1919,1079)] {
            let mapped = full.map(point)!
            precondition(mapped.x == x && mapped.y == y, "Each custom-area corner maps to the desktop corner")
        }
        precondition(full.normalized(CGPoint(x:115,y:56)) == nil, "Margins reject fresh contacts")
        precondition(full.normalized(CGPoint(x:0,y:0),held:true)!.x == 0, "Existing drag clamps at the active edge")
        settings.mapping = .matchDesktop
        let fit = settings.viewport(bounds:bounds,width:1920,height:1080)!
        precondition(abs(fit.rect.width - 700) < 0.001 && abs(fit.rect.height - 393.75) < 0.001)
        precondition(abs(fit.rect.minY - 199.125) < 0.001 && abs(fit.rect.minX - 116) < 0.001)
        precondition(fit.normalized(CGPoint(x:116,y:56)) == nil, "Aspect letterboxes remain inactive")
        let portrait = settings.viewport(bounds:CGRect(x:0,y:0,width:832,height:1032),width:1920,height:1080)!
        let center = portrait.normalized(CGPoint(x:portrait.rect.midX,y:portrait.rect.midY))!
        precondition(center.x == 0.5 && center.y == 0.5)
        // Real production pen policy with changed pad geometry: retire the old
        // contact, reject its late motion, then admit a fresh corner down.
        var pen = PlankIPadPencilPolicy()
        precondition(pen.beginContact(point:CGPoint(x:full.rect.midX,y:full.rect.midY),viewport:full,
            timestamp:1,force:2,maximumForce:4,altitude:.pi/2,azimuth:0) != nil)
        precondition(pen.retire().map(\.phase) == [.cancel,.leave])
        precondition(pen.sample(.move,point:CGPoint(x:fit.rect.midX,y:fit.rect.midY),viewport:fit,
            timestamp:2,force:2,maximumForce:4,altitude:.pi/2,azimuth:0) == nil)
        let down = pen.beginContact(point:CGPoint(x:fit.rect.minX,y:fit.rect.minY),viewport:fit,
            timestamp:3,force:2,maximumForce:4,altitude:.pi/2,azimuth:0)!.last!
        precondition(down.x == 0 && down.y == 0 && down.pressureOrDistance == 0.5)
        settings.left = .nan; settings.right = 2; settings.top = -1; settings.bottom = .infinity
        settings.glow = .nan
        let bounded = settings.validated
        precondition(bounded.left == 0 && bounded.right == 0.4 && bounded.top == 0 && bounded.bottom == 0)
        precondition(bounded.glow == 0.2 && bounded.viewport(bounds:bounds,width:1920,height:1080) != nil)
        settings.left = 0.4; settings.right = 0.4; settings.top = 0.4; settings.bottom = 0.4
        precondition(settings.viewport(bounds:bounds,width:1920,height:1080)!.rect.width > 0)
        precondition(settings.viewport(bounds:CGRect(x:0,y:0,width:32,height:832),width:1920,height:1080) == nil)
        precondition(settings.viewport(bounds:CGRect(x:0,y:0,width:CGFloat.nan,height:832),width:1920,height:1080) == nil)
        precondition(settings.viewport(bounds:bounds,width:1,height:1080) == nil)
        settings.glow = 0; precondition(settings.fillWhite < 0.02)
        settings.glow = 1; precondition(settings.fillWhite <= 0.1)
        let suite = "plank.pencil.pad.checks." + UUID().uuidString
        let storage = UserDefaults(suiteName:suite)!
        defer { storage.removePersistentDomain(forName:suite) }
        precondition(PlankIPadPencilPadSettings.load(from:storage) == .init())
        settings.tone = .warmGray; settings.save(to:storage)
        precondition(PlankIPadPencilPadSettings.load(from:storage) == settings.validated)
        // Existing pad preferences must survive adding palette order.
        let legacy = Data(#"{"mapping":"fullPad","left":0.1,"right":0.2,"top":0.05,"bottom":0.1,"tone":"warmGray","glow":0.3}"#.utf8)
        let migrated = try! JSONDecoder().decode(PlankIPadPencilPadSettings.self,from:legacy)
        precondition(migrated.mapping == .fullPad && migrated.left == 0.1 && migrated.glow == 0.3)
        precondition(migrated.shortcuts == [.shift,.control,.option,.command,.space])
        settings.shortcutOrder = [0x20,0x11,0x11,0xFFFF,0x5B]
        precondition(settings.validated.shortcuts == [.space,.control,.command,.shift,.option])
        settings.save(to:storage)
        precondition(PlankIPadPencilPadSettings.load(from:storage).shortcuts == settings.validated.shortcuts)
        storage.set(Data("invalid".utf8),forKey:PlankIPadPencilPadSettings.storageKey)
        precondition(PlankIPadPencilPadSettings.load(from:storage) == .init())
        print("PASS Pencil pad: asymmetric margins, both mappings, edge admission, rotation geometry, fresh contact, bounded settings, legacy migration and persisted shortcut order")
    }
}
