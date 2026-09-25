#include <QtTest>
#include <QQmlEngine>
#include <QQmlComponent>
#include <QGuiApplication>
#include <QFile>
#include <QRegularExpression>
#include "streamingpreferences.h"

class FakeManager : public QObject {
    Q_OBJECT
public:
    int sequence = 0;
    Q_INVOKABLE int probeHostPlatform(QString address) { emit requested(address); return ++sequence; }
signals:
    void requested(QString address);
    void hostPlatformDetected(int requestId, QString address, int platform);
};
class FakePreferences : public QObject {
    Q_OBJECT
public:
    enum Capture {
        PLANK_CAPTURE_NVFBC_8BIT = StreamingPreferences::PLANK_CAPTURE_NVFBC_8BIT,
        PLANK_CAPTURE_X11_NATIVE10 = StreamingPreferences::PLANK_CAPTURE_X11_NATIVE10,
        PLANK_CAPTURE_SCREENCAPTUREKIT = StreamingPreferences::PLANK_CAPTURE_SCREENCAPTUREKIT
    };
    Q_ENUM(Capture)
};
class HostChoicesTest : public QObject {
    Q_OBJECT
private slots:
    void bookmarkLayoutLabels_data() {
        QTest::addColumn<QString>("filename");
        QTest::addColumn<QString>("choiceId");
        QTest::addColumn<QString>("captureId");
        QTest::newRow("create") << "main.qml" << "addHostLayout" << "addCaptureSource";
        QTest::newRow("edit") << "PcView.qml" << "editHostLayout" << "editCaptureSource";
    }

    void bookmarkLayoutLabels() {
        QFETCH(QString, filename);
        QFETCH(QString, choiceId);
        QFETCH(QString, captureId);
        const QString guiPath = QString::fromUtf8(qgetenv("PLANK_CLIENT_SOURCE")) + "/app/gui/";
        QFile source(guiPath + filename);
        QVERIFY(source.open(QIODevice::ReadOnly));
        // Exercise the actual bookmark dropdown, without the full application's
        // networking and singleton setup. Keep its option indices unchanged.
        const QRegularExpression dropdown(
                    "PlankComboBox \\{\\s+id: " + choiceId + "\\b.*?\\n\\s*\\}",
                    QRegularExpression::DotMatchesEverythingOption);
        const auto match = dropdown.match(QString::fromUtf8(source.readAll()));
        QVERIFY(match.hasMatch());
        const QString fixture = QStringLiteral(
                    "import QtQuick\nimport QtQuick.Controls\nimport QtQuick.Layouts\n"
                    "import \".\"\nItem { id: fixture; property int captureSource: 0; "
                    "property alias choice: %1; "
                    "QtObject { id: %2; property int captureSource: fixture.captureSource }\n%3\n}")
                .arg(choiceId, captureId, match.captured());
        QQmlEngine engine;
        QQmlComponent component(&engine);
        component.setData(fixture.toUtf8(), QUrl::fromLocalFile(guiPath + "LayoutLabelTest.qml"));
        QScopedPointer<QObject> root(component.create());
        QVERIFY2(root, qPrintable(component.errorString()));
        QObject* choice = root->property("choice").value<QObject*>();
        QVERIFY(choice);
        for (int captureSource : {0, 1, 2}) {
            QVERIFY(root->setProperty("captureSource", captureSource));
            const QStringList expected = captureSource == 2
                    ? QStringList{"Match client display(s)", "One Mac virtual display"}
                    : QStringList{"Match client displays", "Match Host", "One virtual display",
                                  "Two virtual displays (horizontal)"};
            QCOMPARE(choice->property("count").toInt(), expected.size());
            for (int index = 0; index < expected.size(); ++index) {
                QVERIFY(choice->setProperty("currentIndex", index));
                QCOMPARE(choice->property("currentText").toString(), expected[index]);
            }
        }
    }

    void filtersAndIgnoresStaleReplies() {
        FakeManager manager;
        qmlRegisterSingletonInstance("ComputerManager", 1, 0, "ComputerManager", &manager);
        qmlRegisterUncreatableType<FakePreferences>("StreamingPreferences", 1, 0, "StreamingPreferences", "enums only");
        QQmlEngine engine;
        QQmlComponent component(&engine, QUrl::fromLocalFile(QString::fromUtf8(qgetenv("PLANK_CLIENT_SOURCE")) + "/app/gui/PlankCaptureSourceBox.qml"));
        QScopedPointer<QObject> box(component.create());
        QVERIFY2(box, qPrintable(component.errorString()));
        QCOMPARE(box->property("count").toInt(), 3);
        box->setProperty("hostAddress", "mac.test");
        box->setProperty("probingEnabled", true);
        QTRY_COMPARE(manager.sequence, 1);
        emit manager.hostPlatformDetected(1, "mac.test", 2);
        QCOMPARE(box->property("count").toInt(), 1);
        QCOMPARE(box->property("captureSource").toInt(), 2);
        QCOMPARE(box->property("currentText").toString(), QStringLiteral("ScreenCaptureKit — macOS"));
        box->setProperty("hostAddress", "linux.test");
        QCOMPARE(box->property("count").toInt(), 3);
        emit manager.hostPlatformDetected(1, "mac.test", 2);
        QCOMPARE(box->property("count").toInt(), 3);
        QTRY_COMPARE(manager.sequence, 2);
        emit manager.hostPlatformDetected(2, "linux.test", 1);
        QCOMPARE(box->property("count").toInt(), 2);
        QCOMPARE(box->property("captureSource").toInt(), 0);
        box->setProperty("currentIndex", 1);
        QCOMPARE(box->property("captureSource").toInt(), 1);
        QCOMPARE(box->property("currentText").toString(), QStringLiteral("Native X11/XShm — 10-bit (Experimental)"));
        box->setProperty("hostAddress", "offline.test");
        QTRY_COMPARE(manager.sequence, 3);
        emit manager.hostPlatformDetected(3, "offline.test", 0);
        QCOMPARE(box->property("count").toInt(), 3);
        QCOMPARE(box->property("captureSource").toInt(), 1);
        box->setProperty("probingEnabled", false);
        emit manager.hostPlatformDetected(3, "offline.test", 2);
        QCOMPARE(box->property("count").toInt(), 3);
    }
};
QTEST_MAIN(HostChoicesTest)
#include "test_hostchoices.moc"
