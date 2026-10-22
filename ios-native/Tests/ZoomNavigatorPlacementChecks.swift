import Foundation
import CoreGraphics
import Combine

@main enum ZoomNavigatorPlacementChecks {
    @MainActor static func main() {
        let bounds = CGRect(x:20,y:60,width:1000,height:800)
        let size = PlankIPadZoomNavigatorPlacement.panelSize
        let bottomLeft = PlankIPadZoomNavigatorPlacement.rect(position:CGPoint(x:0,y:1),usableBounds:bounds,excluding:[])!
        precondition(bottomLeft == CGRect(x:20,y:708,width:140,height:152))
        let topRight = PlankIPadZoomNavigatorPlacement.rect(position:CGPoint(x:1,y:0),usableBounds:bounds,excluding:[])!
        precondition(topRight == CGRect(x:880,y:60,width:140,height:152))
        for position in [CGPoint(x:0,y:0),CGPoint(x:0.25,y:0.75),CGPoint(x:1,y:1)] {
            let rect = PlankIPadZoomNavigatorPlacement.rect(position:position,usableBounds:bounds,excluding:[])!
            let inverse = PlankIPadZoomNavigatorPlacement.normalizedPosition(for:rect.origin,in:bounds)!
            precondition(abs(inverse.x - position.x) < 0.000001 && abs(inverse.y - position.y) < 0.000001)
            // A changed window preserves relative position and fixed touch size.
            let resized = CGRect(x:10,y:40,width:380,height:300)
            let smaller = PlankIPadZoomNavigatorPlacement.rect(position:position,usableBounds:resized,excluding:[])!
            precondition(smaller.size == size && resized.contains(smaller))
            let smallerInverse = PlankIPadZoomNavigatorPlacement.normalizedPosition(for:smaller.origin,in:resized)!
            precondition(abs(smallerInverse.x - position.x) < 0.000001 && abs(smallerInverse.y - position.y) < 0.000001)
        }
        precondition(PlankIPadZoomNavigatorPlacement.rect(position:CGPoint(x:-10,y:10),usableBounds:bounds,excluding:[]) == bottomLeft)
        let exact = CGRect(origin:.zero,size:size)
        precondition(PlankIPadZoomNavigatorPlacement.rect(position:CGPoint(x:1,y:1),usableBounds:exact,excluding:[]) == exact)
        precondition(PlankIPadZoomNavigatorPlacement.normalizedPosition(for:CGPoint(x:100,y:100),in:exact) == .zero)
        for invalid in [CGFloat.nan,CGFloat.infinity,-CGFloat.infinity] {
            precondition(PlankIPadZoomNavigatorPlacement.rect(position:CGPoint(x:invalid,y:0),usableBounds:bounds,excluding:[]) == nil)
            precondition(PlankIPadZoomNavigatorPlacement.normalizedPosition(for:CGPoint(x:0,y:invalid),in:bounds) == nil)
            precondition(PlankIPadZoomNavigatorPlacement.rect(position:.zero,
                usableBounds:CGRect(x:0,y:0,width:invalid,height:800),excluding:[]) == nil)
        }
        precondition(PlankIPadZoomNavigatorPlacement.rect(position:.zero,
            usableBounds:CGRect(x:0,y:0,width:139,height:152),excluding:[]) == nil)
        precondition(PlankIPadZoomNavigatorPlacement.rect(position:.zero,
            usableBounds:CGRect(x:0,y:0,width:140,height:151),excluding:[]) == nil)

        let toolbar = CGRect(x:0,y:650,width:100,height:100)
        let collisionBounds = CGRect(x:0,y:0,width:1000,height:800)
        let displaced = PlankIPadZoomNavigatorPlacement.rect(position:CGPoint(x:0,y:1),usableBounds:collisionBounds,excluding:[toolbar])!
        precondition(displaced.origin == CGPoint(x:104,y:648))
        precondition(collisionBounds.contains(displaced) && !displaced.intersects(toolbar.insetBy(dx:-4,dy:-4)))
        let consent = CGRect(x:400,y:0,width:200,height:800)
        let side = PlankIPadZoomNavigatorPlacement.rect(position:CGPoint(x:0.5,y:0.5),usableBounds:collisionBounds,excluding:[consent])!
        precondition(collisionBounds.contains(side) && !side.intersects(consent.insetBy(dx:-4,dy:-4)))
        precondition(PlankIPadZoomNavigatorPlacement.rect(position:.zero,usableBounds:collisionBounds,excluding:[collisionBounds]) == nil)
        precondition(PlankIPadZoomNavigatorPlacement.rect(position:.zero,usableBounds:collisionBounds,excluding:[.zero,.null,.infinite])
            == CGRect(origin:.zero,size:size))

        let suite = "PLANK.ZoomNavigatorPlacementChecks." + UUID().uuidString
        let defaults = UserDefaults(suiteName:suite)!
        defer { defaults.removePersistentDomain(forName:suite) }
        let preferences = PlankIPadZoomNavigatorPreferences(defaults:defaults)
        precondition(preferences.navigatorVisible && preferences.normalizedPosition == CGPoint(x:0,y:1))
        let zoom = PlankIPadZoomController()
        zoom.requestMode(.double)
        zoom.requestHand(true)
        zoom.setRemoteContactActive(true)
        zoom.requestCenter(CGPoint(x:0.7,y:0.6))
        let zoomState = zoom.state, zoomRevision = zoom.revision
        var publications = 0
        let subscription = preferences.objectWillChange.sink { publications += 1 }
        preferences.setVisible(false)
        preferences.setNormalizedPosition(CGPoint(x:0.72,y:0.18))
        precondition(!preferences.navigatorVisible && preferences.normalizedPosition == CGPoint(x:0.72,y:0.18))
        precondition(zoom.state == zoomState && zoom.revision == zoomRevision && zoom.remoteContactActive)
        let reader = PlankIPadZoomNavigatorPreferences(defaults:UserDefaults(suiteName:suite)!)
        precondition(!reader.navigatorVisible && reader.normalizedPosition == preferences.normalizedPosition)
        let beforeDuplicates = publications
        preferences.setVisible(false)
        preferences.setNormalizedPosition(preferences.normalizedPosition)
        preferences.setNormalizedPosition(CGPoint(x:CGFloat.nan,y:0))
        precondition(publications == beforeDuplicates)
        preferences.setNormalizedPosition(CGPoint(x:5,y:-3))
        precondition(preferences.normalizedPosition == CGPoint(x:1,y:0))
        preferences.resetPosition()
        precondition(preferences.normalizedPosition == CGPoint(x:0,y:1) && !preferences.navigatorVisible)
        preferences.setVisible(true)
        precondition(preferences.navigatorVisible)
        precondition(zoom.state == zoomState && zoom.revision == zoomRevision && zoom.remoteContactActive)
        zoom.setRemoteContactActive(false)
        precondition(zoom.state.center == CGPoint(x:0.7,y:0.6) && zoom.state.handEnabled && zoom.state.mode == .double)
        withExtendedLifetime(subscription) {}

        for invalid: Any in ["invalid",true,Double.nan,Double.infinity] {
            defaults.set(invalid,forKey:PlankIPadZoomNavigatorPreferences.positionXKey)
            precondition(PlankIPadZoomNavigatorPreferences(defaults:defaults).normalizedPosition == CGPoint(x:0,y:1))
        }
        defaults.set(0.7,forKey:PlankIPadZoomNavigatorPreferences.positionXKey)
        defaults.removeObject(forKey:PlankIPadZoomNavigatorPreferences.positionYKey)
        precondition(PlankIPadZoomNavigatorPreferences(defaults:defaults).normalizedPosition == CGPoint(x:0,y:1))
        for invalid: Any in ["false",0,1,Double.nan] {
            defaults.set(invalid,forKey:PlankIPadZoomNavigatorPreferences.visibleKey)
            precondition(PlankIPadZoomNavigatorPreferences(defaults:defaults).navigatorVisible)
        }
        print("PASS zoom map placement: bounded persistent chrome, resize/collision/no-room handling and unchanged held-contact zoom state")
    }
}
