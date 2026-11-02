#include "backend/computermanager.h"
#include "backend/hosttruststore.h"

#include <QSignalSpy>
#include <QTemporaryDir>
#include <QThreadPool>
#include <QtTest>

namespace {
using Preferences = StreamingPreferences;

NvComputer* named(ComputerManager& manager, const QString& name)
{
    for (auto* computer : manager.getComputers()) {
        if (computer->name == name) return computer;
    }
    return nullptr;
}

QVariantList bitrates(int rate)
{
    return Preferences::plankProfileBitratesToVariantList(
                QVector<int>(Preferences::PLANK_PROFILE_COUNT, rate));
}

bool edit(ComputerManager& manager, NvComputer* computer, const QString& address,
          const QString& name)
{
    return manager.editManualBookmark(computer, address, name,
                NvOutputTopology::ScaledSpanMode, NvOutputTopology::DualHorizontalHostLayout,
                QStringLiteral("2560x2160"), QStringLiteral("1280x2160"),
                Preferences::PLANK_PROFILE_NVENC_HEVC_10BIT_444,
                Preferences::PLANK_CAPTURE_NVFBC_8BIT, bitrates(45000), QStringLiteral("span"));
}
}

class BookmarkTest : public QObject
{
    Q_OBJECT
private slots:
    void initTestCase()
    {
        QVERIFY(m_Settings.isValid());
        QCoreApplication::setOrganizationName(QStringLiteral("PLANK-tests"));
        QCoreApplication::setApplicationName(QStringLiteral("bookmarks"));
        QSettings::setDefaultFormat(QSettings::IniFormat);
        QSettings::setPath(QSettings::IniFormat, QSettings::UserScope, m_Settings.path());
        QSettings::setPath(QSettings::IniFormat, QSettings::SystemScope, m_Settings.path());
    }

    void init() { QSettings().clear(); }

    void independentConfigurationsSurviveRestart()
    {
        QString firstId, secondId;
        {
            ComputerManager manager(nullptr); // No polling or network requests.
            QSignalSpy added(&manager, &ComputerManager::computerAddCompleted);
            manager.addNewHostManually(QStringLiteral("host.example.test"), QStringLiteral("Office"),
                2, 9, 1, 0, Preferences::PLANK_PROFILE_H264_8BIT_444,
                Preferences::PLANK_CAPTURE_NVFBC_8BIT, bitrates(85000));
            manager.addNewHostManually(QStringLiteral("host.example.test"), QStringLiteral("Home"),
                3, 6, 1, 1, Preferences::PLANK_PROFILE_NVENC_HEVC_10BIT_444,
                Preferences::PLANK_CAPTURE_NVFBC_8BIT, bitrates(45000), QStringLiteral("span"));
            QCOMPARE(added.size(), 2);
            QVERIFY(added.at(0).at(0).toBool() && added.at(1).at(0).toBool());
            QCOMPARE(manager.getComputers().size(), 2);
            auto* first = named(manager, QStringLiteral("Office"));
            auto* second = named(manager, QStringLiteral("Home"));
            QVERIFY(first && second);
            firstId = first->uuid;
            secondId = second->uuid;
            QVERIFY(firstId != secondId);
            QCOMPARE(first->manualAddress.toString(), second->manualAddress.toString());

            // Online metadata updates identify the same Host without replacing
            // either bookmark's configuration, name or local identity.
            NvHTTP http(first->manualAddress);
            NvComputer advertised(http, QStringLiteral(
                "<root status_code=\"200\"><hostname>Advertised Host</hostname>"
                "<uniqueid>test-host-identity</uniqueid><HttpsPort>28989</HttpsPort>"
                "<PlankAuth>1</PlankAuth><PlankHostMetadataVersion>1</PlankHostMetadataVersion>"
                "<PlankHostVersion>test</PlankHostVersion></root>"));
            first->update(advertised);
            second->update(advertised);
            manager.clientSideAttributeUpdated(first);
            manager.clientSideAttributeUpdated(second);
        }
        {
            ComputerManager manager(nullptr);
            QCOMPARE(manager.getComputers().size(), 2);
            auto* first = named(manager, QStringLiteral("Office"));
            auto* second = named(manager, QStringLiteral("Home"));
            QVERIFY(first && second);
            QCOMPARE(first->uuid, firstId);
            QCOMPARE(second->uuid, secondId);
            QCOMPARE(first->serverUuid, second->serverUuid);
            QCOMPARE(first->serverUuid, QStringLiteral("test-host-identity"));
            QCOMPARE(first->plankHostLayout, QStringLiteral("single"));
            QCOMPARE(second->plankHostLayout, QStringLiteral("dual-horizontal"));
            QCOMPARE(first->plankVirtualMode1, QStringLiteral("3840x2160"));
            QCOMPARE(second->plankVirtualMode1, QStringLiteral("2560x2160"));
            QCOMPARE(first->plankScalingMode, QStringLiteral("native"));
            QCOMPARE(second->plankScalingMode, QStringLiteral("scaled-span"));
            QCOMPARE(first->plankClientDisplays, QStringLiteral("current"));
            QCOMPARE(second->plankClientDisplays, QStringLiteral("span"));
            QCOMPARE(first->plankVideoProfile, int(Preferences::PLANK_PROFILE_H264_8BIT_444));
            QCOMPARE(second->plankVideoProfile, int(Preferences::PLANK_PROFILE_NVENC_HEVC_10BIT_444));
            QCOMPARE(first->plankProfileBitratesKbps.at(0), 85000);
            QCOMPARE(second->plankProfileBitratesKbps.at(0), 45000);
            QVERIFY(edit(manager, first, QStringLiteral("host.example.test"), QStringLiteral("Edited")));
            QCOMPARE(first->uuid, firstId);
            QCOMPARE(second->name, QStringLiteral("Home"));
        }
        ComputerManager reloaded(nullptr);
        QCOMPARE(reloaded.getComputers().size(), 2);
        QVERIFY(named(reloaded, QStringLiteral("Edited")));
        QCOMPARE(named(reloaded, QStringLiteral("Edited"))->uuid, firstId);
        QVERIFY(named(reloaded, QStringLiteral("Home")));
    }

    void addressEditRetainsExistingIdAndClearsAuthentication()
    {
        const QString savedId = QStringLiteral("plank-bookmark-0123456789abcdef0123456789abcdef");
        {
            QSettings settings;
            settings.beginWriteArray("hosts");
            settings.setArrayIndex(0);
            NvComputer saved(NvAddress(QStringLiteral("old.example.test"), 28989), QStringLiteral("Saved"),
                Preferences::PLANK_PROFILE_NVENC_HEVC_10BIT_444,
                Preferences::PLANK_CAPTURE_NVFBC_8BIT, Preferences::plankDefaultProfileBitrates());
            saved.serialize(settings, false);
            // Preserve an ID created by earlier versions; no migration needed.
            settings.setValue("uuid", savedId);
            settings.endArray();
        }
        {
            ComputerManager manager(nullptr);
            manager.addNewHostManually(QStringLiteral("new.example.test"), QStringLiteral("Other"));
            auto* saved = named(manager, QStringLiteral("Saved"));
            auto* other = named(manager, QStringLiteral("Other"));
            QVERIFY(saved && other);
            {
                QWriteLocker lock(&saved->lock);
                saved->sessionToken = QStringLiteral("test-in-memory-token");
                saved->sessionIdentityKey = QByteArray(32, 'x');
                saved->serverUuid = QStringLiteral("previous-host");
            }
            QVERIFY(edit(manager, saved, QStringLiteral("new.example.test"), QStringLiteral("Moved")));
            QCOMPARE(saved->uuid, savedId);
            QCOMPARE(saved->manualAddress.toString(), other->manualAddress.toString());
            QVERIFY(saved->sessionToken.isEmpty() && saved->sessionIdentityKey.isEmpty());
            QVERIFY(saved->serverUuid.isEmpty());
            QCOMPARE(other->plankHostLayout, QStringLiteral("match-client"));
        }
        ComputerManager reloaded(nullptr);
        QCOMPARE(reloaded.getComputers().size(), 2);
        QVERIFY(named(reloaded, QStringLiteral("Moved")));
        QCOMPARE(named(reloaded, QStringLiteral("Moved"))->uuid, savedId);
    }

    void deleteOnlySelectedBookmarkAndPreserveTrust()
    {
        HostTrustStore trust(m_Settings.filePath(QStringLiteral("trust")));
        const auto endpoint = HostTrustStore::endpoint(QUrl(QStringLiteral("https://host.example.test:28989")));
        const QByteArray key(32, 'a');
        QCOMPARE(trust.check(endpoint, key, true).status, HostTrustStore::Status::Trusted);
        QString retainedId;
        {
            ComputerManager manager(nullptr);
            manager.addNewHostManually(QStringLiteral("host.example.test"), QStringLiteral("Remove"));
            manager.addNewHostManually(QStringLiteral("host.example.test"), QStringLiteral("Keep"));
            QCOMPARE(manager.getComputers().size(), 2);
            retainedId = named(manager, QStringLiteral("Keep"))->uuid;
            manager.deleteHost(named(manager, QStringLiteral("Remove")));
            QVERIFY(QThreadPool::globalInstance()->waitForDone(5000));
            QCOMPARE(manager.getComputers().size(), 1);
        }
        ComputerManager reloaded(nullptr);
        QCOMPARE(reloaded.getComputers().size(), 1);
        QCOMPARE(reloaded.getComputers().first()->uuid, retainedId);
        QCOMPARE(trust.check(endpoint, key).status, HostTrustStore::Status::Trusted);
        QCOMPARE(trust.check(endpoint, QByteArray(32, 'b')).status, HostTrustStore::Status::Changed);
    }

    void addressFormsAndMacProfiles_data()
    {
        QTest::addColumn<QString>("address");
        QTest::newRow("hostname-port") << QStringLiteral("host.example.test:29989");
        QTest::newRow("ipv4") << QStringLiteral("192.0.2.10");
        QTest::newRow("ipv6") << QStringLiteral("2001:db8::10");
        QTest::newRow("ipv6-port") << QStringLiteral("[2001:db8::10]:29989");
    }

    void addressFormsAndMacProfiles()
    {
        QFETCH(QString, address);
        ComputerManager manager(nullptr);
        for (int i = 0; i < 2; ++i) {
            manager.addNewHostManually(address, QStringLiteral("Mac %1").arg(i), 2, 9, 1, 0,
                Preferences::PLANK_PROFILE_APPLE_HEVC_10BIT_444,
                Preferences::PLANK_CAPTURE_SCREENCAPTUREKIT, bitrates(55000));
        }
        QCOMPARE(manager.getComputers().size(), 2);
        for (auto* computer : manager.getComputers()) {
            QCOMPARE(computer->plankHostLayout, QStringLiteral("fixed"));
            QCOMPARE(computer->plankVideoProfile, int(Preferences::PLANK_PROFILE_APPLE_HEVC_10BIT_444));
            QCOMPARE(computer->plankProfileBitratesKbps.at(0), 55000);
        }
        // Invalid settings must not bypass validation for any address family.
        manager.addNewHostManually(address, QStringLiteral("Invalid"), 2, 9, 1, 0, -1);
        QCOMPARE(manager.getComputers().size(), 2);
    }

private:
    QTemporaryDir m_Settings;
};

QTEST_GUILESS_MAIN(BookmarkTest)
#include "test_bookmarks.moc"
