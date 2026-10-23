#pragma once

#include <QByteArray>
#include <QString>
#include <QUuid>

namespace PlankHostRecovery {
constexpr quint64 SilenceMs = 1000;
constexpr quint64 ProbeIntervalMs = 1000;

inline QString canonicalInstance(const QString& value)
{
    const QUuid id(value);
    if (id.isNull() || value.size() != 36) return {};
    const QString canonical = id.toString(QUuid::WithoutBraces);
    return value.compare(canonical, Qt::CaseInsensitive) == 0 ? canonical : QString();
}

inline bool videoSilent(quint64 now, quint64 lastVideo)
{
    // Schedules a credential-free worker-replacement probe, not an outage
    // warning. Capture may legitimately idle without producing video frames.
    return lastVideo != 0 && now >= lastVideo && now - lastVideo >= SilenceMs;
}

inline bool replacementConfirmed(const QString& before, const QString& after,
                                 const QByteArray& expectedCertificate,
                                 const QByteArray& actualCertificate)
{
    const QString oldId = canonicalInstance(before);
    const QString newId = canonicalInstance(after);
    return !oldId.isEmpty() && !newId.isEmpty() && oldId != newId &&
            expectedCertificate.size() == 32 && expectedCertificate == actualCertificate;
}
}
