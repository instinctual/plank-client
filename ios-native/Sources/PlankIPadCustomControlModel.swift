import Foundation
import CoreGraphics

/// Anchors belong to the whole available surface, independently of pen margins.
/// Each key keeps its own point size when reordered, copied or moved.
struct PlankControlPlacement: Codable, Equatable, Sendable {
    static let minimumDimension = 44.0
    static let maximumWidth = 480.0
    static let maximumHeight = 240.0
    var x: Double, y: Double, width: Double, height: Double
    init(x: Double, y: Double, width: Double, height: Double) {
        self.x = x; self.y = y; self.width = width; self.height = height
    }
    var isValid: Bool {
        x.isFinite && y.isFinite && width.isFinite && height.isFinite
            && (0...1).contains(x) && (0...1).contains(y)
            && (Self.minimumDimension...Self.maximumWidth).contains(width)
            && (Self.minimumDimension...Self.maximumHeight).contains(height)
    }
    func frame(in bounds: CGRect) -> CGRect {
        guard isValid, bounds.minX.isFinite, bounds.minY.isFinite,
              bounds.width.isFinite, bounds.height.isFinite,
              bounds.width > 0, bounds.height > 0 else { return .zero }
        let w = min(CGFloat(width),max(CGFloat(Self.minimumDimension),bounds.width))
        let h = min(CGFloat(height),max(CGFloat(Self.minimumDimension),bounds.height))
        func center(_ value: CGFloat, start: CGFloat, available: CGFloat, length: CGFloat) -> CGFloat {
            guard available >= length else { return start + available / 2 }
            return min(max(start + value * available,start + length / 2),start + available - length / 2)
        }
        return CGRect(x:center(CGFloat(x),start:bounds.minX,available:bounds.width,length:w) - w / 2,
                      y:center(CGFloat(y),start:bounds.minY,available:bounds.height,length:h) - h / 2,
                      width:w,height:h)
    }
    init(frame: CGRect, in bounds: CGRect) {
        func fraction(_ value: CGFloat, start: CGFloat, length: CGFloat) -> Double {
            guard value.isFinite, start.isFinite, length.isFinite, length > 0 else { return 0.5 }
            return min(max(Double((value - start) / length),0),1)
        }
        func dimension(_ value: CGFloat, maximum: Double) -> Double {
            value.isFinite ? min(max(Double(value),Self.minimumDimension),maximum) : Self.minimumDimension
        }
        self.init(x:fraction(frame.midX,start:bounds.minX,length:bounds.width),
                  y:fraction(frame.midY,start:bounds.minY,length:bounds.height),
                  width:dimension(frame.width,maximum:Self.maximumWidth),
                  height:dimension(frame.height,maximum:Self.maximumHeight))
    }
}

/// The editor is a uniformly scaled view of the actual available control surface.
/// Point-sized keys are resolved in that surface first, so a small sheet cannot
/// clamp their centers differently from the live overlay.
struct PlankControlPreviewGeometry: Equatable, Sendable {
    let referenceBounds: CGRect
    let previewFrame: CGRect
    let scale: CGFloat
    init(referenceSize: CGSize, availableSize: CGSize, inset: CGFloat = 16) {
        guard referenceSize.width.isFinite, referenceSize.height.isFinite,
              availableSize.width.isFinite, availableSize.height.isFinite,
              referenceSize.width > 0, referenceSize.height > 0,
              availableSize.width > 0, availableSize.height > 0 else {
            referenceBounds = .zero; previewFrame = .zero; scale = 1
            return
        }
        let padding = inset.isFinite ? max(0,inset) : 0
        let availableWidth = max(0,availableSize.width - padding * 2)
        let availableHeight = max(0,availableSize.height - padding * 2)
        referenceBounds = CGRect(origin:.zero,size:referenceSize)
        scale = max(0.000_001,min(availableWidth / referenceSize.width,availableHeight / referenceSize.height))
        let size = CGSize(width:referenceSize.width * scale,height:referenceSize.height * scale)
        previewFrame = CGRect(x:(availableSize.width - size.width) / 2,
                              y:(availableSize.height - size.height) / 2,width:size.width,height:size.height)
    }
    /// Coordinates are local to the preview, not its containing editor.
    func preview(frame: CGRect) -> CGRect {
        CGRect(x:(frame.minX - referenceBounds.minX) * scale,
               y:(frame.minY - referenceBounds.minY) * scale,
               width:frame.width * scale,height:frame.height * scale)
    }
    func referenceTranslation(_ translation: CGSize) -> CGSize {
        CGSize(width:translation.width / scale,height:translation.height / scale)
    }
}

/// Center clamping preserves the saved dimensions, including keys wider than a
/// temporarily small surface. It does not turn a moved key into another size.
func clampedPlacement(_ placement: PlankControlPlacement, in bounds: CGRect) -> PlankControlPlacement {
    guard placement.isValid, bounds.minX.isFinite, bounds.minY.isFinite,
          bounds.width.isFinite, bounds.height.isFinite,
          bounds.width > 0, bounds.height > 0 else { return placement }
    let frame = placement.frame(in:bounds)
    var next = placement
    next.x = Double((frame.midX - bounds.minX) / bounds.width)
    next.y = Double((frame.midY - bounds.minY) / bounds.height)
    return next
}

/// Transient local chrome uses whole-window coordinates, just like saved keys.
/// It never changes normalized anchors, the schema, or the remote pen viewport.
struct PlankControlChromeGeometry: Equatable, Sendable {
    let bounds: CGRect
    let usableBounds: CGRect
    let reservedRects: [CGRect]
    init(bounds: CGRect, usableBounds: CGRect, reservedRects: [CGRect] = []) {
        let window = Self.valid(bounds) ? bounds : .zero
        self.bounds = window
        let usable = Self.valid(usableBounds) ? usableBounds.intersection(window) : .zero
        self.usableBounds = Self.valid(usable) ? usable : .zero
        self.reservedRects = reservedRects.prefix(8).compactMap { frame in
            guard Self.valid(frame) else { return nil }
            let clipped = frame.intersection(window)
            return Self.valid(clipped) ? clipped : nil
        }
    }
    static func unrestricted(size: CGSize) -> Self {
        let bounds = CGRect(origin:.zero,size:size)
        return Self(bounds:bounds,usableBounds:bounds)
    }
    static func unmeasured(size: CGSize) -> Self {
        Self(bounds:CGRect(origin:.zero,size:size),usableBounds:.zero)
    }
    static func valid(_ rect: CGRect) -> Bool {
        !rect.isNull && rect.minX.isFinite && rect.minY.isFinite
            && rect.width.isFinite && rect.height.isFinite && rect.width > 0 && rect.height > 0
    }
    var shadedRects: [CGRect] {
        guard Self.valid(bounds) else { return [] }
        guard Self.valid(usableBounds) else { return [bounds] }
        return [CGRect(x:bounds.minX,y:bounds.minY,width:bounds.width,height:usableBounds.minY - bounds.minY),
                CGRect(x:bounds.minX,y:usableBounds.maxY,width:bounds.width,height:bounds.maxY - usableBounds.maxY),
                CGRect(x:bounds.minX,y:usableBounds.minY,width:usableBounds.minX - bounds.minX,height:usableBounds.height),
                CGRect(x:usableBounds.maxX,y:usableBounds.minY,width:bounds.maxX - usableBounds.maxX,height:usableBounds.height)]
            .filter(Self.valid) + reservedRects
    }
}

/// Preserve touching/snapped clusters and intentional overlaps, moving them as
/// one piece around local chrome. Separate groups never gain a new overlap.
/// A group that cannot fit stays saved and editable; no key is scaled to fit.
enum PlankControlPlacementResolver {
    struct Resolution: Equatable, Sendable {
        var frames: [UUID: CGRect]
        var unavailableIDs: Set<UUID>
    }
    static func pointSizedFrame(_ placement: PlankControlPlacement,in bounds: CGRect) -> CGRect {
        guard placement.isValid, PlankControlChromeGeometry.valid(bounds) else { return .zero }
        let center = placement.frame(in:bounds)
        return CGRect(x:center.midX - placement.width / 2,y:center.midY - placement.height / 2,
                      width:placement.width,height:placement.height)
    }
    static func resolve(controls: [PlankCustomControl],landscape: Bool,
                        chrome: PlankControlChromeGeometry,gutter: CGFloat = 6) -> Resolution {
        let controls = Array(controls.prefix(PlankControlLayout.maximumControls))
        let gap = gutter.isFinite ? max(0,gutter) : 6
        let area = chrome.usableBounds.insetBy(dx:gap,dy:gap)
        guard PlankControlChromeGeometry.valid(area) else {
            return Resolution(frames:[:],unavailableIDs:Set(controls.map(\.id)))
        }
        let original = controls.map { pointSizedFrame($0.placement(landscape:landscape),in:chrome.bounds) }
        var groups: [[Int]] = []
        func intervalGap(_ a: ClosedRange<CGFloat>,_ b: ClosedRange<CGFloat>) -> CGFloat {
            max(0,max(a.lowerBound,b.lowerBound) - min(a.upperBound,b.upperBound))
        }
        func connected(_ a: CGRect,_ b: CGRect) -> Bool {
            guard PlankControlChromeGeometry.valid(a),PlankControlChromeGeometry.valid(b) else { return false }
            let x = intervalGap(a.minX...a.maxX,b.minX...b.maxX)
            let y = intervalGap(a.minY...a.maxY,b.minY...b.maxY)
            return (x <= 0.000_001 && y <= gap + 0.000_001)
                || (y <= 0.000_001 && x <= gap + 0.000_001)
        }
        for index in original.indices {
            let joins = groups.indices.filter { groups[$0].contains { connected(original[index],original[$0]) } }
            var group = [index]
            for joined in joins.reversed() { group += groups.remove(at:joined) }
            groups.append(group)
        }
        func union(_ group: [Int]) -> CGRect { group.reduce(CGRect.null) { $0.union(original[$1]) } }
        groups.sort {
            let a = union($0), b = union($1)
            if abs(a.minY - b.minY) > 0.000_001 { return a.minY < b.minY }
            if abs(a.minX - b.minX) > 0.000_001 { return a.minX < b.minX }
            return $0.map { controls[$0].id.uuidString }.min()! < $1.map { controls[$0].id.uuidString }.min()!
        }
        var result = Resolution(frames:[:],unavailableIDs:[])
        var occupied: [CGRect] = []
        func intersects(_ a: CGRect,_ b: CGRect) -> Bool {
            let intersection = a.intersection(b)
            return !intersection.isNull && intersection.width > 0.000_001 && intersection.height > 0.000_001
        }
        for group in groups {
            let frame = union(group)
            let minX = area.minX - frame.minX, maxX = area.maxX - frame.maxX
            let minY = area.minY - frame.minY, maxY = area.maxY - frame.maxY
            guard PlankControlChromeGeometry.valid(frame),minX <= maxX,maxY >= minY else {
                result.unavailableIDs.formUnion(group.map { controls[$0].id }); continue
            }
            let obstacles = (chrome.reservedRects + occupied).map { $0.insetBy(dx:-gap,dy:-gap) }
            var x = [min(max(0,minX),maxX),minX,maxX]
            var y = [min(max(0,minY),maxY),minY,maxY]
            for obstacle in obstacles {
                x += [obstacle.minX - frame.maxX,obstacle.maxX - frame.minX]
                y += [obstacle.minY - frame.maxY,obstacle.maxY - frame.minY]
            }
            // At most32 control rectangles and8 chrome regions participate.
            x = Array(Set(x.filter { $0.isFinite && $0 >= minX && $0 <= maxX })).sorted()
            y = Array(Set(y.filter { $0.isFinite && $0 >= minY && $0 <= maxY })).sorted()
            var candidates = x.flatMap { dx in y.map { CGPoint(x:dx,y:$0) } }
            candidates.sort {
                let a = $0.x * $0.x + $0.y * $0.y, b = $1.x * $1.x + $1.y * $1.y
                if abs(a - b) > 0.000_001 { return a < b }
                if abs($0.x) != abs($1.x) { return abs($0.x) < abs($1.x) }
                if $0.y != $1.y { return $0.y < $1.y }
                return $0.x < $1.x
            }
            let translation = candidates.first { delta in
                group.allSatisfy { index in
                    let moved = original[index].offsetBy(dx:delta.x,dy:delta.y)
                    return obstacles.allSatisfy { !intersects(moved,$0) }
                }
            }
            guard let translation else {
                result.unavailableIDs.formUnion(group.map { controls[$0].id }); continue
            }
            for index in group {
                let moved = original[index].offsetBy(dx:translation.x,dy:translation.y)
                result.frames[controls[index].id] = moved; occupied.append(moved)
            }
        }
        return result
    }
}

/// Snapping is measured in actual surface points, independently of sheet scale.
/// Only peers near the perpendicular span participate, preventing a distant row
/// from pulling an unrelated key sideways.
enum PlankControlSnap {
    enum Axis: Equatable, Sendable { case vertical, horizontal }
    enum Kind: Equatable, Sendable { case edge, alignment, gutter, size }
    struct Guide: Equatable, Sendable {
        var axis: Axis
        var position: CGFloat
        var start: CGFloat
        var end: CGFloat
        var kind: Kind
    }
    struct Result: Equatable, Sendable {
        var placement: PlankControlPlacement
        var guides: [Guide]
    }
    private struct Candidate {
        var delta: CGFloat
        var guide: Guide
        var priority: Int
    }
    private static func valid(_ frame: CGRect) -> Bool {
        frame.minX.isFinite && frame.minY.isFinite && frame.width.isFinite && frame.height.isFinite
            && frame.width > 0 && frame.height > 0
    }
    private static func intervalGap(_ a: ClosedRange<CGFloat>, _ b: ClosedRange<CGFloat>) -> CGFloat {
        max(0,max(a.lowerBound,b.lowerBound) - min(a.upperBound,b.upperBound))
    }
    private static func nearest(_ candidates: [Candidate], threshold: CGFloat) -> Candidate? {
        candidates.filter { $0.delta.isFinite && abs($0.delta) <= threshold }.min {
            if abs(abs($0.delta) - abs($1.delta)) > 0.000_001 { return abs($0.delta) < abs($1.delta) }
            if $0.priority != $1.priority { return $0.priority < $1.priority }
            if $0.guide.position != $1.guide.position { return $0.guide.position < $1.guide.position }
            if $0.guide.start != $1.guide.start { return $0.guide.start < $1.guide.start }
            return $0.guide.end < $1.guide.end
        }
    }
    static func move(placement: PlankControlPlacement, in bounds: CGRect, peers: [CGRect],
                     gutter: CGFloat = 6, threshold: CGFloat = 8) -> Result {
        guard placement.isValid, valid(bounds) else { return Result(placement:placement,guides:[]) }
        let gap = gutter.isFinite ? max(0,gutter) : 6
        let tolerance = threshold.isFinite ? max(0,threshold) : 8
        var result = clampedPlacement(placement,in:bounds)
        var frame = result.frame(in:bounds)
        var x: [Candidate] = [], y: [Candidate] = []
        func add(_ axis: Axis, target: CGFloat, source: CGFloat, position: CGFloat,
                 span: ClosedRange<CGFloat>, kind: Kind, priority: Int) {
            let delta = target - source
            let shifted = axis == .vertical ? frame.offsetBy(dx:delta,dy:0) : frame.offsetBy(dx:0,dy:delta)
            guard shifted.minX >= bounds.minX - 0.000_001, shifted.maxX <= bounds.maxX + 0.000_001,
                  shifted.minY >= bounds.minY - 0.000_001, shifted.maxY <= bounds.maxY + 0.000_001 else { return }
            let candidate = Candidate(delta:delta,guide:Guide(axis:axis,position:position,
                start:span.lowerBound,end:span.upperBound,kind:kind),priority:priority)
            if axis == .vertical { x.append(candidate) } else { y.append(candidate) }
        }
        add(.vertical,target:bounds.minX + gap,source:frame.minX,position:bounds.minX + gap,
            span:bounds.minY...bounds.maxY,kind:.edge,priority:0)
        add(.vertical,target:bounds.maxX - gap,source:frame.maxX,position:bounds.maxX - gap,
            span:bounds.minY...bounds.maxY,kind:.edge,priority:0)
        add(.horizontal,target:bounds.minY + gap,source:frame.minY,position:bounds.minY + gap,
            span:bounds.minX...bounds.maxX,kind:.edge,priority:0)
        add(.horizontal,target:bounds.maxY - gap,source:frame.maxY,position:bounds.maxY - gap,
            span:bounds.minX...bounds.maxX,kind:.edge,priority:0)
        for peer in peers.filter(valid) {
            if intervalGap(frame.minY...frame.maxY,peer.minY...peer.maxY) <= gap + tolerance {
                let span = min(frame.minY,peer.minY)...max(frame.maxY,peer.maxY)
                add(.vertical,target:peer.maxX + gap,source:frame.minX,position:peer.maxX + gap / 2,
                    span:span,kind:.gutter,priority:1)
                add(.vertical,target:peer.minX - gap,source:frame.maxX,position:peer.minX - gap / 2,
                    span:span,kind:.gutter,priority:1)
                for (target,source) in [(peer.minX,frame.minX),(peer.maxX,frame.maxX),(peer.midX,frame.midX)] {
                    add(.vertical,target:target,source:source,position:target,span:span,kind:.alignment,priority:2)
                }
            }
            if intervalGap(frame.minX...frame.maxX,peer.minX...peer.maxX) <= gap + tolerance {
                let span = min(frame.minX,peer.minX)...max(frame.maxX,peer.maxX)
                add(.horizontal,target:peer.maxY + gap,source:frame.minY,position:peer.maxY + gap / 2,
                    span:span,kind:.gutter,priority:1)
                add(.horizontal,target:peer.minY - gap,source:frame.maxY,position:peer.minY - gap / 2,
                    span:span,kind:.gutter,priority:1)
                for (target,source) in [(peer.minY,frame.minY),(peer.maxY,frame.maxY),(peer.midY,frame.midY)] {
                    add(.horizontal,target:target,source:source,position:target,span:span,kind:.alignment,priority:2)
                }
            }
        }
        let horizontal = nearest(x,threshold:tolerance), vertical = nearest(y,threshold:tolerance)
        frame = frame.offsetBy(dx:horizontal?.delta ?? 0,dy:vertical?.delta ?? 0)
        result.x = Double((frame.midX - bounds.minX) / bounds.width)
        result.y = Double((frame.midY - bounds.minY) / bounds.height)
        return Result(placement:result,guides:[horizontal?.guide,vertical?.guide].compactMap { $0 })
    }

    /// Translation is already in reference-surface points. The dragged corner
    /// changes size; its diagonally opposite corner remains fixed throughout.
    static func resize(original: PlankControlPlacement, corner: Int, translation: CGSize,
                       in bounds: CGRect, peers: [CGRect], gutter: CGFloat = 6,
                       threshold: CGFloat = 8) -> Result {
        guard original.isValid, valid(bounds), (0...3).contains(corner),
              translation.width.isFinite, translation.height.isFinite else {
            return Result(placement:original,guides:[])
        }
        let gap = gutter.isFinite ? max(0,gutter) : 6
        let tolerance = threshold.isFinite ? max(0,threshold) : 8
        let initial = original.frame(in:bounds)
        let left = corner % 2 == 0, top = corner < 2
        let fixed = CGPoint(x:left ? initial.maxX : initial.minX,y:top ? initial.maxY : initial.minY)
        let minimum = CGFloat(PlankControlPlacement.minimumDimension)
        let maximumWidth = min(CGFloat(PlankControlPlacement.maximumWidth),left ? fixed.x - bounds.minX : bounds.maxX - fixed.x)
        let maximumHeight = min(CGFloat(PlankControlPlacement.maximumHeight),top ? fixed.y - bounds.minY : bounds.maxY - fixed.y)
        let width = min(max(minimum,initial.width + (left ? -translation.width : translation.width)),max(minimum,maximumWidth))
        let height = min(max(minimum,initial.height + (top ? -translation.height : translation.height)),max(minimum,maximumHeight))
        var frame = CGRect(x:left ? fixed.x - width : fixed.x,y:top ? fixed.y - height : fixed.y,width:width,height:height)
        let moving = CGPoint(x:left ? frame.minX : frame.maxX,y:top ? frame.minY : frame.maxY)
        var x: [Candidate] = [], y: [Candidate] = []
        func add(_ axis: Axis, target: CGFloat, position: CGFloat, span: ClosedRange<CGFloat>, kind: Kind, priority: Int) {
            let dimension = axis == .vertical ? abs(target - fixed.x) : abs(target - fixed.y)
            let maximum = axis == .vertical ? maximumWidth : maximumHeight
            let correctSide = axis == .vertical ? (left ? target < fixed.x : target > fixed.x) : (top ? target < fixed.y : target > fixed.y)
            guard correctSide, dimension >= minimum, dimension <= maximum + 0.000_001 else { return }
            let candidate = Candidate(delta:target - (axis == .vertical ? moving.x : moving.y),
                guide:Guide(axis:axis,position:position,start:span.lowerBound,end:span.upperBound,kind:kind),priority:priority)
            if axis == .vertical { x.append(candidate) } else { y.append(candidate) }
        }
        let edgeX = left ? bounds.minX + gap : bounds.maxX - gap
        let edgeY = top ? bounds.minY + gap : bounds.maxY - gap
        add(.vertical,target:edgeX,position:edgeX,span:bounds.minY...bounds.maxY,kind:.edge,priority:0)
        add(.horizontal,target:edgeY,position:edgeY,span:bounds.minX...bounds.maxX,kind:.edge,priority:0)
        for peer in peers.filter(valid) {
            if intervalGap(frame.minY...frame.maxY,peer.minY...peer.maxY) <= gap + tolerance {
                let span = min(frame.minY,peer.minY)...max(frame.maxY,peer.maxY)
                for target in [peer.minX,peer.maxX] { add(.vertical,target:target,position:target,span:span,kind:.alignment,priority:2) }
                add(.vertical,target:peer.minX - gap,position:peer.minX - gap / 2,span:span,kind:.gutter,priority:1)
                add(.vertical,target:peer.maxX + gap,position:peer.maxX + gap / 2,span:span,kind:.gutter,priority:1)
                let target = fixed.x + (left ? -peer.width : peer.width)
                add(.vertical,target:target,position:target,span:span,kind:.size,priority:3)
            }
            if intervalGap(frame.minX...frame.maxX,peer.minX...peer.maxX) <= gap + tolerance {
                let span = min(frame.minX,peer.minX)...max(frame.maxX,peer.maxX)
                for target in [peer.minY,peer.maxY] { add(.horizontal,target:target,position:target,span:span,kind:.alignment,priority:2) }
                add(.horizontal,target:peer.minY - gap,position:peer.minY - gap / 2,span:span,kind:.gutter,priority:1)
                add(.horizontal,target:peer.maxY + gap,position:peer.maxY + gap / 2,span:span,kind:.gutter,priority:1)
                let target = fixed.y + (top ? -peer.height : peer.height)
                add(.horizontal,target:target,position:target,span:span,kind:.size,priority:3)
            }
        }
        let horizontal = nearest(x,threshold:tolerance), vertical = nearest(y,threshold:tolerance)
        let cornerX = moving.x + (horizontal?.delta ?? 0), cornerY = moving.y + (vertical?.delta ?? 0)
        frame = CGRect(x:min(fixed.x,cornerX),y:min(fixed.y,cornerY),width:abs(cornerX - fixed.x),height:abs(cornerY - fixed.y))
        return Result(placement:PlankControlPlacement(frame:frame,in:bounds),
                      guides:[horizontal?.guide,vertical?.guide].compactMap { $0 })
    }
}

struct PlankCustomControl: Codable, Equatable, Identifiable, Sendable {
    static let maximumLabelLength = 40
    var id: UUID
    var label: String
    var binding: PlankControlBinding
    var behavior: PlankControlBehavior
    var portrait: PlankControlPlacement
    var landscape: PlankControlPlacement
    init(id: UUID = UUID(), label: String, binding: PlankControlBinding,
         behavior: PlankControlBehavior = .hold, portrait: PlankControlPlacement,
         landscape: PlankControlPlacement) {
        self.id = id; self.label = label; self.binding = binding; self.behavior = behavior
        self.portrait = portrait; self.landscape = landscape
    }
    var isValid: Bool {
        !label.trimmingCharacters(in:.whitespacesAndNewlines).isEmpty && label.count <= Self.maximumLabelLength
            && !label.unicodeScalars.contains { CharacterSet.controlCharacters.contains($0) }
            && binding.isValid && portrait.isValid && landscape.isValid
    }
    func placement(landscape: Bool) -> PlankControlPlacement { landscape ? self.landscape : portrait }
    mutating func setPlacement(_ value: PlankControlPlacement, landscape: Bool) {
        if landscape { self.landscape = value } else { portrait = value }
    }
    var automaticLabel: String { String(binding.title.prefix(Self.maximumLabelLength)) }
    /// Keep an artist's named action, but follow a changed shortcut when the
    /// existing label is still its generated key name (including legacy ⌘).
    mutating func setBinding(_ value: PlankControlBinding) {
        guard value.isValid else { return }
        let followsKey = label.trimmingCharacters(in:.whitespacesAndNewlines).isEmpty
            || label == automaticLabel
            || (binding.modifiers == 0 && PlankPencilModifier(rawValue:binding.code)?.title == label)
        binding = value
        if followsKey { useBindingLabel() }
    }
    mutating func useBindingLabel() { label = automaticLabel }
}

struct PlankControlLayout: Codable, Equatable, Identifiable, Sendable {
    static let maximumControls = 32
    var id: UUID
    var name: String
    var controls: [PlankCustomControl]
    init(id: UUID = UUID(), name: String, controls: [PlankCustomControl]) {
        self.id = id; self.name = name; self.controls = controls
    }
    var isValid: Bool {
        !name.trimmingCharacters(in:.whitespacesAndNewlines).isEmpty && name.count <= 48
            && !name.unicodeScalars.contains { CharacterSet.controlCharacters.contains($0) }
            && controls.count <= Self.maximumControls && controls.allSatisfy(\.isValid)
            && Set(controls.map(\.id)).count == controls.count
    }
    static func defaults(shortcuts: [PlankPencilModifier] = PlankPencilModifier.allCases,
                         name: String = "Artist Controls") -> Self {
        var seen = Set<PlankPencilModifier>()
        let keys = (shortcuts + PlankPencilModifier.allCases).filter { seen.insert($0).inserted }
        var modifierIndex = 0
        let controls = keys.map { key in
            let wide = key == .space
            let column = Double(modifierIndex % 2), row = Double(modifierIndex / 2)
            if !wide { modifierIndex += 1 }
            let landscape = PlankControlPlacement(x:wide ? 0.15 : 0.06 + column * 0.10,
                y:wide ? 0.66 : 0.77 + row * 0.09,width:wide ? 200 : 76,height:52)
            let portrait = PlankControlPlacement(x:wide ? 0.25 : 0.10 + column * 0.18,
                y:wide ? 0.75 : 0.85 + row * 0.06,width:wide ? 200 : 76,height:52)
            return PlankCustomControl(label:key.title,binding:.init(code:key.rawValue),
                portrait:portrait,landscape:landscape)
        }
        return Self(name:name,controls:controls)
    }
    func copied(name: String? = nil) -> Self {
        var next = self
        next.id = UUID()
        next.name = name ?? String(self.name.prefix(43)) + " Copy"
        next.controls = controls.map { control in var value = control; value.id = UUID(); return value }
        return next
    }
}

/// One validated, bounded library shared by direct Desktop and Pencil sharing.
/// Loading old five-key settings imports their sanitized order once. It never
/// rewrites pad margins, appearance, identities or consent records.
struct PlankCustomControlLibrary: Codable, Equatable, Sendable {
    static let maximumLayouts = 16
    static let storageKey = "plank.ipad.custom-controls.v1"
    var version = 1
    var layouts: [PlankControlLayout]
    var selectedID: UUID
    init(layouts: [PlankControlLayout], selectedID: UUID? = nil) {
        let values = layouts.isEmpty ? [PlankControlLayout.defaults()] : layouts
        self.layouts = values; self.selectedID = selectedID ?? values[0].id
    }
    var isValid: Bool {
        version == 1 && !layouts.isEmpty && layouts.count <= Self.maximumLayouts
            && layouts.allSatisfy(\.isValid) && Set(layouts.map(\.id)).count == layouts.count
            && layouts.contains { $0.id == selectedID }
    }
    var selectedLayout: PlankControlLayout {
        layouts.first { $0.id == selectedID } ?? layouts.first ?? .defaults()
    }
    @discardableResult mutating func select(id: UUID) -> Bool {
        guard layouts.contains(where:{ $0.id == id }) else { return false }
        selectedID = id; return true
    }
    @discardableResult mutating func upsert(layout: PlankControlLayout) -> Bool {
        guard layout.isValid else { return false }
        if let index = layouts.firstIndex(where:{ $0.id == layout.id }) { layouts[index] = layout }
        else {
            guard layouts.count < Self.maximumLayouts else { return false }
            layouts.append(layout)
        }
        selectedID = layout.id
        return true
    }
    @discardableResult mutating func remove(id: UUID) -> Bool {
        guard layouts.count > 1, let index = layouts.firstIndex(where:{ $0.id == id }) else { return false }
        layouts.remove(at:index)
        if selectedID == id { selectedID = layouts[0].id }
        return true
    }
    @discardableResult mutating func duplicate(id: UUID) -> UUID? {
        guard let original = layouts.first(where:{ $0.id == id }) else { return nil }
        let copy = original.copied()
        return upsert(layout:copy) ? copy.id : nil
    }
    static func load(from defaults: UserDefaults = .standard) -> Self {
        if let data = defaults.data(forKey:storageKey), data.count <= 512 * 1024,
           let library = try? JSONDecoder().decode(Self.self,from:data), library.isValid { return library }
        let imported = PlankControlLayout.defaults(shortcuts:PlankIPadPencilPadSettings.load(from:defaults).shortcuts)
        let library = Self(layouts:[imported])
        library.save(to:defaults)
        return library
    }
    func save(to defaults: UserDefaults = .standard) {
        guard isValid, let data = try? JSONEncoder().encode(self), data.count <= 512 * 1024 else { return }
        defaults.set(data,forKey:Self.storageKey)
    }
}
