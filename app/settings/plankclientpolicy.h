#pragma once

#include <QString>
#include <QtGlobal>

class PlankClientPolicy
{
public:
    explicit PlankClientPolicy(
            const QString& configPath = defaultConfigPath());

    static QString defaultConfigPath();

    bool managedBoolean(const QString& key, bool* value) const;
    bool rememberUsername() const;
    quint16 networkPort() const;
    bool relayWakeEnabled() const;
    quint16 relayWakePort() const;

    static constexpr quint16 BuiltInNetworkPort = 28989;
    static constexpr quint16 BuiltInRelayWakePort = 28988;

private:
    quint16 configuredPort(const QString& key, quint16 fallback,
                           const QString& description) const;

    QString m_ConfigPath;
};
