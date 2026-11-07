#pragma once

#include <QString>
#include <QStringList>
#include <QtGlobal>

class PlankClientPolicy
{
public:
    struct Metrics {
        bool enabled = false;
        quint16 port = 28991;
        unsigned intervalSeconds = 1;
        QString error;
    };
    struct InternetCheck {
        bool enabled = true;
        QStringList hosts = {QStringLiteral("8.8.8.8"), QStringLiteral("1.1.1.1"),
                             QStringLiteral("9.9.9.9")};
        int intervalMs = 15000;
        int timeoutMs = 1000;
        QString error;
    };

    explicit PlankClientPolicy(
            const QString& configPath = defaultConfigPath());

    static QString defaultConfigPath();

    bool managedBoolean(const QString& key, bool* value) const;
    bool rememberUsername() const;
    quint16 networkPort() const;
    bool relayWakeEnabled() const;
    quint16 relayWakePort() const;
    InternetCheck internetCheck() const;
    Metrics metrics() const;

    static constexpr quint16 BuiltInNetworkPort = 28989;
    static constexpr quint16 BuiltInRelayWakePort = 28988;

private:
    quint16 configuredPort(const QString& key, quint16 fallback,
                           const QString& description) const;

    QString m_ConfigPath;
};
