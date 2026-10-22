import Foundation
import CoreGraphics

@main enum CustomControlChecks {
    static func main() throws {
        try modelChecks()
        labelChecks()
        geometryChecks()
        chromeReservationChecks()
        ownershipChecks()
        failedAdmissionChecks()
        receiverChecks()
        print("PASS custom controls: bounded saved layouts, legacy migration, independent Space size, reference/live preview parity, translated snapped groups around measured chrome, explicit unavailable/recoverable fit, stable gutter/alignment/size snapping, anchored resizing, finite bindings, overlapping ownership, physical repeat, combo order, atomic rejection, fail-closed input epochs and stale-change protection")
    }
    private static func labelChecks() {
        let source = PlankControlLayout.defaults().controls.first { $0.binding.code == 0x20 }!
        var duplicate = source
        duplicate.id = UUID()
        duplicate.setBinding(.init(code:0x11))
        precondition(duplicate.label == "Ctrl" && source.label == "Space",
                     "A duplicated key adopts its new key name without renaming the source")
        precondition(duplicate.portrait == source.portrait && duplicate.landscape == source.landscape,
                     "Changing a key cannot resize or move its control")
        duplicate.setBinding(.init(code:0x5A,modifiers:3))
        precondition(duplicate.label == "Shift + Ctrl + Z")
        duplicate.label = "Undo"
        duplicate.setBinding(.init(code:0x59,modifiers:8))
        precondition(duplicate.label == "Undo", "An artist's custom action name survives rebinding")
        duplicate.useBindingLabel()
        precondition(duplicate.label == "Command + Y")
        duplicate.setBinding(.init(code:0x58))
        precondition(duplicate.label == "X", "Using the key name resumes automatic naming")
        var command = PlankControlLayout.defaults().controls.first { $0.binding.code == 0x5B }!
        precondition(command.label == "⌘")
        command.setBinding(.init(code:0x11))
        precondition(command.label == "Ctrl", "Legacy modifier glyphs remain automatic names")
        let valid = duplicate
        duplicate.setBinding(.init(code:0xFFFF))
        precondition(duplicate == valid, "Invalid rebinding cannot mutate a saved control")
        for entry in PlankControlKeyCatalog.entries {
            for mask in UInt8(0)...15 {
                var generated = source
                generated.setBinding(.init(code:entry.code,modifiers:mask))
                precondition(generated.isValid && generated.label.count <= PlankCustomControl.maximumLabelLength,
                             "Every supported generated shortcut name fits the saved label limit")
                generated.setBinding(.init(code:0x20))
                precondition(generated.label == "Space", "Truncated generated combo names continue following the key")
            }
        }
    }
    private static func geometryChecks() {
        func close(_ a: CGFloat, _ b: CGFloat) -> Bool { abs(a - b) < 0.000_01 }
        func same(_ a: CGRect, _ b: CGRect) -> Bool {
            close(a.minX,b.minX) && close(a.minY,b.minY) && close(a.width,b.width) && close(a.height,b.height)
        }
        // A wide key near an edge is clamped in the actual surface, then scaled.
        // Resolving its point dimensions directly in the sheet changes its center.
        for referenceSize in [CGSize(width:1100,height:730),CGSize(width:730,height:1100)] {
            for available in [CGSize(width:430,height:620),CGSize(width:850,height:450)] {
                let geometry = PlankControlPreviewGeometry(referenceSize:referenceSize,availableSize:available)
                let bounds = geometry.referenceBounds
                precondition(bounds.size == referenceSize && geometry.scale > 0)
                precondition(geometry.previewFrame.width <= available.width - 32 + 0.000_01)
                precondition(geometry.previewFrame.height <= available.height - 32 + 0.000_01)
                precondition(close(geometry.previewFrame.midX,available.width / 2)
                    && close(geometry.previewFrame.midY,available.height / 2))
                for center in [0.0,0.02,0.5,0.98,1.0] {
                    let key = PlankControlPlacement(x:center,y:1-center,width:480,height:160)
                    let live = key.frame(in:bounds)
                    let preview = geometry.preview(frame:live)
                    let decoded = CGRect(x:preview.minX / geometry.scale,y:preview.minY / geometry.scale,
                        width:preview.width / geometry.scale,height:preview.height / geometry.scale)
                    precondition(same(live,decoded),"Preview must be the same actual key rectangle at uniform scale")
                    precondition(close(preview.width,480 * geometry.scale) && close(preview.height,160 * geometry.scale))
                    let clamped = clampedPlacement(key,in:bounds)
                    precondition(same(clamped.frame(in:bounds),live) && clamped.width == 480 && clamped.height == 160)
                }
                let moved = geometry.referenceTranslation(CGSize(width:geometry.scale * 76,height:geometry.scale * -52))
                precondition(close(moved.width,76) && close(moved.height,-52))
            }
        }
        let invalid = PlankControlPreviewGeometry(referenceSize:CGSize(width:0,height:CGFloat.nan),availableSize:CGSize(width:400,height:500))
        precondition(invalid.previewFrame == .zero && invalid.referenceBounds == .zero && invalid.scale == 1)
        let invalidBounds = CGRect(x:CGFloat.nan,y:0,width:400,height:500)
        let safe = PlankControlPlacement(x:0.5,y:0.5,width:200,height:52)
        precondition(clampedPlacement(safe,in:invalidBounds) == safe)
        precondition(PlankControlSnap.move(placement:safe,in:invalidBounds,peers:[]).placement == safe)
        let bounds = CGRect(x:10,y:20,width:1000,height:800)
        let peer = CGRect(x:210,y:220,width:76,height:52)
        let proposed = PlankControlPlacement(frame:CGRect(x:296,y:224,width:76,height:52),in:bounds)
        let row = PlankControlSnap.move(placement:proposed,in:bounds,peers:[peer])
        let rowFrame = row.placement.frame(in:bounds)
        precondition(same(rowFrame,CGRect(x:292,y:220,width:76,height:52)),"Neighboring buttons snap into a six-point-gutter row")
        precondition(close(rowFrame.minX - peer.maxX,6) && row.placement.width == proposed.width)
        precondition(row.guides.count == 2 && row.guides.contains { $0.kind == .gutter })
        let column = PlankControlSnap.move(placement:PlankControlPlacement(frame:CGRect(x:214,y:281,width:76,height:52),in:bounds),
                                          in:bounds,peers:[peer])
        precondition(same(column.placement.frame(in:bounds),CGRect(x:210,y:278,width:76,height:52)),"Aligned columns retain the same gutter")
        let edge = PlankControlSnap.move(placement:.init(x:0,y:0,width:200,height:52),in:bounds,peers:[])
        precondition(same(edge.placement.frame(in:bounds),CGRect(x:16,y:26,width:200,height:52)))
        // An imported or orientation-changed edge placement can retain x=1
        // while its actual center is clamped half a key-width inside the edge.
        // A drag starts from that resolved center, so the first inward movement
        // changes the live frame rather than spending 100 points in a dead zone.
        let savedEdge = PlankControlPlacement(x:1,y:0.5,width:200,height:52)
        let resolvedEdge = clampedPlacement(savedEdge,in:bounds)
        var translatedEdge = resolvedEdge
        translatedEdge.x -= 2 / Double(bounds.width)
        let firstInwardMove = PlankControlSnap.move(placement:translatedEdge,in:bounds,peers:[],threshold:0)
        precondition(close(firstInwardMove.placement.frame(in:bounds).midX,savedEdge.frame(in:bounds).midX - 2),
                     "An edge-clamped key moves inward from its actual center immediately")
        var unresolved = savedEdge
        unresolved.x -= 2 / Double(bounds.width)
        precondition(same(unresolved.frame(in:bounds),savedEdge.frame(in:bounds)),
                     "The regression fixture must distinguish the raw normalized anchor from its displayed center")
        let distant = CGRect(x:310,y:70,width:76,height:52)
        let free = PlankControlPlacement(frame:CGRect(x:312,y:520,width:76,height:52),in:bounds)
        let ungrabbed = PlankControlSnap.move(placement:free,in:bounds,peers:[distant])
        precondition(same(ungrabbed.placement.frame(in:bounds),free.frame(in:bounds)) && ungrabbed.guides.isEmpty,
                     "A faraway row must not pull the key into a column")
        let farPeer = CGRect(x:600,y:600,width:76,height:52)
        let ordered = PlankControlSnap.move(placement:proposed,in:bounds,peers:[peer,farPeer])
        let reversed = PlankControlSnap.move(placement:proposed,in:bounds,peers:[farPeer,peer])
        precondition(ordered == reversed,"Peer enumeration must not change snapping")
        var stable = row
        for _ in 0..<20 {
            stable = PlankControlSnap.move(placement:stable.placement,in:bounds,peers:[peer])
            precondition(same(stable.placement.frame(in:bounds),rowFrame),"Repeated snap may not drift")
        }
        let originalFrame = CGRect(x:110,y:120,width:76,height:52)
        let original = PlankControlPlacement(frame:originalFrame,in:bounds)
        let equalSizePeer = CGRect(x:192,y:120,width:100,height:52)
        let resized = PlankControlSnap.resize(original:original,corner:3,translation:CGSize(width:21,height:2),
                                              in:bounds,peers:[equalSizePeer])
        precondition(same(resized.placement.frame(in:bounds),CGRect(x:110,y:120,width:100,height:52)),
                     "Resize can match a neighboring button while keeping the opposite corner fixed")
        var stableResize = resized
        for _ in 0..<20 {
            stableResize = PlankControlSnap.resize(original:stableResize.placement,corner:3,translation:.zero,
                in:bounds,peers:[equalSizePeer])
            precondition(same(stableResize.placement.frame(in:bounds),resized.placement.frame(in:bounds)),
                         "Equal-size resizing cannot drift on repeated zero translations")
        }
        for corner in 0...3 {
            let resize = PlankControlSnap.resize(original:original,corner:corner,translation:CGSize(width:12,height:9),
                                                 in:bounds,peers:[],threshold:0)
            let result = resize.placement.frame(in:bounds)
            precondition(close(corner % 2 == 0 ? result.maxX : result.minX,
                               corner % 2 == 0 ? originalFrame.maxX : originalFrame.minX))
            precondition(close(corner < 2 ? result.maxY : result.minY,
                               corner < 2 ? originalFrame.maxY : originalFrame.minY),"Every resize anchors the diagonally opposite corner")
        }
        let longKey = PlankControlPlacement(frame:CGRect(x:380,y:550,width:200,height:52),in:bounds)
        let smallerPeer = CGRect(x:298,y:550,width:76,height:52)
        let longMoved = PlankControlSnap.move(placement:longKey,in:bounds,peers:[smallerPeer])
        precondition(longMoved.placement.width == 200 && longMoved.placement.height == 52,
                     "Moving a wide key beside a smaller key cannot resize either")
    }
    private static func chromeReservationChecks() {
        let bounds = CGRect(x:0,y:0,width:1180,height:820)
        func key(_ frame: CGRect) -> PlankCustomControl {
            let placement = PlankControlPlacement(frame:frame,in:bounds)
            return PlankCustomControl(id:UUID(),label:"Space",binding:.init(code:0x20),behavior:.hold,
                                      portrait:placement,landscape:placement)
        }
        func overlaps(_ a: CGRect,_ b: CGRect) -> Bool {
            let value = a.intersection(b)
            return !value.isNull && value.width > 0.000_01 && value.height > 0.000_01
        }
        let keys = [key(CGRect(x:1000,y:6,width:160,height:52)),
                    key(CGRect(x:1000,y:64,width:160,height:52)),
                    key(CGRect(x:1000,y:122,width:160,height:52)),
                    key(CGRect(x:1000,y:650,width:160,height:52)),
                    key(CGRect(x:1000,y:708,width:160,height:52))]
        let original = keys
        let unknown = PlankControlChromeGeometry.unmeasured(size:bounds.size)
        let unknownResult = PlankControlPlacementResolver.resolve(controls:keys,landscape:true,chrome:unknown)
        precondition(unknown.usableBounds == .zero && unknownResult.frames.isEmpty
            && unknownResult.unavailableIDs == Set(keys.map(\.id)),
            "An orientation without a chrome measurement must not infer unrestricted placement")
        let visible = PlankControlChromeGeometry(bounds:bounds,
            usableBounds:CGRect(x:0,y:66,width:1180,height:734))
        let floating = CGRect(x:1020,y:12,width:148,height:44)
        let hidden = PlankControlChromeGeometry(bounds:bounds,
            usableBounds:CGRect(x:0,y:0,width:1180,height:800),reservedRects:[floating])
        for chrome in [visible,hidden] {
            let resolved = PlankControlPlacementResolver.resolve(controls:keys,landscape:true,chrome:chrome)
            precondition(resolved.unavailableIDs.isEmpty && resolved.frames.count == keys.count)
            precondition(keys == original,"Passive reservation must not rewrite saved anchors or point sizes")
            for control in keys {
                let frame = resolved.frames[control.id]!
                precondition(frame.width == control.landscape.width && frame.height == control.landscape.height)
                precondition(chrome.usableBounds.contains(frame))
                precondition(chrome.reservedRects.allSatisfy { !overlaps(frame,$0) },"No key may occupy actual chrome")
            }
            for pair in [(0,1),(1,2),(3,4)] {
                precondition(abs(resolved.frames[keys[pair.1].id]!.minY - resolved.frames[keys[pair.0].id]!.maxY - 6) < 0.000_01,
                             "Chrome translation must preserve the existing six-point snapped group")
            }
            precondition(resolved == PlankControlPlacementResolver.resolve(controls:Array(keys.reversed()),landscape:true,chrome:chrome),
                         "Saved array order must not change deterministic chrome placement")
            let geometry = PlankControlPreviewGeometry(referenceSize:bounds.size,availableSize:CGSize(width:700,height:500))
            for frame in resolved.frames.values {
                let preview = geometry.preview(frame:frame)
                precondition(abs(preview.minX / geometry.scale - frame.minX) < 0.000_01
                    && abs(preview.minY / geometry.scale - frame.minY) < 0.000_01,
                    "Editor and live overlay resolve the same full-window frame before preview scaling")
            }
            // Only an explicit geometry edit adopts displayed positions into
            // its draft, with the old raw arrangement kept for one-step Undo.
            let undo = keys
            var draft = keys
            for index in draft.indices {
                draft[index].landscape = PlankControlPlacement(frame:resolved.frames[draft[index].id]!,in:bounds)
            }
            precondition(draft.map(\.landscape.width) == keys.map(\.landscape.width))
            precondition(PlankControlPlacementResolver.resolve(controls:draft,landscape:true,chrome:chrome) == resolved,
                         "Materializing visible geometry cannot shift it again")
            draft = undo
            precondition(draft == original,"Undo/Cancel restores exact pre-edit raw anchors")
        }
        let consent = CGRect(x:310,y:80,width:560,height:180)
        let consentChrome = PlankControlChromeGeometry(bounds:bounds,usableBounds:visible.usableBounds,reservedRects:[consent])
        let center = key(CGRect(x:500,y:104,width:200,height:52))
        let consentResult = PlankControlPlacementResolver.resolve(controls:[center],landscape:true,chrome:consentChrome)
        precondition(consentResult.frames[center.id] != nil && !overlaps(consentResult.frames[center.id]!,consent),
                     "Measured foreground consent stays clear without resizing keys")
        let approved = PlankControlPlacementResolver.resolve(controls:[center],landscape:true,chrome:visible)
        precondition(approved.frames[center.id] == center.landscape.frame(in:bounds),
                     "Removing the consent obstacle restores the original safe placement")
        let overlapping = [key(CGRect(x:1000,y:6,width:160,height:52)),key(CGRect(x:1050,y:24,width:100,height:52))]
        let overlapResult = PlankControlPlacementResolver.resolve(controls:overlapping,landscape:true,chrome:visible)
        let a = overlapResult.frames[overlapping[0].id]!, b = overlapResult.frames[overlapping[1].id]!
        precondition(overlaps(a,b) && abs(b.minX - a.minX - 50) < 0.000_01 && abs(b.minY - a.minY - 18) < 0.000_01,
                     "Intentional control overlap remains intact inside a translated group")
        let tiny = PlankControlChromeGeometry(bounds:CGRect(x:0,y:0,width:200,height:150),
            usableBounds:CGRect(x:0,y:66,width:200,height:64))
        var large = key(CGRect(x:100,y:100,width:480,height:240))
        let before = large
        let unavailable = PlankControlPlacementResolver.resolve(controls:[large],landscape:true,chrome:tiny)
        precondition(unavailable.frames.isEmpty && unavailable.unavailableIDs == [large.id] && large == before,
                     "Impossible fit is explicit and never silently shrinks or deletes a saved control")
        large.landscape.width = 100; large.landscape.height = 44
        let recovered = PlankControlPlacementResolver.resolve(controls:[large],landscape:true,chrome:tiny)
        precondition(recovered.unavailableIDs.isEmpty && recovered.frames[large.id]!.size == CGSize(width:100,height:44),
                     "The unavailable saved control can recover through an explicit editor size change")
        let invalid = PlankControlChromeGeometry(bounds:bounds,usableBounds:CGRect(x:CGFloat.nan,y:0,width:100,height:100),
                                                 reservedRects:[.null,CGRect(x:0,y:0,width:CGFloat.infinity,height:44)])
        precondition(PlankControlPlacementResolver.resolve(controls:keys,landscape:true,chrome:invalid).unavailableIDs.count == keys.count)
        precondition(invalid.reservedRects.isEmpty)
    }
    private static func modelChecks() throws {
        // The accepted mapping table is the authority for the assignable catalog.
        var mapped = Set<UInt16>()
        for usage in 0...255 {
            for mode in [KeyboardFunctionKeyMode.pc,.appleExtended] {
                if let code = plankIPadVirtualKey(for:usage,functionKeyMode:mode) { mapped.insert(code) }
            }
        }
        precondition(mapped == Set(PlankControlKeyCatalog.entries.map(\.code)))
        precondition(!PlankControlBinding(code:0xFFFF).isValid)
        precondition(!PlankControlBinding(code:0x41,modifiers:16).isValid)
        precondition(PlankControlBinding(code:0x5A,modifiers:3).title == "Shift + Ctrl + Z")
        let original = PlankControlLayout.defaults()
        let reordered = PlankControlLayout.defaults(shortcuts:[.space,.command,.shift,.option,.control])
        let before = original.controls.first { $0.binding.code == 0x20 }!
        let after = reordered.controls.first { $0.binding.code == 0x20 }!
        precondition(before.portrait == after.portrait && before.landscape == after.landscape)
        precondition(after.landscape.width == 200 && reordered.controls[0].binding.code == 0x20)
        precondition(original.isValid && reordered.isValid)
        var moved = after
        moved.landscape.x = 0.92; moved.landscape.y = 0.08
        precondition(moved.landscape.width == 200 && moved.landscape.height == 52)
        let bounds = CGRect(x:10,y:20,width:1000,height:800)
        let atEdge = PlankControlPlacement(x:1,y:0,width:200,height:52).frame(in:bounds)
        precondition(atEdge == CGRect(x:810,y:20,width:200,height:52))
        let small = PlankControlPlacement(x:0,y:0,width:200,height:52).frame(in:CGRect(x:0,y:0,width:80,height:30))
        precondition(small.width == 80 && small.height == 44, "Never shrink hit targets below 44 points")
        let placement = PlankControlPlacement(frame:CGRect(x:310,y:220,width:200,height:52),in:bounds)
        let restored = placement.frame(in:bounds)
        precondition(abs(restored.minX - 310) < 0.0001 && abs(restored.minY - 220) < 0.0001
            && restored.width == 200 && restored.height == 52)
        precondition(PlankControlPlacement(x:.nan,y:0,width:44,height:44).frame(in:bounds) == .zero)
        precondition(!PlankControlPlacement(x:2,y:0,width:44,height:44).isValid)
        precondition(!PlankControlPlacement(x:0,y:0,width:43,height:44).isValid)
        let copy = original.copied()
        precondition(copy.id != original.id && Set(copy.controls.map(\.id)).isDisjoint(with:Set(original.controls.map(\.id))))
        precondition(copy.controls.map(\.landscape) == original.controls.map(\.landscape))
        var invalid = original
        invalid.controls.append(invalid.controls[0])
        precondition(!invalid.isValid, "Repeated control IDs must fail validation")
        invalid = original; invalid.controls[0].label = String(repeating:"a",count:41)
        precondition(!invalid.isValid)
        invalid = original; invalid.controls[0].landscape.width = .infinity
        precondition(!invalid.isValid)
        var library = PlankCustomControlLibrary(layouts:[original])
        let secondID = library.duplicate(id:original.id)!
        precondition(library.layouts.count == 2 && library.selectedID == secondID)
        precondition(library.remove(id:secondID) && library.selectedID == original.id)
        precondition(!library.remove(id:original.id), "Keep one available layout")
        precondition(!library.select(id:UUID()) && !library.upsert(layout:invalid))
        for index in 1..<PlankCustomControlLibrary.maximumLayouts {
            precondition(library.upsert(layout:original.copied(name:"Layout \(index)")))
        }
        precondition(!library.upsert(layout:original.copied()))
        let decoded = try JSONDecoder().decode(PlankCustomControlLibrary.self,from:JSONEncoder().encode(library))
        precondition(decoded == library)
        let suite = "plank.custom-control.checks." + UUID().uuidString
        let storage = UserDefaults(suiteName:suite)!
        defer { storage.removePersistentDomain(forName:suite) }
        var legacy = PlankIPadPencilPadSettings()
        legacy.left = 0.2; legacy.glow = 0.35; legacy.shortcutOrder = [0x20,0x11,0x11,0xFFFF,0x5B]
        legacy.save(to:storage)
        let legacyData = storage.data(forKey:PlankIPadPencilPadSettings.storageKey)
        let migrated = PlankCustomControlLibrary.load(from:storage)
        precondition(migrated.selectedLayout.controls.map(\.binding.code) == [0x20,0x11,0x5B,0x10,0x12])
        precondition(migrated.selectedLayout.controls[0].landscape.width == 200)
        precondition(storage.data(forKey:PlankIPadPencilPadSettings.storageKey) == legacyData)
        precondition(PlankCustomControlLibrary.load(from:storage) == migrated, "Stable migrated IDs survive reopening")
        library.save(to:storage)
        precondition(PlankCustomControlLibrary.load(from:storage) == library)
        var bad = library; bad.selectedID = UUID()
        storage.set(try JSONEncoder().encode(bad),forKey:PlankCustomControlLibrary.storageKey)
        precondition(PlankCustomControlLibrary.load(from:storage).isValid)
    }
    private static func ownershipChecks() {
        typealias Event = PlankControlKeyEvent
        var ledger = PlankControlKeyOwnership()
        let first = UUID(), second = UUID()
        let combo = PlankControlBinding(code:0x5A,modifiers:3)
        let down = ledger.previewBegin(owner:.control(first),binding:combo)!
        precondition(ledger.heldCodes.isEmpty, "Preview may not acquire ownership")
        precondition(down.events == [Event(code:0x10,pressed:true,modifiers:1),
            Event(code:0x11,pressed:true,modifiers:3),Event(code:0x5A,pressed:true,modifiers:3)])
        precondition(ledger.apply(down))
        precondition(!ledger.apply(down), "A preview is applied once")
        precondition(ledger.begin(owner:.control(second),binding:combo,admit:{ $0.isEmpty }))
        precondition(ledger.end(owner:.control(first),admit:{ $0.isEmpty }))
        let up = ledger.previewEnd(owner:.control(second))!
        precondition(up.events == [Event(code:0x5A,pressed:false,modifiers:3),
            Event(code:0x11,pressed:false,modifiers:1),Event(code:0x10,pressed:false,modifiers:0)])
        precondition(ledger.apply(up) && ledger.heldCodes.isEmpty)
        precondition(!ledger.begin(owner:.control(first),binding:combo,admit:{ _ in false }))
        precondition(ledger.heldCodes.isEmpty && ledger.controlOwners.isEmpty, "Rejected admission is wholly inert")
        precondition(ledger.begin(owner:.control(first),binding:combo,admit:{ _ in true }))
        let tap = ledger.previewTap(binding:.init(code:0x41,modifiers:2))!
        precondition(tap.events == [Event(code:0x41,pressed:true,modifiers:3),Event(code:0x41,pressed:false,modifiers:3)])
        precondition(ledger.apply(tap) && ledger.heldCodes == [0x10,0x11,0x5A])
        precondition(ledger.tap(binding:combo,admit:{ $0.isEmpty }), "Do not manufacture a key release for a held trigger")
        precondition(ledger.end(owner:.control(first),admit:{ _ in true }))
        let hardware = ledger.previewHardware(code:0x20,pressed:true,modifiers:0)!
        precondition(ledger.apply(hardware))
        precondition(ledger.begin(owner:.control(first),binding:.init(code:0x20),admit:{ $0.isEmpty }))
        let repeatKey = ledger.previewHardware(code:0x20,pressed:true,modifiers:0)!
        precondition(repeatKey.events == [Event(code:0x20,pressed:true)])
        precondition(ledger.apply(repeatKey))
        let hardwareUp = ledger.previewHardware(code:0x20,pressed:false,modifiers:0)!
        precondition(hardwareUp.events.isEmpty && ledger.apply(hardwareUp))
        precondition(ledger.end(owner:.control(first),admit:{ $0 == [Event(code:0x20,pressed:false)] }))
        // Hardware + two fingers share Shift. Lifting either owner is balanced.
        precondition(ledger.apply(ledger.previewHardware(code:0x10,pressed:true,modifiers:1)!))
        precondition(ledger.begin(owner:.control(first),binding:.init(code:0x10),admit:{ $0.isEmpty }))
        precondition(ledger.begin(owner:.control(second),binding:.init(code:0x10),admit:{ $0.isEmpty }))
        precondition(ledger.retireControls(admit:{ $0.isEmpty }))
        precondition(ledger.controlOwners.isEmpty && ledger.heldCodes == [0x10])
        let hardwareShiftUp = ledger.previewHardware(code:0x10,pressed:false,modifiers:1)!
        precondition(hardwareShiftUp.events == [Event(code:0x10,pressed:false)], "Stale original flags cannot retain released Shift")
        precondition(ledger.apply(hardwareShiftUp))
        precondition(ledger.previewHardware(code:0x10,pressed:false,modifiers:0) == nil)
        precondition(ledger.begin(owner:.control(first),binding:.init(code:0x5A,modifiers:8),admit:{ _ in true }))
        let release = ledger.previewRetireAll()!
        precondition(release.events.map(\.code) == [0x5A,0x5B] && release.events.allSatisfy { !$0.pressed })
        precondition(ledger.apply(release) && ledger.heldCodes.isEmpty)
        let stale = ledger.previewBegin(owner:.control(first),binding:.init(code:0x41))!
        precondition(ledger.tap(binding:.init(code:0x42),admit:{ _ in true }))
        precondition(!ledger.apply(stale), "A delayed preview cannot overwrite a newer hold")
        var other = PlankControlKeyOwnership()
        let foreign = PlankControlKeyOwnership().previewBegin(owner:.control(UUID()),binding:.init(code:0x41))!
        precondition(!other.apply(foreign))
        precondition(ledger.previewBegin(owner:.control(UUID()),binding:.init(code:0xFFFF)) == nil)
        for _ in 0..<PlankControlKeyOwnership.maximumOwners {
            precondition(ledger.begin(owner:.control(UUID()),binding:.init(code:0x41),admit:{ _ in true }))
        }
        precondition(!ledger.begin(owner:.control(UUID()),binding:.init(code:0x42),admit:{ _ in true }))
        precondition(ledger.retireAll(admit:{ $0 == [Event(code:0x41,pressed:false)] }))
    }
    private static func receiverChecks() {
        var receiver = PlankPencilKeyOwnership()
        precondition(receiver.update(code:0x41,pressed:true,source:.pad)?.pressed == true)
        precondition(receiver.update(code:0x41,pressed:true,source:.local) == nil)
        precondition(receiver.update(code:0x41,pressed:true,source:.local)?.pressed == true, "Physical repeat remains available")
        precondition(receiver.update(code:0x41,pressed:false,source:.pad) == nil)
        precondition(receiver.update(code:0x41,pressed:false,source:.local)?.pressed == false)
        _ = receiver.update(code:0x11,pressed:true,source:.pad)
        _ = receiver.update(code:0x5A,pressed:true,source:.pad)
        precondition(receiver.retirePad().map(\.code) == [0x5A,0x11])
    }
    private static func failedAdmissionChecks() {
        final class StubClient {
            var accepts = true, closed = false
            var offers = 0, closes = 0
            var cleanup: [PlankControlKeyEvent] = []
            func offer(_ events: [PlankControlKeyEvent]) -> Bool {
                offers += 1
                return accepts && !closed
            }
            func close(_ retirement: [PlankControlKeyEvent]) {
                cleanup = retirement
                closes += 1; closed = true
            }
        }
        let client = StubClient(), owner = UUID()
        var session = PlankControlKeySession()
        let down = session.ownership.previewBegin(owner:.control(owner),binding:.init(code:0x20))!
        precondition(session.admit(down,offer:client.offer,closeOnRejection:client.close))
        client.accepts = false
        let up = session.ownership.previewEnd(owner:.control(owner))!
        precondition(!session.admit(up,offer:client.offer,closeOnRejection:client.close))
        precondition(!session.accepting && session.ownership.heldCodes.isEmpty
            && session.ownership.controlOwners.isEmpty && client.closed && client.closes == 1)
        precondition(client.cleanup == [.init(code:0x20,pressed:false)])
        let offers = client.offers
        precondition(!session.admit(down,offer:client.offer,closeOnRejection:client.close))
        precondition(client.offers == offers && client.closes == 1, "No retry loop or continuing stale epoch")
        client.closed = false; client.accepts = true; session.reopen()
        let fresh = session.ownership.previewBegin(owner:.control(owner),binding:.init(code:0x20))!
        precondition(fresh.events == [.init(code:0x20,pressed:true)], "Fresh connection cannot inherit the refused touch")
        precondition(session.admit(fresh,offer:client.offer,closeOnRejection:client.close))
        let duplicate = session.ownership.previewBegin(owner:.control(UUID()),binding:.init(code:0x20))!
        client.accepts = false
        let duplicateOffers = client.offers
        precondition(session.admit(duplicate,offer:client.offer,closeOnRejection:client.close))
        precondition(client.offers == duplicateOffers && client.closes == 1,
            "Ownership-only changes have no queue capacity requirement")
        let retirement = session.ownership.previewRetireAll()!
        precondition(!session.admit(retirement,offer:client.offer,closeOnRejection:client.close))
        precondition(client.closes == 2 && !session.accepting && session.ownership.heldCodes.isEmpty)
        session.reopen(); client.closed = false; client.accepts = true
        let hardware = session.ownership.previewHardware(code:0x41,pressed:true,modifiers:0)!
        precondition(session.admit(hardware,offer:client.offer,closeOnRejection:client.close))
        client.accepts = false
        let hardwareUp = session.ownership.previewHardware(code:0x41,pressed:false,modifiers:0)!
        precondition(!session.admit(hardwareUp,offer:client.offer,closeOnRejection:client.close))
        precondition(client.closes == 3 && !session.accepting && session.ownership.heldCodes.isEmpty)
    }
}
