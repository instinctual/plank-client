#include "plankclientpolicy.h"

#include <QSettings>
#include <QtDebug>

PlankClientPolicy::PlankClientPolicy(const QString& configPath)
    : m_ConfigPath(configPath)
{
}

QString PlankClientPolicy::defaultConfigPath()
{
    return QStringLiteral("/etc/plank/client.conf");
}

bool PlankClientPolicy::managedBoolean(const QString& key, bool* value) const
{
    Q_ASSERT(value != nullptr);

    QSettings policy(m_ConfigPath, QSettings::IniFormat);
    if (!policy.contains(key)) {
        return false;
    }

    const QString configuredValue = policy.value(key).toString().trimmed().toLower();
    if (configuredValue == QStringLiteral("true")) {
        *value = true;
    }
    else if (configuredValue == QStringLiteral("false")) {
        *value = false;
    }
    else {
        qWarning() << "Invalid managed boolean" << configuredValue
                   << "for" << key << "in" << m_ConfigPath
                   << "- forcing false";
        *value = false;
    }

    return true;
}

quint16 PlankClientPolicy::networkPort() const
{
    return configuredPort(QStringLiteral("network/port"), BuiltInNetworkPort,
                          QStringLiteral("PLANK network"));
}

bool PlankClientPolicy::rememberUsername() const
{
    bool enabled = false;
    return managedBoolean(QStringLiteral("authentication/remember_username"), &enabled) && enabled;
}

bool PlankClientPolicy::relayWakeEnabled() const
{
    bool enabled = false;
    return managedBoolean(QStringLiteral("network/relay_wake_enabled"), &enabled) && enabled;
}

quint16 PlankClientPolicy::relayWakePort() const
{
    return configuredPort(QStringLiteral("network/relay_wake_port"),
                          BuiltInRelayWakePort,
                          QStringLiteral("PLANK Relay wake"));
}

quint16 PlankClientPolicy::configuredPort(const QString& key, quint16 fallback,
                                          const QString& description) const
{
    QSettings policy(m_ConfigPath, QSettings::IniFormat);
    bool valid = false;
    const int configuredPort = policy.value(key, fallback).toInt(&valid);
    if (!valid || configuredPort < 1024 || configuredPort > 65535) {
        qWarning() << "Invalid" << description << "port" << configuredPort
                   << "in" << m_ConfigPath << "- using"
                   << fallback;
        return fallback;
    }

    return static_cast<quint16>(configuredPort);
}
