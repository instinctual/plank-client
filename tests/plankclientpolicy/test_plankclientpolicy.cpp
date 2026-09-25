#include <QtTest>
#include <QSettings>
#include <QTemporaryDir>

#include "plankclientpolicy.h"

class TestPlankClientPolicy : public QObject
{
    Q_OBJECT

private slots:
    void omittedValueIsNotManaged();
    void explicitFalseIsManaged();
    void explicitTrueIsManaged();
    void invalidValueFailsClosed();
    void omittedPortUsesBuiltInDefault();
    void configuredPortIsReturned();
    void invalidPortUsesBuiltInDefault();
    void configuredRelayWakePortIsReturned();
    void invalidRelayWakePortUsesBuiltInDefault();
    void relayWakeDefaultsOff();
    void relayWakeOptIn_data();
    void relayWakeOptIn();
    void rememberUsernameDefaultsOff();
    void rememberUsernameOptIn_data();
    void rememberUsernameOptIn();
};

void TestPlankClientPolicy::omittedValueIsNotManaged()
{
    QTemporaryDir directory;
    QVERIFY(directory.isValid());

    bool value = true;
    const PlankClientPolicy policy(directory.filePath(QStringLiteral("client.conf")));
    QVERIFY(!policy.managedBoolean(QStringLiteral("network/mdns_discovery"), &value));
    QVERIFY(value);
}

void TestPlankClientPolicy::explicitFalseIsManaged()
{
    QTemporaryDir directory;
    QVERIFY(directory.isValid());
    const QString path = directory.filePath(QStringLiteral("client.conf"));
    {
        QSettings settings(path, QSettings::IniFormat);
        settings.setValue(QStringLiteral("network/mdns_discovery"), false);
    }

    bool value = true;
    const PlankClientPolicy policy(path);
    QVERIFY(policy.managedBoolean(QStringLiteral("network/mdns_discovery"), &value));
    QVERIFY(!value);
}

void TestPlankClientPolicy::explicitTrueIsManaged()
{
    QTemporaryDir directory;
    QVERIFY(directory.isValid());
    const QString path = directory.filePath(QStringLiteral("client.conf"));
    {
        QSettings settings(path, QSettings::IniFormat);
        settings.setValue(QStringLiteral("network/mdns_discovery"), true);
    }

    bool value = false;
    const PlankClientPolicy policy(path);
    QVERIFY(policy.managedBoolean(QStringLiteral("network/mdns_discovery"), &value));
    QVERIFY(value);
}

void TestPlankClientPolicy::invalidValueFailsClosed()
{
    QTemporaryDir directory;
    QVERIFY(directory.isValid());
    const QString path = directory.filePath(QStringLiteral("client.conf"));
    {
        QSettings settings(path, QSettings::IniFormat);
        settings.setValue(QStringLiteral("network/mdns_discovery"),
                          QStringLiteral("sometimes"));
    }

    bool value = true;
    const PlankClientPolicy policy(path);
    QVERIFY(policy.managedBoolean(QStringLiteral("network/mdns_discovery"), &value));
    QVERIFY(!value);
}

void TestPlankClientPolicy::omittedPortUsesBuiltInDefault()
{
    QTemporaryDir directory;
    QVERIFY(directory.isValid());

    const PlankClientPolicy policy(
                directory.filePath(QStringLiteral("client.conf")));
    QCOMPARE(policy.networkPort(), PlankClientPolicy::BuiltInNetworkPort);
}

void TestPlankClientPolicy::configuredPortIsReturned()
{
    QTemporaryDir directory;
    QVERIFY(directory.isValid());
    const QString path = directory.filePath(QStringLiteral("client.conf"));
    {
        QSettings settings(path, QSettings::IniFormat);
        settings.setValue(QStringLiteral("network/port"), 31000);
    }

    const PlankClientPolicy policy(path);
    QCOMPARE(policy.networkPort(), quint16(31000));
}

void TestPlankClientPolicy::invalidPortUsesBuiltInDefault()
{
    QTemporaryDir directory;
    QVERIFY(directory.isValid());
    const QString path = directory.filePath(QStringLiteral("client.conf"));
    {
        QSettings settings(path, QSettings::IniFormat);
        settings.setValue(QStringLiteral("network/port"), 70000);
    }

    const PlankClientPolicy policy(path);
    QCOMPARE(policy.networkPort(), PlankClientPolicy::BuiltInNetworkPort);
}

void TestPlankClientPolicy::configuredRelayWakePortIsReturned()
{
    QTemporaryDir directory;
    QVERIFY(directory.isValid());
    const QString path = directory.filePath(QStringLiteral("client.conf"));
    {
        QSettings settings(path, QSettings::IniFormat);
        settings.setValue(QStringLiteral("network/relay_wake_port"), 30123);
    }

    const PlankClientPolicy policy(path);
    QCOMPARE(policy.relayWakePort(), quint16(30123));
}

void TestPlankClientPolicy::invalidRelayWakePortUsesBuiltInDefault()
{
    QTemporaryDir directory;
    QVERIFY(directory.isValid());
    const QString path = directory.filePath(QStringLiteral("client.conf"));
    {
        QSettings settings(path, QSettings::IniFormat);
        settings.setValue(QStringLiteral("network/relay_wake_port"), 22);
    }

    const PlankClientPolicy policy(path);
    QCOMPARE(policy.relayWakePort(), PlankClientPolicy::BuiltInRelayWakePort);
}

void TestPlankClientPolicy::relayWakeDefaultsOff()
{
    QTemporaryDir directory;
    QVERIFY(directory.isValid());
    const QString path = directory.filePath(QStringLiteral("client.conf"));
    const PlankClientPolicy policy(path);
    QVERIFY(!policy.relayWakeEnabled());
    QSettings settings(path, QSettings::IniFormat);
    settings.setValue(QStringLiteral("network/relay_wake_port"), 30123);
    settings.sync();
    QVERIFY(!policy.relayWakeEnabled());
}

void TestPlankClientPolicy::relayWakeOptIn_data()
{
    QTest::addColumn<QString>("configured");
    QTest::addColumn<bool>("enabled");
    QTest::newRow("true") << QStringLiteral("true") << true;
    QTest::newRow("false") << QStringLiteral("false") << false;
    QTest::newRow("empty") << QString() << false;
    QTest::newRow("invalid") << QStringLiteral("sometimes") << false;
    QTest::newRow("numeric") << QStringLiteral("1") << false;
    QTest::newRow("normalized") << QStringLiteral(" TRUE ") << true;
}

void TestPlankClientPolicy::relayWakeOptIn()
{
    QFETCH(QString, configured);
    QFETCH(bool, enabled);
    QTemporaryDir directory;
    QVERIFY(directory.isValid());
    const QString path = directory.filePath(QStringLiteral("client.conf"));
    const PlankClientPolicy policy(path);
    QSettings settings(path, QSettings::IniFormat);
    settings.setValue(QStringLiteral("network/relay_wake_enabled"), configured);
    settings.sync();
    QCOMPARE(policy.relayWakeEnabled(), enabled);
    settings.remove(QStringLiteral("network/relay_wake_enabled"));
    settings.sync();
    QVERIFY(!policy.relayWakeEnabled());
}

void TestPlankClientPolicy::rememberUsernameDefaultsOff()
{
    QTemporaryDir directory;
    QVERIFY(directory.isValid());
    const PlankClientPolicy policy(directory.filePath(QStringLiteral("missing.conf")));
    QVERIFY(!policy.rememberUsername());
}

void TestPlankClientPolicy::rememberUsernameOptIn_data()
{
    relayWakeOptIn_data();
}

void TestPlankClientPolicy::rememberUsernameOptIn()
{
    QFETCH(QString, configured);
    QFETCH(bool, enabled);
    QTemporaryDir directory;
    QVERIFY(directory.isValid());
    const QString path = directory.filePath(QStringLiteral("client.conf"));
    QSettings settings(path, QSettings::IniFormat);
    settings.setValue(QStringLiteral("authentication/remember_username"), configured);
    settings.sync();
    const PlankClientPolicy policy(path);
    QCOMPARE(policy.rememberUsername(), enabled);
    settings.remove(QStringLiteral("authentication/remember_username"));
    settings.sync();
    QVERIFY(!policy.rememberUsername());
}

QTEST_APPLESS_MAIN(TestPlankClientPolicy)
#include "test_plankclientpolicy.moc"
