#include "../../app/backend/pollingthread.h"
#include <QSemaphore>
#include <QtTest>

class Worker : public PollingThread {
public:
    QSemaphore entered;
    unsigned long delay = 3000;
    void run() override { entered.release(); waitForNextPoll(delay); }
};

class TestPollingThread : public QObject {
    Q_OBJECT
private slots:
    void cancellationCannotMissTheWait() {
        for (int attempt = 0; attempt < 300; ++attempt) {
            Worker worker;
            worker.start();
            QVERIFY(worker.entered.tryAcquire(1, 2000));
            worker.stop();
            QVERIFY(worker.wait(1000));
            QVERIFY(worker.stopping());
        }
    }
    void cancellationBeforeStartIsDurable() {
        Worker worker;
        worker.stop();
        worker.start();
        QVERIFY(worker.wait(1000));
    }
    void ordinaryTimeoutStillSchedulesAnotherPoll() {
        Worker worker;
        worker.delay = 50;
        worker.start();
        QVERIFY(worker.entered.tryAcquire(1, 2000));
        QVERIFY(worker.wait(1000));
        QVERIFY(!worker.stopping());
    }
};
QTEST_GUILESS_MAIN(TestPollingThread)
#include "test_pollingthread.moc"
