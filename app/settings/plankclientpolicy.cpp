#include "plankclientpolicy.h"
#include "../clientidentity.h"

#include <QSettings>
#include <QtDebug>

PlankClientPolicy::PlankClientPolicy(const QString& configPath)
    : m_ConfigPath(configPath)
{
}

QString PlankClientPolicy::defaultConfigPath()
{
    return QString::fromLatin1(PlankClientIdentity::configPath);
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

PlankClientPolicy::InternetCheck PlankClientPolicy::internetCheck() const
{
    InternetCheck result;
    managedBoolean(QStringLiteral("network/internet_check_enabled"), &result.enabled);
    if (!result.enabled) return result;

    QSettings policy(m_ConfigPath, QSettings::IniFormat);
    const QString hostsKey = QStringLiteral("network/internet_check_hosts");
    if (policy.contains(hostsKey)) {
        // QSettings parses unquoted comma-separated INI values as QStringList.
        // Also accept a quoted list, without ever treating entries as commands.
        result.hosts = policy.value(hostsKey).toStringList().join(',').split(',');
        for (auto& host : result.hosts) host = host.trimmed();
    }
    bool intervalValid = false, timeoutValid = false;
    const int interval = policy.value(QStringLiteral("network/internet_check_interval_seconds"), 15)
            .toInt(&intervalValid);
    const int timeout = policy.value(QStringLiteral("network/internet_check_timeout_ms"), 1000)
            .toInt(&timeoutValid);
    if (!intervalValid || interval < 5 || interval > 3600 ||
            !timeoutValid || timeout < 100 || timeout > 10000) {
        result.error = QStringLiteral("Internet-check interval must be 5–3600 seconds and timeout 100–10000 ms.");
    }
    else {
        result.intervalMs = interval * 1000;
        result.timeoutMs = timeout;
    }
    if (policy.status() != QSettings::NoError) {
        result.error = QStringLiteral("Internet-check configuration could not be read.");
    }
    return result;
}

PlankClientPolicy::Metrics PlankClientPolicy::metrics() const
{
    Metrics result;
    managedBoolean(QStringLiteral("metrics/metrics_enabled"), &result.enabled);
    if (!result.enabled) return result;
    QSettings policy(m_ConfigPath, QSettings::IniFormat);
    bool portValid = false, intervalValid = false;
    const auto port = policy.value(QStringLiteral("metrics/metrics_port"), 28991).toUInt(&portValid);
    const auto interval = policy.value(QStringLiteral("metrics/metrics_interval_seconds"), 1).toUInt(&intervalValid);
    if (policy.status() != QSettings::NoError || !portValid || port < 1024 || port > 65535 ||
            !intervalValid || interval < 1 || interval > 60) {
        result.enabled = false;
        result.error = QStringLiteral("Metrics disabled: port must be 1024–65535 and interval 1–60 seconds.");
        return result;
    }
    result.port = quint16(port);
    result.intervalSeconds = interval;
    return result;
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
