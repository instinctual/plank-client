import Foundation

@main enum ControlAppearanceChecks {
    static func main() {
        let standard = PlankIPadControlAppearance()
        precondition(standard.transparency == 0 && standard.idleFillAlpha == 0.94
            && standard.idleBorderAlpha == 0.8 && standard.idleLabelAlpha == 1
            && standard.pressedFillAlpha == 1 && standard.pressedBorderAlpha == 1)
        for step in 0...100 {
            let appearance = PlankIPadControlAppearance(transparency:Double(step) / 100)
            precondition(appearance.idleFillAlpha >= 0 && appearance.idleFillAlpha <= 0.94)
            precondition(appearance.idleBorderAlpha >= 0 && appearance.idleBorderAlpha <= 0.8)
            precondition(appearance.idleLabelAlpha >= 0.3 && appearance.idleLabelAlpha <= 1)
            precondition(appearance.pressedFillAlpha >= 0.55 && appearance.pressedFillAlpha <= 1)
            precondition(appearance.pressedBorderAlpha >= 0.75 && appearance.pressedBorderAlpha <= 1)
            precondition(appearance.pressedLabelAlpha == 1)
        }
        let transparent = PlankIPadControlAppearance(transparency:1)
        precondition(transparent.idleFillAlpha == 0 && transparent.idleBorderAlpha == 0
            && transparent.idleLabelAlpha > 0 && transparent.pressedFillAlpha > 0)
        precondition(PlankIPadControlAppearance(transparency:-1).transparency == 0)
        precondition(PlankIPadControlAppearance(transparency:2).transparency == 1)
        for invalid in [Double.nan,Double.infinity,-Double.infinity] {
            precondition(PlankIPadControlAppearance(transparency:invalid) == standard)
        }

        let suite = "PLANK.ControlAppearanceChecks." + UUID().uuidString
        let defaults = UserDefaults(suiteName:suite)!
        defer { defaults.removePersistentDomain(forName:suite) }
        precondition(PlankIPadControlAppearance.load(from:defaults) == 0)
        PlankIPadControlAppearance.save(0.64,to:defaults)
        // A second reader is the same preference used by Desktop and Sharing.
        let secondReader = UserDefaults(suiteName:suite)!
        precondition(PlankIPadControlAppearance.load(from:secondReader) == 0.64)
        PlankIPadControlAppearance.save(.nan,to:defaults)
        precondition(PlankIPadControlAppearance.load(from:defaults) == 0.64)
        PlankIPadControlAppearance.save(2,to:defaults)
        precondition(PlankIPadControlAppearance.load(from:defaults) == 1)
        PlankIPadControlAppearance.save(-1,to:defaults)
        precondition(PlankIPadControlAppearance.load(from:defaults) == 0)
        for invalid: Any in ["invalid",true,Double.nan,Double.infinity] {
            defaults.set(invalid,forKey:PlankIPadControlAppearance.preferenceKey)
            precondition(PlankIPadControlAppearance.load(from:defaults) == 0)
        }
        print("PASS artist-control appearance: bounded persistent global transparency, retained default appearance and pressed/label legibility")
    }
}
