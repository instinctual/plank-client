#include "test_internetconnectivity.h"
#include "../../app/backend/internetconnectivity.h"

#include <QtTest>
#include <QDir>
#include <QFile>
#include <QGuiApplication>
#include <QQmlComponent>
#include <QQmlContext>
#include <QQmlEngine>
#include <QQuickItem>
#include <QQuickStyle>
#include <QQuickWindow>
#include <QTemporaryDir>

using Monitor = InternetConnectivity;
using Settings = PlankClientPolicy::InternetCheck;

static Monitor::PingCommand stub(int code, int delayMs = 0, const QString& error = QString(),
                                const QString& output = QString())
{
    return {QCoreApplication::applicationFilePath(),
            {QStringLiteral("--ping-stub"), QString::number(code), QString::number(delayMs), error, output}, 1};
}

static void showSummary(Monitor& monitor)
{
    monitor.setSummaryVisible(true);
    monitor.setWindowVisible(true);
}

static QQuickItem* findVisualItem(QQuickItem* parent, const QString& name)
{
    // Repeater delegates have visual parents distinct from QObject ownership.
    if (parent->objectName() == name) return parent;
    for (auto* child : parent->childItems())
        if (auto* found = findVisualItem(child, name)) return found;
    return nullptr;
}

void ConnectivityTest::policyDefaultsAndExplicitValues()
{
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    const QString path = dir.filePath("client.conf");
    auto config = PlankClientPolicy(path).internetCheck();
    QVERIFY(config.enabled);
    QCOMPARE(config.hosts, QStringList({"8.8.8.8", "1.1.1.1", "9.9.9.9"}));
    QCOMPARE(config.intervalMs, 15000);
    QCOMPARE(config.timeoutMs, 1000);
    QVERIFY(config.error.isEmpty());
    QFile file(path);
    QVERIFY(file.open(QIODevice::WriteOnly));
    file.write("[network]\ninternet_check_hosts = 192.0.2.1, 198.51.100.1\n"
               "internet_check_interval_seconds = 30\ninternet_check_timeout_ms = 700\n");
    file.close();
    config = PlankClientPolicy(path).internetCheck();
    QCOMPARE(config.hosts, QStringList({"192.0.2.1", "198.51.100.1"}));
    QCOMPARE(config.intervalMs, 30000);
    QCOMPARE(config.timeoutMs, 700);
    QVERIFY(config.error.isEmpty());
}

void ConnectivityTest::invalidPolicy_data()
{
    QTest::addColumn<QByteArray>("line");
    QTest::newRow("empty targets") << QByteArray("internet_check_hosts =");
    QTest::newRow("option injection") << QByteArray("internet_check_hosts = -f");
    QTest::newRow("shell injection") << QByteArray("internet_check_hosts = \"192.0.2.1;exit 0\"");
    QTest::newRow("DNS name") << QByteArray("internet_check_hosts = example.com");
    QTest::newRow("multicast") << QByteArray("internet_check_hosts = 224.0.0.1");
    QTest::newRow("IPv6") << QByteArray("internet_check_hosts = 2001:db8::1");
    QTest::newRow("IPv4-mapped IPv6") << QByteArray("internet_check_hosts = ::ffff:192.0.2.1");
    QTest::newRow("unspecified") << QByteArray("internet_check_hosts = 0.0.0.0");
    QTest::newRow("broadcast") << QByteArray("internet_check_hosts = 255.255.255.255");
    QTest::newRow("scoped") << QByteArray("internet_check_hosts = fe80::1%eth0");
    QTest::newRow("empty entry") << QByteArray("internet_check_hosts = 192.0.2.1,,192.0.2.2");
    QTest::newRow("too many") << QByteArray("internet_check_hosts = 192.0.2.1,192.0.2.2,192.0.2.3,192.0.2.4,192.0.2.5,192.0.2.6,192.0.2.7,192.0.2.8,192.0.2.9");
    QTest::newRow("short interval") << QByteArray("internet_check_interval_seconds = 0");
    QTest::newRow("large interval") << QByteArray("internet_check_interval_seconds = 3601");
    QTest::newRow("overflow interval") << QByteArray("internet_check_interval_seconds = 4294967296");
    QTest::newRow("short timeout") << QByteArray("internet_check_timeout_ms = 99");
    QTest::newRow("long timeout") << QByteArray("internet_check_timeout_ms = 10001");
    QTest::newRow("invalid timeout") << QByteArray("internet_check_timeout_ms = forever");
}

void ConnectivityTest::invalidPolicy()
{
    QFETCH(QByteArray, line);
    QTemporaryDir dir;
    QFile file(dir.filePath("client.conf"));
    QVERIFY(file.open(QIODevice::WriteOnly));
    file.write("[network]\n" + line + "\n");
    file.close();
    int attempts = 0;
    Monitor monitor(PlankClientPolicy(file.fileName()).internetCheck(), nullptr,
                    [&attempts](const QString&, int) { ++attempts; return stub(0); });
    monitor.setWindowVisible(true);
    monitor.checkNow();
    showSummary(monitor);
    QCOMPARE(monitor.state(), Monitor::Unavailable);
    QVERIFY(!monitor.summaryError().isEmpty());
    QVERIFY(!monitor.summaryRunning());
    QCOMPARE(attempts, 0); // Never fall back to public IPs after a config error.
}

void ConnectivityTest::disabledMakesNoProbes()
{
    Settings settings;
    settings.enabled = false;
    int attempts = 0;
    Monitor monitor(settings, nullptr, [&attempts](const QString&, int) { ++attempts; return stub(0); });
    monitor.setWindowVisible(true);
    monitor.checkNow();
    monitor.networkChanged();
    monitor.refreshIfStale();
    showSummary(monitor);
    QTest::qWait(300);
    QCOMPARE(monitor.state(), Monitor::Disabled);
    QCOMPARE(attempts, 0);
    QVERIFY(!monitor.summaryRunning());
    QVERIFY(!monitor.summaryError().isEmpty());
}

void ConnectivityTest::orderedFallbackAndPeriodicRestart()
{
    Settings settings;
    settings.intervalMs = 1100;
    QStringList attempted;
    Monitor monitor(settings, nullptr, [&attempted](const QString& host, int timeout) {
        attempted.append(host);
        Q_ASSERT(timeout == 1000);
        return stub(host == "1.1.1.1" ? 0 : 1);
    });
    QCOMPARE(monitor.state(), Monitor::Checking);
    monitor.setWindowVisible(true);
    QTRY_COMPARE(monitor.state(), Monitor::Reachable);
    QCOMPARE(attempted, QStringList({"8.8.8.8", "1.1.1.1"}));
    QSignalSpy updates(&monitor, &Monitor::stateChanged);
    QTRY_COMPARE_WITH_TIMEOUT(attempted.size(), 4, 3000);
    QTest::qWait(50);
    QCOMPARE(attempted, QStringList({"8.8.8.8", "1.1.1.1", "8.8.8.8", "1.1.1.1"}));
    QCOMPARE(monitor.state(), Monitor::Reachable);
    QCOMPARE(updates.size(), 0); // No Checking flicker between periodic rounds.
    monitor.stop();
}

void ConnectivityTest::noResponseRecoversWithoutInteraction()
{
    Settings settings;
    bool online = false;
    int attempts = 0;
    Monitor monitor(settings, nullptr, [&online, &attempts](const QString&, int) {
        ++attempts;
        return stub(online ? 0 : 1);
    });
    monitor.setWindowVisible(true);
    QTRY_COMPARE(monitor.state(), Monitor::NoResponse);
    QCOMPARE(attempts, 3);
    online = true;
    monitor.networkChanged();
    QTRY_COMPARE(monitor.state(), Monitor::Reachable);
    QCOMPARE(attempts, 4);
    monitor.refreshIfStale();
    QCOMPARE(attempts, 4); // Focus changes cannot produce a ping storm.
}

void ConnectivityTest::deadlineDoesNotBlockUiOrLeaveChildren()
{
    Settings settings;
    settings.timeoutMs = 100;
    Monitor monitor(settings, nullptr, [](const QString&, int) { return stub(0, 60000); });
    QElapsedTimer elapsed;
    elapsed.start();
    monitor.setWindowVisible(true);
    bool responsiveWhilePending = false;
    QTimer::singleShot(0, &monitor, [&] { responsiveWhilePending = monitor.state() == Monitor::Checking; });
    QTRY_COMPARE_WITH_TIMEOUT(monitor.state(), Monitor::NoResponse, 2500);
    QVERIFY(elapsed.elapsed() < 2500);
    QVERIFY(responsiveWhilePending);
    QTRY_VERIFY(monitor.findChildren<QProcess*>().isEmpty());
}

void ConnectivityTest::unavailableIsNotAnInternetOutage()
{
    Settings settings;
    settings.hosts = {"192.0.2.1"};
    Monitor monitor(settings, nullptr, [](const QString&, int) {
        return Monitor::PingCommand{QStringLiteral("/no-such-plank-test-ping"), {}, 1};
    });
    monitor.setWindowVisible(true);
    QTRY_COMPARE(monitor.state(), Monitor::Unavailable);
    QTRY_VERIFY(monitor.findChildren<QProcess*>().isEmpty());
}

void ConnectivityTest::probeErrorCanFallBack()
{
    Settings settings;
    int attempts = 0;
    Monitor monitor(settings, nullptr, [&attempts](const QString&, int) {
        return stub(++attempts == 1 ? 64 : 0);
    });
    monitor.setWindowVisible(true);
    QTRY_COMPARE(monitor.state(), Monitor::Reachable);
    QCOMPARE(attempts, 2);
}

void ConnectivityTest::networkErrorIsNotUtilityFailure_data()
{
    QTest::addColumn<QString>("error");
    QTest::addColumn<int>("expectedState");
    QTest::newRow("no route") << QStringLiteral("ping: connect: Network is unreachable") << int(Monitor::NoResponse);
    QTest::newRow("host unreachable") << QStringLiteral("ping: sendto: No route to host") << int(Monitor::NoResponse);
    QTest::newRow("permission denied") << QStringLiteral("ping: socket: Operation not permitted") << int(Monitor::Unavailable);
}

void ConnectivityTest::networkErrorIsNotUtilityFailure()
{
    QFETCH(QString, error);
    QFETCH(int, expectedState);
    Settings settings;
    settings.hosts = {"192.0.2.1"};
    Monitor monitor(settings, nullptr, [error](const QString&, int) { return stub(64, 0, error); });
    monitor.setWindowVisible(true);
    QTRY_COMPARE(int(monitor.state()), expectedState);
}

void ConnectivityTest::restartAndStopDiscardCancelledResults()
{
    Settings settings;
    settings.hosts = {"192.0.2.1"};
    settings.timeoutMs = 1000;
    int attempts = 0;
    Monitor monitor(settings, nullptr, [&attempts](const QString&, int) {
        return ++attempts == 1 ? stub(0, 500) : stub(1);
    });
    QSignalSpy updates(&monitor, &Monitor::stateChanged);
    monitor.setWindowVisible(true);
    QTest::qWait(50);
    monitor.checkNow();
    monitor.checkNow(); // Coalesces while cancellation is still in progress.
    QTRY_COMPARE(monitor.state(), Monitor::NoResponse);
    QCOMPARE(attempts, 2);
    QCOMPARE(updates.size(), 1);
    QTest::qWait(550);
    QCOMPARE(monitor.state(), Monitor::NoResponse);
    monitor.stop();
    monitor.checkNow();
    QCOMPARE(attempts, 2);
    QTRY_VERIFY(monitor.findChildren<QProcess*>().isEmpty());

    Monitor stopped(settings, nullptr, [](const QString&, int) { return stub(0, 60000); });
    QSignalSpy stoppedUpdates(&stopped, &Monitor::stateChanged);
    stopped.setWindowVisible(true);
    QTest::qWait(50);
    stopped.stop();
    QTRY_VERIFY(stopped.findChildren<QProcess*>().isEmpty());
    QCOMPARE(stoppedUpdates.size(), 0);
}

void ConnectivityTest::systemPingLoopback()
{
    Settings settings;
    settings.hosts = {"127.0.0.1"};
    settings.timeoutMs = 2000;
    // Use the actual OS utility as an ordinary runner account. No public
    // Internet dependency and no elevated process or new PLANK capability.
    Monitor monitor(settings);
    monitor.setWindowVisible(true);
    QTRY_COMPARE_WITH_TIMEOUT(monitor.state(), Monitor::Reachable, 4000);
    // Exercise varying real timings: macOS's unused single-sample standard
    // deviation can be NaN, even though the RTT is valid.
    for (int attempt = 0; attempt < 16; ++attempt) {
        showSummary(monitor);
        monitor.checkNow();
        QTRY_VERIFY_WITH_TIMEOUT(!monitor.summaryRunning(), 4000);
        QCOMPARE(monitor.summaryResults().size(), 1);
        const auto row = monitor.summaryResults().first().toMap();
        QCOMPARE(row.value("state").toInt(), int(Monitor::Reachable));
        QVERIFY(row.value("roundTripMs").toDouble() >= 0);
    }
}

void ConnectivityTest::summaryChecksEveryTargetConcurrently()
{
    Settings settings;
    settings.hosts = {"192.0.2.1", "192.0.2.2", "192.0.2.3"};
    QStringList attempted;
    Monitor monitor(settings, nullptr, [&attempted](const QString& address, int timeout) {
        attempted.append(address);
        Q_ASSERT(timeout == 1000);
        if (address.endsWith(".1")) return stub(0, 250, {}, "rtt min/avg/max/mdev = 12.125/12.125/12.125/0.000 ms\n");
        if (address.endsWith(".2")) return stub(1, 10);
        return stub(64, 50, "ping: socket: Operation not permitted");
    });
    QSignalSpy backgroundUpdates(&monitor, &Monitor::stateChanged);
    showSummary(monitor);
    QCOMPARE(attempted, settings.hosts); // All targets start before any result.
    QVERIFY(monitor.summaryRunning());
    QTRY_VERIFY(!monitor.summaryRunning());
    const auto rows = monitor.summaryResults();
    QCOMPARE(rows.size(), 3);
    for (int i = 0; i < rows.size(); ++i)
        QCOMPARE(rows[i].toMap().value("address").toString(), settings.hosts[i]);
    QCOMPARE(rows[0].toMap().value("state").toInt(), int(Monitor::Reachable));
    QCOMPARE(rows[0].toMap().value("roundTripMs").toDouble(), 12.125);
    QCOMPARE(rows[1].toMap().value("state").toInt(), int(Monitor::NoResponse));
    QCOMPARE(rows[1].toMap().value("roundTripMs").toDouble(), -1.0);
    QCOMPARE(rows[2].toMap().value("state").toInt(), int(Monitor::Unavailable));
    QCOMPARE(backgroundUpdates.size(), 1); // Live results also serve the main indicator.
    QCOMPARE(monitor.state(), Monitor::Reachable);
    QTRY_VERIFY(monitor.findChildren<QProcess*>().isEmpty());
}

void ConnectivityTest::summaryRoundTripFormats_data()
{
    QTest::addColumn<QString>("output");
    QTest::addColumn<double>("expectedMs");
    QTest::newRow("iputils") << QString("PING example\n\nrtt min/avg/max/mdev = 8.125/8.125/8.125/0.000 ms\n") << 8.125;
    QTest::newRow("macOS") << QString("round-trip min/avg/max/stddev = 0.187/0.187/0.187/0.000 ms\n") << 0.187;
    QTest::newRow("macOS NaN deviation") << QString("round-trip min/avg/max/stddev = 0.187/0.187/0.187/nan ms\n") << 0.187;
    QTest::newRow("macOS negative NaN deviation") << QString("round-trip min/avg/max/stddev = 0.187/0.187/0.187/-nan ms\n") << 0.187;
    QTest::newRow("missing") << QString("reply received\n") << -1.0;
    QTest::newRow("malformed") << QString("rtt min/avg/max/mdev = 1.0/NaN/1.0/0.0 ms\n") << -1.0;
    QTest::newRow("negative RTT") << QString("rtt min/avg/max/mdev = 1.0/-1.0/1.0/0.0 ms\n") << -1.0;
    QTest::newRow("infinite RTT") << QString("rtt min/avg/max/mdev = 1.0/inf/1.0/0.0 ms\n") << -1.0;
    QTest::newRow("bounded output") << (QString(5000, 'x') + "\nrtt min/avg/max/mdev = 1/1/1/0 ms\n") << -1.0;
}

void ConnectivityTest::summaryRoundTripFormats()
{
    QFETCH(QString, output);
    QFETCH(double, expectedMs);
    Settings settings;
    settings.hosts = {"192.0.2.1"};
    Monitor monitor(settings, nullptr, [output](const QString&, int) { return stub(0, 80, {}, output); });
    showSummary(monitor);
    QTRY_VERIFY(!monitor.summaryRunning());
    const auto row = monitor.summaryResults().first().toMap();
    QCOMPARE(row.value("state").toInt(), int(Monitor::Reachable));
    QCOMPARE(row.value("roundTripMs").toDouble(), expectedMs);
}

void ConnectivityTest::summaryFailuresAndDeadlines()
{
    Settings settings;
    settings.hosts = {"192.0.2.1", "192.0.2.2", "192.0.2.3"};
    settings.timeoutMs = 200;
    Monitor monitor(settings, nullptr, [](const QString& address, int) {
        if (address.endsWith(".1")) return stub(0, 60000);
        if (address.endsWith(".2")) return Monitor::PingCommand{"/no-such-plank-test-ping", {}, 1};
        return stub(64, 0, "ping: sendto: No route to host");
    });
    showSummary(monitor);
    // Require an event-loop turn while the probe is pending. QTRY's wait
    // coalesces timer deliveries, so counting 10 ms ticks is not reliable.
    // A synchronous wait would finish the summary before this can run.
    bool responsiveWhilePending = false;
    QTimer::singleShot(0, &monitor, [&] { responsiveWhilePending = monitor.summaryRunning(); });
    QTRY_VERIFY_WITH_TIMEOUT(!monitor.summaryRunning(), 2500);
    QVERIFY(responsiveWhilePending);
    const auto rows = monitor.summaryResults();
    QCOMPARE(rows[0].toMap().value("state").toInt(), int(Monitor::NoResponse));
    QCOMPARE(rows[1].toMap().value("state").toInt(), int(Monitor::Unavailable));
    QCOMPARE(rows[2].toMap().value("state").toInt(), int(Monitor::NoResponse));
    QTRY_VERIFY(monitor.findChildren<QProcess*>().isEmpty());
}

void ConnectivityTest::summaryCancellationAndReopen()
{
    Settings settings;
    settings.hosts = {"192.0.2.1", "192.0.2.2"};
    int attempts = 0;
    bool slow = true;
    Monitor monitor(settings, nullptr, [&attempts, &slow](const QString&, int) {
        ++attempts;
        return stub(0, slow ? 60000 : 10, {}, "rtt min/avg/max/mdev = 9/9/9/0 ms\n");
    });
    showSummary(monitor);
    QCOMPARE(attempts, 2);
    // Close/reopen/refresh immediately, including processes still starting.
    monitor.setSummaryVisible(false);
    slow = false;
    showSummary(monitor);
    monitor.checkNow();
    QTRY_VERIFY(!monitor.summaryRunning());
    QCOMPARE(attempts, 4);
    for (const auto& row : monitor.summaryResults()) {
        QCOMPARE(row.toMap().value("state").toInt(), int(Monitor::Reachable));
        QCOMPARE(row.toMap().value("roundTripMs").toDouble(), 9.0);
    }
    QTRY_VERIFY(monitor.findChildren<QProcess*>().isEmpty());
    slow = true;
    monitor.checkNow();
    monitor.stop();
    QTRY_VERIFY(monitor.findChildren<QProcess*>().isEmpty());
    QVERIFY(!monitor.summaryRunning());
    for (const auto& row : monitor.summaryResults())
        QCOMPARE(row.toMap().value("state").toInt(), int(Monitor::Reachable));
    QCOMPARE(attempts, 6);
}

void ConnectivityTest::hiddenWindowStopsAndResumes()
{
    Settings settings;
    settings.hosts = {"192.0.2.1", "192.0.2.2"};
    settings.intervalMs = 1100;
    settings.timeoutMs = 2500;
    int attempts = 0;
    bool slow = false;
    Monitor monitor(settings, nullptr, [&](const QString&, int) {
        ++attempts;
        return stub(0, slow ? 60000 : 10, {}, "rtt min/avg/max/mdev = 9/9/9/0 ms\n");
    });
    monitor.networkChanged();
    monitor.refreshIfStale();
    monitor.checkNow();
    QTest::qWait(300);
    QCOMPARE(attempts, 0);
    monitor.setWindowVisible(true);
    QTRY_COMPARE(monitor.state(), Monitor::Reachable);
    QCOMPARE(attempts, 1);
    monitor.setWindowVisible(false);
    monitor.networkChanged();
    monitor.checkNow();
    QTest::qWait(1300);
    QCOMPARE(attempts, 1); // The prior periodic timer cannot restart hidden work.

    slow = true;
    monitor.setSummaryVisible(true);
    QCOMPARE(attempts, 1); // A dialog flag cannot override a hidden main window.
    monitor.setWindowVisible(true);
    QCOMPARE(attempts, 3);
    QVERIFY(monitor.summaryRunning());
    monitor.setWindowVisible(false);
    monitor.networkChanged();
    QTRY_VERIFY(monitor.findChildren<QProcess*>().isEmpty());
    QTest::qWait(1100);
    QCOMPARE(attempts, 3);
    slow = false;
    monitor.setWindowVisible(true);
    QTRY_VERIFY(!monitor.summaryRunning());
    QCOMPARE(attempts, 5);
    monitor.setSummaryVisible(false);
    QTRY_VERIFY(monitor.findChildren<QProcess*>().isEmpty());
    QCOMPARE(attempts, 6); // Closing live mode resumes the ordered background check.
    QTRY_COMPARE_WITH_TIMEOUT(attempts, 7, 2500);
    monitor.stop();
    QTRY_VERIFY(monitor.findChildren<QProcess*>().isEmpty());
}

void ConnectivityTest::liveSummaryPreservesResultsAndDoesNotOverlap()
{
    Settings settings;
    settings.hosts = {"192.0.2.1", "192.0.2.2"};
    settings.timeoutMs = 2500;
    int attempts = 0;
    bool overlap = false, retainedResults = false;
    Monitor* observer = nullptr;
    Monitor monitor(settings, nullptr, [&](const QString&, int) {
        for (auto* process : observer->findChildren<QProcess*>())
            overlap |= process->state() != QProcess::NotRunning;
        const bool first = attempts < 2;
        if (attempts == 2) {
            const auto rows = observer->summaryResults();
            retainedResults = rows.size() == 2;
            for (const auto& row : rows)
                retainedResults &= row.toMap().value("state").toInt() == Monitor::Reachable &&
                    row.toMap().value("roundTripMs").toDouble() == 12;
        }
        ++attempts;
        return stub(0, first ? 1500 : 200, {}, first ?
            "rtt min/avg/max/mdev = 12/12/12/0 ms\n" : "rtt min/avg/max/mdev = 30/30/30/0 ms\n");
    });
    observer = &monitor;
    showSummary(monitor);
    QTest::qWait(1100);
    QCOMPARE(attempts, 2); // A slow round may exceed the one-second live cadence.
    QTRY_VERIFY_WITH_TIMEOUT(attempts >= 4, 4000);
    QVERIFY(!overlap && retainedResults);
    QTRY_COMPARE(monitor.summaryResults().first().toMap().value("roundTripMs").toDouble(), 30.0);
    monitor.stop();
    QTRY_VERIFY(monitor.findChildren<QProcess*>().isEmpty());
    const int stoppedAt = attempts;
    QTest::qWait(1100);
    QCOMPARE(attempts, stoppedAt);
}

void ConnectivityTest::statusUi()
{
    qmlRegisterUncreatableType<Monitor>("InternetConnectivity", 1, 0, "InternetConnectivity", "test");
    Settings settings;
    settings.hosts = {"192.0.2.1", "192.0.2.2", "192.0.2.3"};
    bool online = false;
    int attempts = 0;
    Monitor monitor(settings, nullptr, [&online, &attempts](const QString& address, int) {
        ++attempts;
        if (!online || address.endsWith(".2")) return stub(1, 10);
        if (address.endsWith(".3")) return stub(64, 20, "ping: socket: Operation not permitted");
        return stub(0, 250, {}, "rtt min/avg/max/mdev = 12.4/12.4/12.4/0 ms\n");
    });
    QQmlEngine engine;
    QSignalSpy warnings(&engine, &QQmlEngine::warnings);
    engine.rootContext()->setContextProperty("testMonitor", &monitor);
    QQmlComponent component(&engine);
    component.setData(R"(
        import QtQuick 2.15
        import QtQuick.Controls 2.15
        import QtQuick.Controls.Material 2.15
        import QtQuick.Layouts 1.15
        import "qrc:/gui"
        ApplicationWindow {
            width: 1000; height: 640; visible: true
            Material.theme: Material.Dark
            Material.background: "#2D3035"
            Item { id: stackView }
            header: ToolBar {
                height: 56
                background: Rectangle { color: "#393D43" }
                RowLayout {
                    anchors.fill: parent; anchors.leftMargin: 12; anchors.rightMargin: 12
                    Label { text: "1.4.05-client-live-connectivity"; font.pointSize: 9; color: "#B7BDC5" }
                    Label { text: "Computers"; Layout.fillWidth: true; horizontalAlignment: Text.AlignHCenter }
                    PlankInternetStatus { monitor: testMonitor }
                    ToolButton { text: "+" }
                    ToolButton { text: "Settings" }
                }
            }
        }
    )", QUrl("qrc:/connectivity-test.qml"));
    QScopedPointer<QObject> root(component.create());
    QVERIFY2(root, qPrintable(component.errorString()));
    auto* window = qobject_cast<QQuickWindow*>(root.data());
    QVERIFY(window);
    QVERIFY(QTest::qWaitForWindowExposed(window));
    auto* status = root->findChild<QQuickItem*>("internetStatus");
    auto* label = root->findChild<QQuickItem*>("internetStatusLabel");
    QVERIFY(status && label);
    QCOMPARE(label->property("text").toString(), QString("Internet: Checking"));
    QVERIFY(status->width() > 80);
    QVERIFY(status->height() <= 56);
    QVERIFY(monitor.windowVisible()); // Enabled by the production QML binding.
    QTRY_COMPARE(label->property("text").toString(), QString("Internet: No response"));
    window->resize(720, 640);
    QTest::qWait(50);
    QVERIFY(status->mapToScene(QPointF()).x() >= 0);
    QVERIFY(status->mapToScene(QPointF(status->width(), 0)).x() <= window->width());
    QVERIFY(label->property("contentWidth").toReal() <= label->width() + 1);
    const QString captureDirectory = qEnvironmentVariable("PLANK_CONNECTIVITY_CAPTURE_DIR");
    if (!captureDirectory.isEmpty()) {
        QVERIFY(QDir().mkpath(captureDirectory));
        QVERIFY(window->grabWindow().save(captureDirectory + "/internet-no-response.png"));
    }
    online = true;
    monitor.checkNow();
    QTRY_COMPARE(label->property("text").toString(), QString("Internet: Reachable"));
    if (!captureDirectory.isEmpty())
        QVERIFY(window->grabWindow().save(captureDirectory + "/internet-reachable.png"));

    const auto click = [window](QQuickItem* item) {
        QTest::mouseClick(window, Qt::LeftButton, Qt::NoModifier,
            item->mapToScene(QPointF(item->width() / 2, item->height() / 2)).toPoint());
    };
    auto* dialog = root->findChild<QObject*>("internetSummaryDialog");
    auto* refresh = root->findChild<QQuickItem*>("internetSummaryRefresh");
    auto* close = root->findChild<QQuickItem*>("internetSummaryClose");
    QVERIFY(dialog && !refresh && close);
    click(status);
    QTRY_VERIFY(dialog->property("opened").toBool());
    QTRY_COMPARE(monitor.summaryResults().size(), 3);
    QTRY_VERIFY(!monitor.summaryRunning());
    QQuickItem* firstRtt = nullptr;
    QQuickItem* secondStatus = nullptr;
    QQuickItem* thirdStatus = nullptr;
    QTRY_VERIFY((firstRtt = findVisualItem(window->contentItem(), "internetSummaryRtt0")));
    QTRY_VERIFY((secondStatus = findVisualItem(window->contentItem(), "internetSummaryStatus1")));
    QTRY_VERIFY((thirdStatus = findVisualItem(window->contentItem(), "internetSummaryStatus2")));
    QCOMPARE(firstRtt->property("text").toString(), QString("12 ms"));
    QCOMPARE(secondStatus->property("text").toString(), QString("No response"));
    QCOMPARE(thirdStatus->property("text").toString(), QString("Unavailable"));
    QVERIFY(firstRtt->mapToScene(QPointF(firstRtt->width(), 0)).x() <= window->width());
    // Results are published before Qt Quick polishes the new delegate layout.
    // Wait for geometry, retaining the same bounds. Resolve the current delegate
    // each time because live ping rounds can replace the Repeater's items.
    qreal footerGap = -1;
    QTRY_VERIFY2_WITH_TIMEOUT(([&] {
        auto* lastRow = findVisualItem(window->contentItem(), "internetSummaryStatus2");
        if (!lastRow) return false;
        footerGap = close->mapToScene(QPointF()).y() -
                lastRow->mapToScene(QPointF(0, lastRow->height())).y();
        return footerGap >= 0 && footerGap <= 40;
    })(), qPrintable(QStringLiteral("Footer gap after layout: %1").arg(footerGap)), 1000);
    QVERIFY(!root->findChild<QObject*>("internetSummaryCheckedAt"));
    if (!captureDirectory.isEmpty())
        QVERIFY(window->grabWindow().save(captureDirectory + "/internet-summary.png"));
    online = false;
    QTRY_COMPARE_WITH_TIMEOUT(monitor.state(), Monitor::NoResponse, 3000);
    for (const auto& row : monitor.summaryResults())
        QCOMPARE(row.toMap().value("state").toInt(), int(Monitor::NoResponse));
    click(close);
    QTRY_VERIFY(!dialog->property("visible").toBool());
    QTRY_VERIFY(status->hasActiveFocus());
    QTest::keyClick(window, Qt::Key_Return);
    QTRY_VERIFY(dialog->property("opened").toBool());
    QTest::keyClick(window, Qt::Key_Escape);
    QTRY_VERIFY(!dialog->property("visible").toBool());
    QTRY_VERIFY(monitor.findChildren<QProcess*>().isEmpty());

    // Exercise the production indicator's actual Window binding, not a test
    // copy of the visibility rule. Focus/network notifications cannot revive it.
    window->hide();
    QTRY_VERIFY(!monitor.windowVisible());
    QTRY_VERIFY(monitor.findChildren<QProcess*>().isEmpty());
    const int hiddenAt = attempts;
    monitor.networkChanged();
    monitor.refreshIfStale();
    QTest::qWait(1100);
    QCOMPARE(attempts, hiddenAt);
    window->show();
    QTRY_VERIFY(monitor.windowVisible());
    QTRY_VERIFY(attempts > hiddenAt);
    window->showMinimized();
    QTRY_VERIFY(!monitor.windowVisible());
    QTRY_VERIFY(monitor.findChildren<QProcess*>().isEmpty());
    const int minimizedAt = attempts;
    monitor.networkChanged();
    monitor.refreshIfStale();
    QTest::qWait(1100);
    QCOMPARE(attempts, minimizedAt);
    window->showNormal();
    QTRY_VERIFY(monitor.windowVisible());
    QTRY_VERIFY(attempts > minimizedAt);

    settings.hosts = {"192.0.2.1", "192.0.2.2", "192.0.2.3", "192.0.2.4",
                      "192.0.2.5", "192.0.2.6", "192.0.2.7", "192.0.2.8"};
    Monitor maximum(settings, nullptr, [](const QString&, int) {
        return stub(0, 30, {}, "round-trip min/avg/max/stddev = 0.1/0.1/0.1/0 ms\n");
    });
    status->setProperty("monitor", QVariant::fromValue(static_cast<QObject*>(&maximum)));
    status->forceActiveFocus();
    QTest::keyClick(window, Qt::Key_Space);
    QTRY_VERIFY(dialog->property("opened").toBool());
    QTRY_COMPARE(maximum.summaryResults().size(), 8);
    QTRY_VERIFY(!maximum.summaryRunning());
    QTest::qWait(50);
    QQuickItem* lastRtt = nullptr;
    QTRY_VERIFY((lastRtt = findVisualItem(window->contentItem(), "internetSummaryRtt7")));
    QCOMPARE(lastRtt->property("text").toString(), QString("<1 ms"));
    QVERIFY(dialog->property("height").toReal() <= window->height() - 32);
    QVERIFY(lastRtt->mapToScene(QPointF(0, lastRtt->height())).y() < close->mapToScene(QPointF()).y());
    if (!captureDirectory.isEmpty())
        QVERIFY(window->grabWindow().save(captureDirectory + "/internet-summary-eight.png"));
    click(close);
    QTRY_VERIFY(!dialog->property("visible").toBool());
    settings.enabled = false;
    Monitor disabled(settings);
    status->setProperty("monitor", QVariant::fromValue(static_cast<QObject*>(&disabled)));
    QTRY_VERIFY(!status->isVisible());
    QVERIFY(!maximum.windowVisible());
    QTRY_VERIFY(maximum.findChildren<QProcess*>().isEmpty());
    root.reset();
    QCOMPARE(warnings.size(), 0);
}
int main(int argc, char** argv)
{
    // Deterministic child processes exercise the real QProcess/deadline/reaping
    // path. This mode exists only in this test binary, never the shipped Client.
    if (argc == 6 && QByteArray(argv[1]) == "--ping-stub") {
        QCoreApplication app(argc, argv);
        QFile error;
        if (error.open(stderr, QIODevice::WriteOnly)) {
            error.write(argv[4]);
            error.flush();
        }
        QFile output;
        if (output.open(stdout, QIODevice::WriteOnly)) {
            output.write(argv[5]);
            output.flush();
        }
        const int code = QByteArray(argv[2]).toInt();
        QTimer::singleShot(QByteArray(argv[3]).toInt(), &app, [code] { QCoreApplication::exit(code); });
        return app.exec();
    }
    QGuiApplication app(argc, argv);
    QQuickStyle::setStyle("Material");
    ConnectivityTest test;
    return QTest::qExec(&test, argc, argv);
}
