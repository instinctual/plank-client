#pragma once

#include "../settings/plankclientpolicy.h"

#include <QObject>
#include <QProcess>
#include <QTimer>
#include <QVariantList>
#include <functional>

// Informational only. This monitor has no access to bookmarks, authentication,
// Host polling or session recovery; a failed ping can never gate a connection.
class InternetConnectivity : public QObject
{
    Q_OBJECT
    Q_PROPERTY(State state READ state NOTIFY stateChanged)
    Q_PROPERTY(QString statusText READ statusText NOTIFY stateChanged)
    Q_PROPERTY(QString detail READ detail NOTIFY stateChanged)
    Q_PROPERTY(bool windowVisible READ windowVisible WRITE setWindowVisible NOTIFY modeChanged)
    Q_PROPERTY(bool summaryVisible READ summaryVisible WRITE setSummaryVisible NOTIFY modeChanged)
    Q_PROPERTY(QVariantList summaryResults READ summaryResults NOTIFY summaryChanged)
    Q_PROPERTY(bool summaryRunning READ summaryRunning NOTIFY summaryChanged)
    Q_PROPERTY(QString summaryError READ summaryError NOTIFY summaryChanged)

public:
    enum State { Disabled, Checking, Reachable, NoResponse, Unavailable };
    Q_ENUM(State)

    struct PingCommand {
        QString program;
        QStringList arguments;
        int noResponseExitCode = 1;
    };
    using CommandFactory = std::function<PingCommand(const QString&, int)>;

    explicit InternetConnectivity(const PlankClientPolicy::InternetCheck& settings,
                                  QObject* parent = nullptr,
                                  CommandFactory commandFactory = systemPingCommand);
    ~InternetConnectivity() override;

    State state() const { return m_State; }
    QString statusText() const;
    QString detail() const { return m_Detail; }
    static PingCommand systemPingCommand(const QString& address, int timeoutMs);

    bool windowVisible() const { return m_WindowVisible; }
    bool summaryVisible() const { return m_SummaryVisible; }
    void setWindowVisible(bool visible);
    void setSummaryVisible(bool visible);
    void stop();
    void checkNow();
    void networkChanged();
    void refreshIfStale();

    QVariantList summaryResults() const;
    bool summaryRunning() const { return !m_SummaryProbes.isEmpty(); }
    QString summaryError() const { return m_SummaryError; }

signals:
    void stateChanged();
    void summaryChanged();
    void modeChanged();

private:
    struct Probe;
    struct SummaryResult {
        QString address;
        State state = Checking;
        double roundTripMs = -1;
    };
    void beginRound();
    void beginSummaryRound();
    bool canProbe() const;
    void cancelProbes(bool restart);
    void startTarget();
    void targetFinished(Probe* probe, State state);
    void summaryFinished(Probe* probe, int row, State state, double roundTripMs);
    void completeRound(State state, const QString& address = {});
    void setState(State state, const QString& detail);

    const PlankClientPolicy::InternetCheck m_Settings;
    const CommandFactory m_CommandFactory;
    Probe* m_Probe = nullptr;
    QTimer m_Poll;
    QTimer m_NetworkChange;
    State m_State = Checking;
    QString m_Detail;
    int m_Target = 0;
    qint64 m_LastStartedMs = 0;
    bool m_WindowVisible = false;
    bool m_SummaryVisible = false;
    bool m_Cancelled = false;
    bool m_RestartPending = false;
    bool m_ProbeError = false;
    QString m_ConfigurationError;
    QList<Probe*> m_SummaryProbes;
    QList<SummaryResult> m_SummaryRows;
    QString m_SummaryError;
};
