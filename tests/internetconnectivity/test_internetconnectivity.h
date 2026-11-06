#pragma once

#include <QObject>

class ConnectivityTest : public QObject
{
    Q_OBJECT

private slots:
    void policyDefaultsAndExplicitValues();
    void invalidPolicy_data();
    void invalidPolicy();
    void disabledMakesNoProbes();
    void orderedFallbackAndPeriodicRestart();
    void noResponseRecoversWithoutInteraction();
    void deadlineDoesNotBlockUiOrLeaveChildren();
    void unavailableIsNotAnInternetOutage();
    void probeErrorCanFallBack();
    void networkErrorIsNotUtilityFailure_data();
    void networkErrorIsNotUtilityFailure();
    void restartAndStopDiscardCancelledResults();
    void systemPingLoopback();
    void summaryChecksEveryTargetConcurrently();
    void summaryRoundTripFormats_data();
    void summaryRoundTripFormats();
    void summaryFailuresAndDeadlines();
    void summaryCancellationAndReopen();
    void hiddenWindowStopsAndResumes();
    void liveSummaryPreservesResultsAndDoesNotOverlap();
    void statusUi();
};
