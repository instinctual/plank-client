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
    void preservesExistingState()
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
        QCOMPARE(current.fileName(), production.fileName());
        QCOMPARE(current.value("bookmark/fixture").toString(), QString("production"));
        QCOMPARE(QStandardPaths::writableLocation(QStandardPaths::AppLocalDataLocation), productionData);
        QCOMPARE(QStandardPaths::writableLocation(QStandardPaths::CacheLocation), productionCache);
        QCOMPARE(currentTrust, productionTrust);
        HostTrustStore retained(currentTrust);
        QCOMPARE(retained.check(endpoint, productionKey, false).status, HostTrustStore::Status::Trusted);
        QCOMPARE(Path::getLogDir(), productionLogs);
        QCOMPARE(Path::getLogDir(), storage.path() + "/state/plank/logs");
        QCOMPARE(PlankClientPolicy::defaultConfigPath(), QString("/etc/plank/client.conf"));
        QCOMPARE(QString::fromLatin1(PlankClientIdentity::desktopId), QString("la.instinctual.Plank.Client"));
        QCOMPARE(QString::fromLatin1(PlankClientIdentity::executable), QString("plank-client"));
    }
};
QTEST_GUILESS_MAIN(ClientIdentityTest)
#include "test_clientidentity.moc"
