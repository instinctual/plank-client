#pragma once
#include <linux/input-event-codes.h>
#include <cstdint>

// Qt's Wayland/XCB native scan codes are XKB keycodes (evdev + 8). Preserve
// physical keys, including keypad and left/right modifiers. Do not translate
// through the Client's character layout: the Host owns that layout.
struct QtLinuxRemoteKey { unsigned short vk = 0; bool nonNormalized = false; };
inline QtLinuxRemoteKey qtLinuxRemoteKey(std::uint32_t scan)
{
    if (scan < 8) return {};
    const auto key = scan - 8;
    if (key >= KEY_1 && key <= KEY_9) return {static_cast<unsigned short>(0x31 + key - KEY_1)};
    if (key >= KEY_F1 && key <= KEY_F10) return {static_cast<unsigned short>(0x70 + key - KEY_F1)};
    if (key >= KEY_F13 && key <= KEY_F24) return {static_cast<unsigned short>(0x7c + key - KEY_F13)};
    switch (key) {
    case KEY_A: return {0x41}; case KEY_B: return {0x42}; case KEY_C: return {0x43};
    case KEY_D: return {0x44}; case KEY_E: return {0x45}; case KEY_F: return {0x46};
    case KEY_G: return {0x47}; case KEY_H: return {0x48}; case KEY_I: return {0x49};
    case KEY_J: return {0x4a}; case KEY_K: return {0x4b}; case KEY_L: return {0x4c};
    case KEY_M: return {0x4d}; case KEY_N: return {0x4e}; case KEY_O: return {0x4f};
    case KEY_P: return {0x50}; case KEY_Q: return {0x51}; case KEY_R: return {0x52};
    case KEY_S: return {0x53}; case KEY_T: return {0x54}; case KEY_U: return {0x55};
    case KEY_V: return {0x56}; case KEY_W: return {0x57}; case KEY_X: return {0x58};
    case KEY_Y: return {0x59}; case KEY_Z: return {0x5a}; case KEY_0: return {0x30};
    case KEY_BACKSPACE: return {0x08}; case KEY_TAB: return {0x09}; case KEY_CLEAR: return {0x0c};
    case KEY_ENTER: case KEY_KPENTER: return {0x0d}; case KEY_PAUSE: return {0x13};
    case KEY_CAPSLOCK: return {0x14}; case KEY_ESC: return {0x1b}; case KEY_SPACE: return {0x20};
    case KEY_PAGEUP: return {0x21}; case KEY_PAGEDOWN: return {0x22}; case KEY_END: return {0x23};
    case KEY_HOME: return {0x24}; case KEY_LEFT: return {0x25}; case KEY_UP: return {0x26};
    case KEY_RIGHT: return {0x27}; case KEY_DOWN: return {0x28}; case KEY_SELECT: return {0x29};
    case KEY_SYSRQ: return {0x2c}; case KEY_INSERT: return {0x2d}; case KEY_DELETE: return {0x2e};
    case KEY_HELP: return {0x2f}; case KEY_KP0: return {0x60}; case KEY_KP1: return {0x61};
    case KEY_KP2: return {0x62}; case KEY_KP3: return {0x63}; case KEY_KP4: return {0x64};
    case KEY_KP5: return {0x65}; case KEY_KP6: return {0x66}; case KEY_KP7: return {0x67};
    case KEY_KP8: return {0x68}; case KEY_KP9: return {0x69}; case KEY_KPASTERISK: return {0x6a};
    case KEY_KPPLUS: return {0x6b}; case KEY_KPCOMMA: return {0x6c}; case KEY_KPMINUS: return {0x6d};
    case KEY_KPDOT: return {0x6e}; case KEY_KPSLASH: return {0x6f}; case KEY_F11: return {0x7a};
    case KEY_F12: return {0x7b}; case KEY_NUMLOCK: return {0x90}; case KEY_SCROLLLOCK: return {0x91};
    case KEY_LEFTSHIFT: return {0xa0}; case KEY_RIGHTSHIFT: return {0xa1};
    case KEY_LEFTCTRL: return {0xa2}; case KEY_RIGHTCTRL: return {0xa3};
    case KEY_LEFTALT: return {0xa4}; case KEY_RIGHTALT: return {0xa5};
    case KEY_LEFTMETA: return {0x5b}; case KEY_RIGHTMETA: return {0x5c}; case KEY_COMPOSE: return {0x5d};
    case KEY_BACK: return {0xa6}; case KEY_FORWARD: return {0xa7}; case KEY_REFRESH: return {0xa8};
    case KEY_STOP: return {0xa9}; case KEY_SEARCH: return {0xaa}; case KEY_BOOKMARKS: return {0xab};
    case KEY_HOMEPAGE: return {0xac}; case KEY_SEMICOLON: return {0xba}; case KEY_EQUAL: return {0xbb};
    case KEY_COMMA: return {0xbc}; case KEY_MINUS: return {0xbd}; case KEY_DOT: return {0xbe};
    case KEY_SLASH: return {0xbf}; case KEY_GRAVE: return {0xc0}; case KEY_LEFTBRACE: return {0xdb};
    case KEY_BACKSLASH: return {0xdc}; case KEY_YEN: return {0xdc, true}; case KEY_RIGHTBRACE: return {0xdd};
    case KEY_APOSTROPHE: return {0xde}; case KEY_102ND: return {0xe2}; case KEY_RO: return {0xe2, true};
    case KEY_HANGEUL: return {0x1c}; case KEY_HANJA: return {0x1d};
    default: return {};
    }
}
