import SwiftUI

/// Local navigation only. Keeping these targets separate from the drawing
/// surface prevents a navigation gesture becoming a remote mouse press.
struct PlankIPadZoomMenu: View {
    @ObservedObject var zoom: PlankIPadZoomController
    @ObservedObject var preferences: PlankIPadZoomNavigatorPreferences
    var enabled: Bool
    var body: some View {
        Menu {
            Button { zoom.requestMode(.fit) } label: {
                HStack { Text("Fit"); if zoom.state.mode == .fit { Image(systemName:"checkmark") } }
            }
            Button { zoom.requestMode(.double) } label: {
                HStack { Text("2× View"); if zoom.state.mode == .double { Image(systemName:"checkmark") } }
            }.disabled(!enabled)
            Toggle("Hand",isOn:Binding(get:{ zoom.state.handEnabled },set:{ zoom.requestHand($0) }))
                .disabled(!enabled || zoom.state.mode == .fit)
            Button("Center View") { zoom.requestCenter(CGPoint(x:0.5,y:0.5)) }
                .disabled(!enabled || zoom.state.mode == .fit)
            Divider()
            Toggle("Show Zoom Map",isOn:Binding(get:{ preferences.navigatorVisible },set:{ preferences.setVisible($0) }))
            Button("Reset Map Position") { preferences.resetPosition() }
        } label: {
            Image(systemName:zoom.state.handEnabled ? "hand.draw" : "plus.magnifyingglass")
                .frame(width:44,height:44)
        }
        .accessibilityLabel("Desktop view")
        .accessibilityValue(zoom.state.mode == .fit ? "Fit" : (zoom.state.handEnabled ? "2× View, Hand" : "2× View"))
    }
}

struct PlankIPadPencilZoomMenu: View {
    @ObservedObject var zoom: PlankIPadZoomController
    @ObservedObject var preferences: PlankIPadZoomNavigatorPreferences
    @ObservedObject var relay: PlankIPadPencilRelay
    @ObservedObject var receiver: PlankIPadPencilImageReceiver
    var body: some View {
        PlankIPadZoomMenu(zoom:zoom,preferences:preferences,enabled:relay.canNavigateImage)
    }
}

/// Dragging the header moves local chrome only. The map below keeps its own
/// source-navigation gesture, so a placement drag never requests a new crop.
struct PlankIPadZoomNavigatorPanel<Content: View>: View {
    @ObservedObject var preferences: PlankIPadZoomNavigatorPreferences
    let usableBounds: CGRect
    let excludedRects: [CGRect]
    let panelRectChanged: (CGRect?) -> Void
    let content: Content
    @State private var dragStart: CGPoint?
    @State private var dragPosition: CGPoint?

    init(preferences: PlankIPadZoomNavigatorPreferences, usableBounds: CGRect,
         excludedRects: [CGRect] = [], panelRectChanged: @escaping (CGRect?) -> Void = { _ in },
         @ViewBuilder content: () -> Content) {
        self.preferences = preferences; self.usableBounds = usableBounds
        self.excludedRects = excludedRects; self.panelRectChanged = panelRectChanged
        self.content = content()
    }

    var body: some View {
        let placement = preferences.navigatorVisible ? PlankIPadZoomNavigatorPlacement.rect(
            position:dragPosition ?? preferences.normalizedPosition,usableBounds:usableBounds,excluding:excludedRects) : nil
        ZStack(alignment:.topLeading) {
            if let placement {
                VStack(spacing:0) {
                    HStack(spacing:0) {
                        HStack(spacing:6) {
                            Image(systemName:"line.3.horizontal")
                            Text("Map")
                        }
                        .font(.caption).foregroundStyle(.secondary)
                        .frame(width:96,height:44).contentShape(Rectangle())
                        .gesture(DragGesture(minimumDistance:4,coordinateSpace:.named("plank-control-window"))
                            .onChanged { value in
                                let origin = dragStart ?? placement.origin
                                if dragStart == nil { dragStart = origin }
                                dragPosition = PlankIPadZoomNavigatorPlacement.normalizedPosition(
                                    for:CGPoint(x:origin.x + value.translation.width,y:origin.y + value.translation.height),
                                    in:usableBounds)
                            }
                            .onEnded { _ in
                                if let dragPosition,let final = PlankIPadZoomNavigatorPlacement.rect(
                                    position:dragPosition,usableBounds:usableBounds,excluding:excludedRects),
                                   let position = PlankIPadZoomNavigatorPlacement.normalizedPosition(for:final.origin,in:usableBounds) {
                                    preferences.setNormalizedPosition(position)
                                }
                                dragStart = nil; dragPosition = nil
                            })
                        .accessibilityElement(children:.ignore)
                        .accessibilityLabel("Move zoom map")
                        .accessibilityHint("Drag to move the map without changing the desktop view.")
                        .accessibilityAction(named:Text("Move left")) { move(x:-0.1,y:0) }
                        .accessibilityAction(named:Text("Move right")) { move(x:0.1,y:0) }
                        .accessibilityAction(named:Text("Move up")) { move(x:0,y:-0.1) }
                        .accessibilityAction(named:Text("Move down")) { move(x:0,y:0.1) }
                        Button { preferences.setVisible(false) } label: {
                            Image(systemName:"xmark").font(.caption).frame(width:44,height:44)
                        }.buttonStyle(.plain).accessibilityLabel("Hide zoom map")
                    }
                    content.frame(width:140,height:108)
                }
                .frame(width:140,height:152)
                .background(.ultraThinMaterial,in:RoundedRectangle(cornerRadius:12))
                .onGeometryChange(for:CGRect.self) { $0.frame(in:.named("plank-control-window")) } action: { panelRectChanged($0) }
                .position(x:placement.midX,y:placement.midY)
                .onDisappear { panelRectChanged(nil); dragStart = nil; dragPosition = nil }
            } else {
                Color.clear.allowsHitTesting(false).onAppear { panelRectChanged(nil) }
            }
        }
        .frame(maxWidth:.infinity,maxHeight:.infinity,alignment:.topLeading)
        .onChange(of:usableBounds) { _,_ in dragStart = nil; dragPosition = nil }
        .onChange(of:excludedRects) { _,_ in dragStart = nil; dragPosition = nil }
    }

    private func move(x: CGFloat,y: CGFloat) {
        let current = preferences.normalizedPosition
        guard let proposed = PlankIPadZoomNavigatorPlacement.bounded(CGPoint(x:current.x + x,y:current.y + y)),
              let rect = PlankIPadZoomNavigatorPlacement.rect(position:proposed,usableBounds:usableBounds,excluding:excludedRects),
              let position = PlankIPadZoomNavigatorPlacement.normalizedPosition(for:rect.origin,in:usableBounds) else { return }
        preferences.setNormalizedPosition(position)
    }
}

/// The outer footprint stays constant in Fit and 2×, preserving Artist hold
/// ownership and menu protection. The outline represents the full source;
/// the blue rectangle is the current crop, independent of window aspect.
struct PlankIPadZoomNavigator: View {
    @ObservedObject var zoom: PlankIPadZoomController
    let width: Int
    let height: Int
    var enabled: Bool
    var body: some View {
        GeometryReader { geometry in
            if let viewport = PlankIPadViewport(bounds:CGRect(origin:.zero,size:geometry.size),
                width:width,height:height) {
                let crop = zoom.state.visibleSourceCrop
                let selection = CGRect(x:viewport.rect.minX+crop.minX*viewport.rect.width,
                    y:viewport.rect.minY+crop.minY*viewport.rect.height,
                    width:crop.width*viewport.rect.width,height:crop.height*viewport.rect.height)
                ZStack(alignment:.topLeading) {
                    Rectangle().fill(.black.opacity(0.65))
                        .frame(width:viewport.rect.width,height:viewport.rect.height)
                        .position(x:viewport.rect.midX,y:viewport.rect.midY)
                    Rectangle().stroke(.secondary.opacity(0.8),lineWidth:1)
                        .frame(width:viewport.rect.width,height:viewport.rect.height)
                        .position(x:viewport.rect.midX,y:viewport.rect.midY)
                    Rectangle().fill(Color.accentColor.opacity(enabled ? 0.15 : 0.05))
                        .overlay { Rectangle().stroke(Color.accentColor.opacity(enabled ? 0.9 : 0.3),lineWidth:1.5) }
                        .frame(width:selection.width,height:selection.height)
                        .position(x:selection.midX,y:selection.midY)
                }
                .contentShape(Rectangle())
                .gesture(DragGesture(minimumDistance:0).onChanged { value in
                    guard enabled,zoom.state.mode == .double,
                          let position = viewport.normalized(value.location,held:true) else { return }
                    zoom.requestCenter(CGPoint(x:position.x,y:position.y))
                })
            } else { Color.clear }
        }
        .padding(10)
        .frame(width:140,height:108)
        .overlay(alignment:.bottomTrailing) {
            Text(zoom.state.mode == .fit ? "Fit" : "2×").font(.caption2).foregroundStyle(.secondary)
                .padding(.trailing,8).padding(.bottom,2).allowsHitTesting(false)
        }
        .accessibilityElement(children:.ignore)
        .accessibilityLabel("Desktop navigator")
        .accessibilityValue(zoom.state.mode == .fit ? "Fit" : "2× View")
        .accessibilityHint("In 2× View, drag to choose the desktop area.")
        .accessibilityAdjustableAction { direction in
            guard enabled,zoom.state.mode == .double else { return }
            var center = zoom.state.center
            switch direction {
            case .increment: center.x += 0.125
            case .decrement: center.x -= 0.125
            @unknown default: return
            }
            zoom.requestCenter(center)
        }
    }
}

struct PlankIPadPencilZoomNavigator: View {
    @ObservedObject var zoom: PlankIPadZoomController
    @ObservedObject var relay: PlankIPadPencilRelay
    @ObservedObject var receiver: PlankIPadPencilImageReceiver
    var body: some View {
        PlankIPadZoomNavigator(zoom:zoom,width:relay.width,height:relay.height,
            enabled:relay.canNavigateImage)
    }
}
