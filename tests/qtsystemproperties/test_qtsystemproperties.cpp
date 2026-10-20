#include <QtTest>
#include <QGuiApplication>
#include <QScreen>
#include "../../app/backend/systemproperties.h"

// These platform predicates need no session or SDL dependency. In particular,
// this test links no SDL library: launcher display enumeration must be Qt-only.
namespace WMUtils {
bool isRunningDesktopEnvironment() { return true; }
bool isRunningWayland() { return QGuiApplication::platformName() == "wayland"; }
}

class QtSystemPropertiesTest : public QObject
{
    Q_OBJECT
private slots:
    void decoderQualificationIsDeferred()
    {
        SystemProperties properties;
        QVERIFY(!properties.property("hardwareCapabilityKnown").toBool());
        QVERIFY(!properties.property("rendererAlwaysFullScreen").toBool());
    }

    void settingsDescribeAutomaticRecovery()
    {
        SystemProperties properties;
        QVERIFY(properties.property("automaticSessionRecovery").toBool());
    }

    void nativeDisplayGeometryAndRepeatedRefresh()
    {
        SystemProperties properties;
        const auto screens = QGuiApplication::screens();
        QVERIFY(!screens.isEmpty());
        for (int refresh = 0; refresh < 3; ++refresh) {
            properties.refreshDisplays();
            for (int i = 0; i < screens.size(); ++i) {
                const auto screen = screens[i];
                QCOMPARE(properties.getNativeResolution(i).size(),
                         screen->geometry().size() * screen->devicePixelRatio());
                QCOMPARE(properties.getSafeAreaResolution(i).size(),
                         screen->availableGeometry().size() * screen->devicePixelRatio());
                QVERIFY(properties.getRefreshRate(i) > 0);
            }
            QVERIFY(properties.getNativeResolution(screens.size()).isNull());
            QVERIFY(properties.getSafeAreaResolution(screens.size()).isNull());
            QCOMPARE(properties.getRefreshRate(screens.size()), 0);
        }
    }
};
QTEST_MAIN(QtSystemPropertiesTest)
#include "test_qtsystemproperties.moc"
