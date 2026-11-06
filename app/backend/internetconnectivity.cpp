#include "internetconnectivity.h"

#include <QDateTime>
#include <QHostAddress>
#include <QProcessEnvironment>
#include <QRegularExpression>
#include <QtDebug>

// One bounded asynchronous ping, shared by the ordered background check and
// the live concurrent summary. Cancellation reaps the child before reuse.
struct InternetConnectivity::Probe : QObject
{
    using Completion = std::function<void(Probe*, State, double)>;

    Probe(QObject* parent, PingCommand command, int timeoutMs, Completion completion)
        : QObject(parent), command(std::move(command)), completion(std::move(completion)),
          process(this)
    {
        deadline.setSingleShot(true);
        deadline.setTimerType(Qt::PreciseTimer);
        deadline.setInterval(timeoutMs);
        auto environment = QProcessEnvironment::systemEnvironment();
        environment.insert(QStringLiteral("LC_ALL"), QStringLiteral("C"));
        process.setProcessEnvironment(environment);
        process.setStandardInputFile(QProcess::nullDevice());
        connect(&process, &QProcess::readyReadStandardOutput, this, [this] { drain(); });
        connect(&process, &QProcess::readyReadStandardError, this, [this] { drain(); });
        connect(&deadline, &QTimer::timeout, this, [this] {
            timedOut = true;
            process.kill();
        });
        connect(&process, &QProcess::started, this, [this] {
            if (cancelled || timedOut) process.kill();
        });
        connect(&process, &QProcess::finished, this,
                [this](int code, QProcess::ExitStatus status) { finish(code, status); });
        connect(&process, &QProcess::errorOccurred, this, [this](QProcess::ProcessError error) {
            if (error == QProcess::FailedToStart) finish(-1, QProcess::CrashExit);
            // Killed/crashed children also emit finished; consume them once.
        });
    }

    ~Probe() override
    {
        // The parent monitor may already be in QObject destruction. Do not call
        // its captured completion while QProcess reaps a child during shutdown.
        process.disconnect(this);
        process.kill();
    }

    void start()
    {
        if (cancelled) { finish(-1, QProcess::CrashExit); return; }
        deadline.start();
        process.start(command.program, command.arguments);
    }

    void cancel()
    {
        cancelled = true;
        deadline.stop();
        process.kill();
    }

    void drain()
    {
        if (!process.isOpen()) return;
        output.append(process.readAllStandardOutput().left(4096 - output.size()));
        error.append(process.readAllStandardError().left(1024 - error.size()));
    }

    double roundTripMs() const
    {
        // -q -c 1 emits this summary on both iputils and macOS ping. Parse the
        // actual ICMP RTT, never process startup/exit time. Success without a
        // recognized timing line remains reachable with no RTT available.
        // Read only the average. macOS can print NaN for the unused standard
        // deviation when its single-sample variance rounds below zero.
        static const QRegularExpression summary(QStringLiteral(
            R"((?:^|\n)(?:rtt|round-trip) min/avg/max/(?:mdev|stddev) = [0-9]+(?:\.[0-9]+)?/([0-9]+(?:\.[0-9]+)?)/)"));
        const auto match = summary.match(QString::fromLatin1(output));
        if (!match.hasMatch()) return -1;
        bool ok = false;
        const double value = match.captured(1).toDouble(&ok);
        return ok && qIsFinite(value) && value >= 0 ? value : -1;
    }

    void finish(int exitCode, QProcess::ExitStatus exitStatus)
    {
        if (finished) return;
        finished = true;
        deadline.stop();
        drain();
        State state = NoResponse;
        if (!cancelled && !timedOut) {
            const bool routeUnavailable = error.contains("Network is unreachable") ||
                    error.contains("No route to host") || error.contains("Network is down") ||
                    error.contains("Host is down") || error.contains("Host is unreachable");
            if (exitStatus == QProcess::NormalExit && exitCode == 0) state = Reachable;
            else if (exitStatus != QProcess::NormalExit ||
                     (exitCode != command.noResponseExitCode && !routeUnavailable)) state = Unavailable;
        }
        const double rtt = state == Reachable ? roundTripMs() : -1;
        const auto notify = completion;
        deleteLater();
        notify(this, state, rtt);
    }

    const PingCommand command;
    const Completion completion;
    QProcess process;
    QTimer deadline;
    QByteArray output;
    QByteArray error;
    bool cancelled = false;
    bool timedOut = false;
    bool finished = false;
};

InternetConnectivity::InternetConnectivity(const PlankClientPolicy::InternetCheck& settings,
                                           QObject* parent, CommandFactory commandFactory)
    : QObject(parent), m_Settings(settings), m_CommandFactory(std::move(commandFactory)),
      m_ConfigurationError(settings.error)
{
    // Numeric unicast addresses avoid DNS timeouts and command/option injection.
    // Do not fall back to public destinations if an explicit list is invalid.
    if (settings.hosts.isEmpty() || settings.hosts.size() > 8) {
        m_ConfigurationError = tr("Specify between 1 and 8 Internet-check IP addresses.");
    }
    for (const auto& host : settings.hosts) {
        const QHostAddress address(host);
        if (address.protocol() != QAbstractSocket::IPv4Protocol || address.isMulticast() ||
                address == QHostAddress::Broadcast || address == QHostAddress::AnyIPv4) {
            m_ConfigurationError = tr("Internet-check targets must be unicast IPv4 addresses.");
        }
    }
    if (!settings.enabled) {
        m_State = Disabled;
        m_SummaryError = tr("Internet checks are disabled in client.conf.");
    }
    else if (!m_ConfigurationError.isEmpty()) {
        m_State = Unavailable;
        m_Detail = m_ConfigurationError;
        m_SummaryError = m_ConfigurationError;
    }
    else m_Detail = tr("Checking the configured Internet addresses. PLANK connections remain available.");

    m_Poll.setSingleShot(true);
    m_NetworkChange.setSingleShot(true);
    connect(&m_Poll, &QTimer::timeout, this, &InternetConnectivity::checkNow);
    connect(&m_NetworkChange, &QTimer::timeout, this, &InternetConnectivity::checkNow);
}

InternetConnectivity::~InternetConnectivity()
{
    stop();
}

QString InternetConnectivity::statusText() const
{
    switch (m_State) {
    case Disabled: return tr("Internet: Disabled");
    case Checking: return tr("Internet: Checking");
    case Reachable: return tr("Internet: Reachable");
    case NoResponse: return tr("Internet: No response");
    case Unavailable: return tr("Internet: Unavailable");
    }
    return {};
}

InternetConnectivity::PingCommand InternetConnectivity::systemPingCommand(const QString& host, int timeoutMs)
{
    const QHostAddress address(host);
    const QString deadline = QString::number((timeoutMs + 999) / 1000);
#ifdef Q_OS_MACOS
    return {QStringLiteral("/sbin/ping"),
            {QStringLiteral("-n"), QStringLiteral("-q"), QStringLiteral("-c"), QStringLiteral("1"),
             QStringLiteral("-t"), deadline, address.toString()}, 2};
#elif defined(Q_OS_LINUX)
    return {QStringLiteral("/usr/bin/ping"),
            {QStringLiteral("-4"), QStringLiteral("-n"), QStringLiteral("-q"), QStringLiteral("-c"), QStringLiteral("1"),
             QStringLiteral("-w"), deadline, address.toString()}, 1};
#else
    Q_UNUSED(deadline)
    return {};
#endif
}

bool InternetConnectivity::canProbe() const
{
    return m_WindowVisible && m_Settings.enabled && m_ConfigurationError.isEmpty();
}

void InternetConnectivity::setWindowVisible(bool visible)
{
    if (m_WindowVisible == visible) return;
    m_WindowVisible = visible;
    if (canProbe()) checkNow();
    else cancelProbes(false);
    emit modeChanged();
}

void InternetConnectivity::setSummaryVisible(bool visible)
{
    if (m_SummaryVisible == visible) return;
    m_SummaryVisible = visible;
    if (canProbe()) checkNow();
    emit modeChanged();
}

void InternetConnectivity::stop()
{
    setWindowVisible(false);
}

void InternetConnectivity::cancelProbes(bool restart)
{
    m_Poll.stop();
    m_NetworkChange.stop();
    m_RestartPending = restart;
    m_Cancelled = true;
    if (m_Probe) m_Probe->cancel();
    const auto probes = m_SummaryProbes;
    for (auto* probe : probes) probe->cancel();
}

void InternetConnectivity::networkChanged()
{
    // Coalesce bursts of platform notifications. Neither OS reachability nor
    // transport type is itself proof that a configured destination responds.
    if (canProbe()) m_NetworkChange.start(250);
}

void InternetConnectivity::refreshIfStale()
{
    // Wall time includes suspend; activation need not generate another ping if
    // the normal periodic check has just run following resume.
    const int interval = m_SummaryVisible ? 1000 : m_Settings.intervalMs;
    if (canProbe() && !m_Probe && m_SummaryProbes.isEmpty() &&
            QDateTime::currentMSecsSinceEpoch() - m_LastStartedMs >= interval)
        checkNow();
}

void InternetConnectivity::checkNow()
{
    if (!canProbe()) return;
    m_Poll.stop();
    if (m_Probe || !m_SummaryProbes.isEmpty()) {
        cancelProbes(true);
        return; // Reap every old child before switching modes or restarting.
    }
    m_LastStartedMs = QDateTime::currentMSecsSinceEpoch();
    m_RestartPending = false;
    m_Cancelled = false;
    if (m_SummaryVisible) beginSummaryRound();
    else beginRound();
}

void InternetConnectivity::beginRound()
{
    m_ProbeError = false;
    m_Target = 0;
    startTarget();
}

void InternetConnectivity::startTarget()
{
    m_Probe = new Probe(this, m_CommandFactory(m_Settings.hosts.at(m_Target), m_Settings.timeoutMs),
                        m_Settings.timeoutMs, [this](Probe* probe, State state, double) {
        targetFinished(probe, state);
    });
    m_Probe->start();
}

void InternetConnectivity::targetFinished(Probe* probe, State state)
{
    if (probe != m_Probe) return;
    m_Probe = nullptr;
    if (!canProbe()) return;
    if (m_Cancelled) {
        if (m_RestartPending) checkNow();
        return;
    }
    if (state == Reachable) {
        completeRound(Reachable, m_Settings.hosts.at(m_Target));
        return;
    }
    if (state == Unavailable) m_ProbeError = true;
    if (++m_Target < m_Settings.hosts.size()) {
        startTarget();
        return;
    }
    completeRound(m_ProbeError ? Unavailable : NoResponse);
}

QVariantList InternetConnectivity::summaryResults() const
{
    QVariantList results;
    for (const auto& row : m_SummaryRows) {
        QString text;
        switch (row.state) {
        case Checking: text = tr("Checking"); break;
        case Reachable: text = tr("Reachable"); break;
        case NoResponse: text = tr("No response"); break;
        case Unavailable: text = tr("Unavailable"); break;
        case Disabled: text = tr("Disabled"); break;
        }
        results.append(QVariantMap{{QStringLiteral("address"), row.address},
                                  {QStringLiteral("state"), int(row.state)},
                                  {QStringLiteral("status"), text},
                                  {QStringLiteral("roundTripMs"), row.roundTripMs}});
    }
    return results;
}

void InternetConnectivity::beginSummaryRound()
{
    // Keep previous results visible while the next round runs. In particular,
    // don't recreate all rows as Checking once per second.
    if (m_SummaryRows.isEmpty()) {
        for (const auto& address : m_Settings.hosts)
            m_SummaryRows.append(SummaryResult{address, Checking, -1});
    }
    for (int row = 0; row < m_SummaryRows.size(); ++row) {
        m_SummaryProbes.append(new Probe(this, m_CommandFactory(m_SummaryRows.at(row).address, m_Settings.timeoutMs),
                                        m_Settings.timeoutMs, [this, row](Probe* probe, State state, double rtt) {
            summaryFinished(probe, row, state, rtt);
        }));
    }
    emit summaryChanged();
    // Copy the list because FailedToStart/finished may remove its own probe.
    const auto probes = m_SummaryProbes;
    for (auto* probe : probes) probe->start();
}

void InternetConnectivity::summaryFinished(Probe* probe, int row, State state, double roundTripMs)
{
    if (!m_SummaryProbes.removeOne(probe)) return;
    if (!m_Cancelled && canProbe()) {
        m_SummaryRows[row].state = state;
        m_SummaryRows[row].roundTripMs = roundTripMs;
    }
    if (m_SummaryProbes.isEmpty()) {
        if (m_Cancelled) {
            if (m_RestartPending && canProbe()) checkNow();
        }
        else if (canProbe()) {
            State result = NoResponse;
            QString address;
            for (const auto& item : m_SummaryRows) {
                if (item.state == Reachable) { result = Reachable; address = item.address; break; }
                if (item.state == Unavailable) result = Unavailable;
            }
            completeRound(result, address);
        }
    }
    emit summaryChanged();
}

void InternetConnectivity::completeRound(State state, const QString& address)
{
    const QString detail = state == Reachable ?
        tr("Reply received from %1. This checks access to a configured address, "
           "not DNS, VPN connectivity or a particular PLANK Host.").arg(address) :
        state == Unavailable ?
        tr("One or more Internet checks could not run. Check the system ping utility "
           "and client.conf. PLANK connections remain available.") :
        tr("None of the configured Internet addresses replied. Your connection may "
           "be unavailable, or the network may block ping. PLANK connections remain available.");
    setState(state, detail);
    // A synchronous state observer may hide the window or switch modes.
    if (!canProbe() || m_Probe || !m_SummaryProbes.isEmpty()) return;
    // Start-to-start cadence, without concurrent rounds or accumulating work.
    const qint64 elapsed = QDateTime::currentMSecsSinceEpoch() - m_LastStartedMs;
    const qint64 interval = m_SummaryVisible ? 1000 : m_Settings.intervalMs;
    const qint64 minimumDelay = m_SummaryVisible ? 0 : 1000;
    m_Poll.start(static_cast<int>(qBound(minimumDelay, interval - elapsed, interval)));
}

void InternetConnectivity::setState(State state, const QString& detail)
{
    if (state == m_State && detail == m_Detail) return;
    if (state != m_State) qInfo() << "Internet connectivity:" << state;
    m_State = state;
    m_Detail = detail;
    emit stateChanged();
}
