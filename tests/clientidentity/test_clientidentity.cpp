#include "clientidentity.h"
#include "path.h"
#include "settings/plankclientpolicy.h"
#include "backend/hosttruststore.h"

#include <QtTest>
#include <QSettings>
#include <QStandardPaths>
#include <QTemporaryDir>

class ClientIdentityTest : public QObject
{
    Q_OBJECT
private slots:
    void independentState()
    {
        QTemporaryDir storage;
        QVERIFY(storage.isValid());
        qputenv("XDG_CONFIG_HOME", (storage.path() + "/config").toUtf8());
        qputenv("XDG_DATA_HOME", (storage.path() + "/data").toUtf8());
        qputenv("XDG_CACHE_HOME", (storage.path() + "/cache").toUtf8());
        qputenv("XDG_STATE_HOME", (storage.path() + "/state").toUtf8());
        QCoreApplication::setOrganizationName("Instinctual");
        QCoreApplication::setApplicationName("PLANK");
        QSettings production;
        production.setValue("bookmark/fixture", "production");
        production.sync();
        const QString productionData = QStandardPaths::writableLocation(QStandardPaths::AppLocalDataLocation);
        const QString productionCache = QStandardPaths::writableLocation(QStandardPaths::CacheLocation);
        const QString productionTrust = HostTrustStore::defaultDirectory();
        const auto endpoint = HostTrustStore::endpoint(QUrl("https://host.example.invalid:28989"));
        HostTrustStore original(productionTrust);
        const QByteArray productionKey(32, 'a');
        QCOMPARE(original.check(endpoint, productionKey, true).status, HostTrustStore::Status::Trusted);
        Path::initialize(false);
        const QString productionLogs = Path::getLogDir();

        QCoreApplication::setApplicationName(PlankClientIdentity::applicationName);
        QSettings current;
        Path::initialize(false);
        const QString currentTrust = HostTrustStore::defaultDirectory();
        if (PlankClientIdentity::experimental) {
            QVERIFY(current.fileName() != production.fileName());
            QVERIFY(!current.contains("bookmark/fixture"));
            QVERIFY(QStandardPaths::writableLocation(QStandardPaths::AppLocalDataLocation) != productionData);
            QVERIFY(QStandardPaths::writableLocation(QStandardPaths::CacheLocation) != productionCache);
            QVERIFY(currentTrust != productionTrust);
            QCOMPARE(Path::getLogDir(), storage.path() + "/state/plank-client-qt/logs");
            QCOMPARE(PlankClientPolicy::defaultConfigPath(), QString("/etc/plank/client-qt.conf"));
            HostTrustStore separate(currentTrust);
            QCOMPARE(separate.check(endpoint, productionKey, false).status, HostTrustStore::Status::Unknown);
            QCOMPARE(separate.check(endpoint, QByteArray(32, 'b'), true).status, HostTrustStore::Status::Trusted);
            current.setValue("bookmark/fixture", "experiment");
            current.sync();
            QCOMPARE(production.value("bookmark/fixture").toString(), QString("production"));
            QCOMPARE(original.check(endpoint, productionKey, false).status, HostTrustStore::Status::Trusted);
        } else {
            QCOMPARE(current.fileName(), production.fileName());
            QCOMPARE(currentTrust, productionTrust);
            QCOMPARE(Path::getLogDir(), productionLogs);
            QCOMPARE(Path::getLogDir(), storage.path() + "/state/plank/logs");
            QCOMPARE(PlankClientPolicy::defaultConfigPath(), QString("/etc/plank/client.conf"));
        }
    }
};
QTEST_GUILESS_MAIN(ClientIdentityTest)
#include "test_clientidentity.moc"
