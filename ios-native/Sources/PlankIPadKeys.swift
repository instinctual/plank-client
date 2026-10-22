import Foundation

// Same HID-to-Windows mapping as the Vision keyboard adapter.
func plankIPadVirtualKey(
    for key: Int,
    functionKeyMode: KeyboardFunctionKeyMode
) -> UInt16? {
    if functionKeyMode == .pc {
        switch key {
        // visionOS reports these three top-right keys from a Windows keyboard
        // as F13-F15. Restore the meanings printed on the physical keycaps.
        case 0x68: return 0x2C // Print Screen
        case 0x69: return 0x91 // Scroll Lock
        case 0x6A: return 0x13 // Pause
        default: break
        }
    }
    if (0x04...0x1D).contains(key) {
        return UInt16(0x41 + key - 0x04)
    }
    if (0x1E...0x26).contains(key) {
        return UInt16(0x31 + key - 0x1E)
    }
    if key == 0x27 { return 0x30 }
    if (0x3A...0x45).contains(key) {
        return UInt16(0x70 + key - 0x3A)
    }
    if (0x59...0x61).contains(key) {
        return UInt16(0x61 + key - 0x59)
    }
    if key == 0x62 { return 0x60 }
    if (0x68...0x73).contains(key) {
        return UInt16(0x7C + key - 0x68)
    }

    switch key {
    case 0x28, 0x58: return 0x0D // Return and keypad Enter
    case 0x29: return 0x1B
    case 0x2A: return 0x08
    case 0x2B: return 0x09
    case 0x2C: return 0x20
    case 0x2D: return 0xBD
    case 0x2E: return 0xBB
    case 0x2F: return 0xDB
    case 0x30: return 0xDD
    case 0x31, 0x32, 0x64: return 0xDC
    case 0x33: return 0xBA
    case 0x34: return 0xDE
    case 0x35: return 0xC0
    case 0x36: return 0xBC
    case 0x37: return 0xBE
    case 0x38: return 0xBF
    case 0x39: return 0x14
    case 0x46: return 0x2C
    case 0x47: return 0x91
    case 0x48: return 0x13
    case 0x49: return 0x2D
    case 0x4A: return 0x24
    case 0x4B: return 0x21
    case 0x4C: return 0x2E
    case 0x4D: return 0x23
    case 0x4E: return 0x22
    case 0x4F: return 0x27
    case 0x50: return 0x25
    case 0x51: return 0x28
    case 0x52: return 0x26
    case 0x53: return 0x90
    case 0x54: return 0x6F
    case 0x55: return 0x6A
    case 0x56: return 0x6D
    case 0x57: return 0x6B
    case 0x63: return 0x6E
    case 0x65: return 0x5D
    case 0xE0, 0xE4: return 0x11
    case 0xE1, 0xE5: return 0x10
    case 0xE2, 0xE6: return 0x12
    case 0xE3: return 0x5B
    case 0xE7: return 0x5C
    default: return nil
    }
}

// Same committed-character mapping as the Vision software keyboard. The
// remote application owns its text; this adapter stores no editable buffer.
enum PlankIPadSoftwareKeyboard {
    enum Command: Equatable {
        case key(UInt16, UInt8)
        case text(String)
    }
    static func commands(for text: String) -> [Command] {
        text.map { character in
            switch character {
            case "\r", "\n", "\r\n": return .key(0x0D, 0)
            case "\t": return .key(0x09, 0)
            case "\u{8}", "\u{7f}": return .key(0x08, 0)
            case "\u{1b}": return .key(0x1B, 0)
            default:
                if let key = physicalKey(for: character) {
                    return .key(key.code, key.shifted ? 1 : 0)
                }
                return .text(String(character))
            }
        }
    }
    private static func physicalKey(for character: Character) -> (code: UInt16, shifted: Bool)? {
        if let ascii = character.asciiValue {
            if ascii >= Character("a").asciiValue!, ascii <= Character("z").asciiValue! {
                return (UInt16(ascii - Character("a").asciiValue! + 0x41), false)
            }
            if ascii >= Character("A").asciiValue!, ascii <= Character("Z").asciiValue! {
                return (UInt16(ascii - Character("A").asciiValue! + 0x41), true)
            }
            if ascii >= Character("0").asciiValue!, ascii <= Character("9").asciiValue! {
                return (UInt16(ascii), false)
            }
        }
        switch character {
        case " ": return (0x20, false)
        case "!": return (0x31, true)
        case "@": return (0x32, true)
        case "#": return (0x33, true)
        case "$": return (0x34, true)
        case "%": return (0x35, true)
        case "^": return (0x36, true)
        case "&": return (0x37, true)
        case "*": return (0x38, true)
        case "(": return (0x39, true)
        case ")": return (0x30, true)
        case ";": return (0xBA, false)
        case ":": return (0xBA, true)
        case "=": return (0xBB, false)
        case "+": return (0xBB, true)
        case ",": return (0xBC, false)
        case "<": return (0xBC, true)
        case "-": return (0xBD, false)
        case "_": return (0xBD, true)
        case ".": return (0xBE, false)
        case ">": return (0xBE, true)
        case "/": return (0xBF, false)
        case "?": return (0xBF, true)
        case "`": return (0xC0, false)
        case "~": return (0xC0, true)
        case "[": return (0xDB, false)
        case "{": return (0xDB, true)
        case "\\": return (0xDC, false)
        case "|": return (0xDC, true)
        case "]": return (0xDD, false)
        case "}": return (0xDD, true)
        case "'": return (0xDE, false)
        case "\"": return (0xDE, true)
        default: return nil
        }
    }
}
