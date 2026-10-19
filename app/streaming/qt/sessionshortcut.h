#pragma once

#include "linuxkeymap.h"
#include <QtCore/Qt>

inline int qtSessionShortcut(quint32 scan, int key, Qt::KeyboardModifiers modifiers)
{
    constexpr auto required = Qt::ControlModifier | Qt::AltModifier | Qt::ShiftModifier;
    if ((modifiers & required) != required) return 0;
    const auto special = [](int code) { return code == Qt::Key_Q || code == Qt::Key_Z || code == Qt::Key_X ||
        code == Qt::Key_S || code == Qt::Key_D || code == Qt::Key_V || code == Qt::Key_L || code == Qt::Key_K; };
    // Match the displayed Latin key first, then the physical position for
    // non-Latin layouts, just as the ordinary Client does.
    if (special(key)) return key;
    const int physical = qtLinuxRemoteKey(scan).vk;
    return special(physical) ? physical : 0;
}
