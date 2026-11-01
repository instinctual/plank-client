#include "systemproperties.h"
#include "utils.h"
#include "clientdisplays.h"

#include <QGuiApplication>
#include <QFile>
#include <QLibraryInfo>
#include <QScreen>
#include <QSysInfo>
#include <QThread>

#ifndef PLANK_VERSION_STR
#define PLANK_VERSION_STR "development"
#endif

#ifdef Q_OS_WIN32
#define WIN32_LEAN_AND_MEAN
#include <Windows.h>
#endif

SystemProperties::SystemProperties()
{
    plankVersionString = QString::fromLatin1(PLANK_VERSION_STR);
    hasDesktopEnvironment = WMUtils::isRunningDesktopEnvironment();
    isRunningWayland = WMUtils::isRunningWayland();
    isRunningXWayland = isRunningWayland && QGuiApplication::platformName() == "xcb";
    usesMaterial3Theme = QLibraryInfo::version() >= QVersionNumber(6, 5, 0);
    QString nativeArch = QSysInfo::currentCpuArchitecture();

#ifdef Q_OS_WIN32
    {
        USHORT processArch, machineArch;

        // Use IsWow64Process2 on TH2 and later, because it supports ARM64
        auto fnIsWow64Process2 = (decltype(IsWow64Process2)*)GetProcAddress(GetModuleHandleA("kernel32.dll"), "IsWow64Process2");
        if (fnIsWow64Process2 != nullptr && fnIsWow64Process2(GetCurrentProcess(), &processArch, &machineArch)) {
            switch (machineArch) {
            case IMAGE_FILE_MACHINE_I386:
                nativeArch = "i386";
                break;
            case IMAGE_FILE_MACHINE_AMD64:
                nativeArch = "x86_64";
                break;
            case IMAGE_FILE_MACHINE_ARM64:
                nativeArch = "arm64";
                break;
            }
        }

        isWow64 = nativeArch != QSysInfo::buildCpuArchitecture();
    }
#else
    isWow64 = false;
#endif

    if (nativeArch == "i386") {
        friendlyNativeArchName = "x86";
    }
    else if (nativeArch == "x86_64") {
        friendlyNativeArchName = "x64";
    }
    else {
        friendlyNativeArchName = nativeArch.toUpper();
    }

    const auto observe = [this](QScreen* screen) {
        connect(screen, &QScreen::geometryChanged, this, &SystemProperties::clientDisplaysChanged);
    };
    for (auto* screen : QGuiApplication::screens()) observe(screen);
    connect(qGuiApp, &QGuiApplication::screenAdded, this, [this, observe](QScreen* screen) {
        observe(screen); emit clientDisplaysChanged();
    });
    connect(qGuiApp, &QGuiApplication::screenRemoved, this, &SystemProperties::clientDisplaysChanged);
    // The exact-profile decoder is qualified on the actual Qt session surface.
    // A temporary SDL window would probe a different GPU/context and reintroduce
    // the independent SDL Wayland connection this frontend deliberately avoids.
    hardwareCapabilityKnown = false;
    maximumResolution = QSize(8192, 8192);
    refreshDisplays();

    Q_ASSERT(!monitorRefreshRates.isEmpty());
    Q_ASSERT(!monitorNativeResolutions.isEmpty());
    Q_ASSERT(!monitorSafeAreaResolutions.isEmpty());
}

QString SystemProperties::plankChangelog() const
{
    // Release notes travel with the installed Client; no network or Host lookup.
    QFile notes(QStringLiteral(":/res/changelog.md"));
    return notes.open(QIODevice::ReadOnly) ? QString::fromUtf8(notes.readAll()) : QString();
}

QVariantList SystemProperties::clientDisplayChoices(const QString& saved) const
{
    return ClientDisplays::choices(saved);
}

QRect SystemProperties::getNativeResolution(int displayIndex)
{
    // Returns default constructed QRect if out of bounds
    return monitorNativeResolutions.value(displayIndex);
}

QRect SystemProperties::getSafeAreaResolution(int displayIndex)
{
    // Returns default constructed QRect if out of bounds
    return monitorSafeAreaResolutions.value(displayIndex);
}

int SystemProperties::getRefreshRate(int displayIndex)
{
    // Returns 0 if out of bounds
    return monitorRefreshRates.value(displayIndex);
}

void SystemProperties::refreshDisplays()
{
    refreshDisplaysInternal();
}

void SystemProperties::refreshDisplaysInternal()
{
    Q_ASSERT(QThread::currentThread() == qGuiApp->thread());
    monitorNativeResolutions.clear();
    monitorSafeAreaResolutions.clear();
    monitorRefreshRates.clear();
    for (auto* screen : QGuiApplication::screens()) {
        const auto scale = screen->devicePixelRatio();
        monitorNativeResolutions.append(QRect(QPoint(), screen->geometry().size() * scale));
        monitorSafeAreaResolutions.append(QRect(QPoint(), screen->availableGeometry().size() * scale));
        const auto refresh = screen->refreshRate();
        monitorRefreshRates.append(refresh >= 58 && refresh <= 62 ? 60 :
                                   refresh >= 28 && refresh <= 32 ? 30 : qRound(refresh));
    }
}
