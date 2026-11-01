#include <QtTest>

#include <AppKit/AppKit.h>

#include "macclipboardsync.h"
#include <QTimer>
#include <thread>

NSPasteboard* plankClipboardTestPasteboard()
{
    static NSPasteboard* pasteboard = [NSPasteboard pasteboardWithUniqueName];
    return pasteboard;
}

class TestMacClipboardSync : public QObject
{
    Q_OBJECT

private:
    static void setPasteboardText(const char* text)
    {
        @autoreleasepool {
            NSPasteboard* pasteboard = plankClipboardTestPasteboard();
            [pasteboard clearContents];
            [pasteboard setString:[NSString stringWithUTF8String:text]
                         forType:NSPasteboardTypeString];
        }
    }

    static std::vector<std::uint8_t> oneFrame(const char* text,
                                              std::uint64_t generation)
    {
        const auto frames = plank::clipboard::buildEventFrames(
                    reinterpret_cast<const std::uint8_t*>(text),
                    std::strlen(text),
                    generation,
                    PLANK_CLIPBOARD_MAX_EVENT_CHUNK_SIZE);
        Q_ASSERT(frames.size() == 1);
        return frames.front();
    }

private slots:
    void initTestCase()
    {
        QVERIFY(plankClipboardTestPasteboard() != nil);
        QVERIFY([plankClipboardTestPasteboard() setString:@"fixture" forType:NSPasteboardTypeString]);
    }
    void cleanupTestCase()
    {
        [plankClipboardTestPasteboard() releaseGlobally];
    }
    void rejectsMalformedFrameLength();
    void acceptsIndependentDirectionGenerations();
    void resetsGenerationAndRejectsStaleEventsOnReconnect();
    void suppressesRepeatedHostText();
    void sendsOnlyWithStreamFocus();
    void backgroundHostOfferPreservesLocalCopy();
    void retriesAfterTransportSendFailure();
    void resumesPartialCopyWithoutRestartingGeneration();
    void enforces512KiBAndRejectsEmbeddedNul();
    void replacesUnsentCopyWithNewGeneration();
    void resendsTextAfterHostClipboardChanges();
    void rejectsHostOfferWhenEventQueueFails();
    void ignoresOffersWhileStopped();
    void validatesUnicodeScalars();
    void disconnectDoesNotResendRemoteText();
    void disconnectPreservesNewLocalCopy();
    void localCopyCanReturnToLastRemoteText();
    void newerOfferReplacesPendingText();
    void repeatHostOfferAfterLocalChangeAppliesAgain();
    void periodicPollingResumesAfterReconnect();
    void sessionReconnectPollingWiring();
};

void TestMacClipboardSync::rejectsMalformedFrameLength()
{
    int queuedEvents = 0;
    MacClipboardSync sync(
                [](const std::uint8_t*, std::size_t) { return true; },
                [] { return true; },
                [] { return true; },
                [&] {
                    ++queuedEvents;
                    return true;
                });
    sync.start();

    auto truncated = oneFrame("frame", 1);
    truncated.pop_back();
    QVERIFY(!sync.handleHostOffer(truncated.data(), truncated.size()));
    QCOMPARE(queuedEvents, 0);

    auto trailing = oneFrame("frame", 2);
    trailing.push_back(0);
    QVERIFY(!sync.handleHostOffer(trailing.data(), trailing.size()));
    QCOMPARE(queuedEvents, 0);
}

void TestMacClipboardSync::acceptsIndependentDirectionGenerations()
{
    std::vector<std::vector<std::uint8_t>> sent;
    int queuedEvents = 0;
    MacClipboardSync sync(
                [&](const std::uint8_t* data, std::size_t size) {
                    sent.emplace_back(data, data + size);
                    return true;
                },
                [] { return true; },
                [] { return true; },
                [&] {
                    ++queuedEvents;
                    return true;
                });
    sync.start();

    setPasteboardText("client generation one");
    sync.pollLocalClipboardOnMainThread();
    QCOMPARE(sent.size(), std::size_t {1});

    PLANK_CLIPBOARD_WIRE_HEADER outbound {};
    std::memcpy(&outbound, sent.front().data(), sizeof(outbound));
    QCOMPARE(qFromLittleEndian(outbound.generation), std::uint64_t {1});

    const auto host = oneFrame("host generation one", 1);
    QVERIFY(sync.handleHostOffer(host.data(), host.size()));
    QCOMPARE(queuedEvents, 1);
    QVERIFY(sync.applyPendingHostTextOnMainThread());
}

void TestMacClipboardSync::resetsGenerationAndRejectsStaleEventsOnReconnect()
{
    int queuedEvents = 0;
    MacClipboardSync sync(
                [](const std::uint8_t*, std::size_t) { return true; },
                [] { return true; },
                [] { return true; },
                [&] {
                    ++queuedEvents;
                    return true;
                });
    sync.start();

    const auto oldSession = oneFrame("old session", 42);
    QVERIFY(sync.handleHostOffer(oldSession.data(), oldSession.size()));
    QCOMPARE(queuedEvents, 1);

    sync.stop();
    sync.start();
    QVERIFY(!sync.applyPendingHostTextOnMainThread());

    const auto newSession = oneFrame("new session", 1);
    QVERIFY(sync.handleHostOffer(newSession.data(), newSession.size()));
    QCOMPARE(queuedEvents, 2);
    QVERIFY(sync.applyPendingHostTextOnMainThread());
}

void TestMacClipboardSync::suppressesRepeatedHostText()
{
    int queuedEvents = 0;
    MacClipboardSync sync(
                [](const std::uint8_t*, std::size_t) { return true; },
                [] { return true; },
                [] { return true; },
                [&] {
                    ++queuedEvents;
                    return true;
                });
    sync.start();

    auto frame = oneFrame("same text", 1);
    QVERIFY(sync.handleHostOffer(frame.data(), frame.size()));
    QCOMPARE(queuedEvents, 1);

    frame = oneFrame("same text", 2);
    QVERIFY(sync.handleHostOffer(frame.data(), frame.size()));
    QCOMPARE(queuedEvents, 1);
    QVERIFY(sync.applyPendingHostTextOnMainThread());

    frame = oneFrame("same text", 3);
    QVERIFY(sync.handleHostOffer(frame.data(), frame.size()));
    QCOMPARE(queuedEvents, 2);
    QVERIFY(!sync.applyPendingHostTextOnMainThread());
}

void TestMacClipboardSync::sendsOnlyWithStreamFocus()
{
    bool focused = false;
    std::vector<std::vector<std::uint8_t>> sent;
    MacClipboardSync sync(
                [&](const std::uint8_t* data, std::size_t size) {
                    sent.emplace_back(data, data + size);
                    return true;
                },
                [&] { return focused; },
                [] { return true; },
                [] { return true; });
    sync.start();

    setPasteboardText("focus gated");
    sync.pollLocalClipboardOnMainThread();
    QVERIFY(sent.empty());

    focused = true;
    sync.pollLocalClipboardOnMainThread();
    QCOMPARE(sent.size(), std::size_t {1});
}

void TestMacClipboardSync::retriesAfterTransportSendFailure()
{
    int sends = 0;
    MacClipboardSync sync(
                [&](const std::uint8_t*, std::size_t) {
                    return ++sends > 1;
                },
                [] { return true; },
                [] { return true; },
                [] { return true; });
    sync.start();

    setPasteboardText("retry send");
    sync.pollLocalClipboardOnMainThread();
    QCOMPARE(sends, 1);
    sync.pollLocalClipboardOnMainThread();
    QCOMPARE(sends, 2);
}

void TestMacClipboardSync::resendsTextAfterHostClipboardChanges()
{
    std::vector<std::vector<std::uint8_t>> sent;
    MacClipboardSync sync(
                [&](const std::uint8_t* data, std::size_t size) {
                    sent.emplace_back(data, data + size);
                    return true;
                },
                [] { return true; },
                [] { return true; },
                [] { return true; });
    sync.start();

    setPasteboardText("A");
    sync.pollLocalClipboardOnMainThread();
    QCOMPARE(sent.size(), std::size_t {1});

    const auto host = oneFrame("B", 1);
    QVERIFY(sync.handleHostOffer(host.data(), host.size()));
    QVERIFY(sync.applyPendingHostTextOnMainThread());

    setPasteboardText("A");
    sync.pollLocalClipboardOnMainThread();
    QCOMPARE(sent.size(), std::size_t {2});
}

void TestMacClipboardSync::rejectsHostOfferWhenEventQueueFails()
{
    MacClipboardSync sync(
                [](const std::uint8_t*, std::size_t) { return true; },
                [] { return true; },
                [] { return true; },
                [] { return false; });
    sync.start();

    const auto frame = oneFrame("queue failure", 1);
    QVERIFY(!sync.handleHostOffer(frame.data(), frame.size()));
    QVERIFY(!sync.applyPendingHostTextOnMainThread());
}

void TestMacClipboardSync::ignoresOffersWhileStopped()
{
    int queuedEvents = 0;
    MacClipboardSync sync(
                [](const std::uint8_t*, std::size_t) { return true; },
                [] { return true; },
                [] { return true; },
                [&] {
                    ++queuedEvents;
                    return true;
                });
    sync.start();
    sync.stop();

    const auto frame = oneFrame("after stop", 1);
    QVERIFY(sync.handleHostOffer(frame.data(), frame.size()));
    QCOMPARE(queuedEvents, 0);
}

void TestMacClipboardSync::disconnectDoesNotResendRemoteText()
{
    int sends = 0;
    auto sender = [&](const std::uint8_t*, std::size_t) { ++sends; return true; };
    auto yes = [] { return true; };
    MacClipboardSync first(sender, yes, yes, yes);
    first.start();
    auto frame = oneFrame("private Host A text", 1);
    QVERIFY(first.handleHostOffer(frame.data(), frame.size()));
    QVERIFY(first.applyPendingHostTextOnMainThread());
    // Teardown can occur on the deferred cleanup worker. Do not process Qt's
    // event loop before the next poll: cleanup must also protect a fresh object.
    std::thread cleanup([&] { first.stop(); });
    cleanup.join();
    MacClipboardSync second(sender, yes, yes, yes);
    second.start();
    second.pollLocalClipboardOnMainThread();
    QCOMPARE(sends, 0);
    QVERIFY([[plankClipboardTestPasteboard() stringForType:NSPasteboardTypeString] length] == 0);
    frame = oneFrame("Host B text", 1);
    QVERIFY(second.handleHostOffer(frame.data(), frame.size()));
    QVERIFY(second.applyPendingHostTextOnMainThread());
    second.stop();
    second.start();
    second.pollLocalClipboardOnMainThread();
    QCOMPARE(sends, 0);
}

void TestMacClipboardSync::disconnectPreservesNewLocalCopy()
{
    auto yes = [] { return true; };
    MacClipboardSync sync([](const std::uint8_t*, std::size_t) { return true; }, yes, yes, yes);
    sync.start();
    const auto frame = oneFrame("A", 1);
    QVERIFY(sync.handleHostOffer(frame.data(), frame.size()));
    QVERIFY(sync.applyPendingHostTextOnMainThread());
    // Even identical text is a new user-owned copy when its change count differs.
    setPasteboardText("A");
    const auto count = plankClipboardTestPasteboard().changeCount;
    sync.stop();
    QCOMPARE(plankClipboardTestPasteboard().changeCount, count);
    QVERIFY([[plankClipboardTestPasteboard() stringForType:NSPasteboardTypeString] isEqualToString:@"A"]);
}

void TestMacClipboardSync::localCopyCanReturnToLastRemoteText()
{
    int sends = 0;
    auto yes = [] { return true; };
    MacClipboardSync sync([&](const std::uint8_t*, std::size_t) { ++sends; return true; }, yes, yes, yes);
    sync.start();
    const auto frame = oneFrame("A", 1);
    QVERIFY(sync.handleHostOffer(frame.data(), frame.size()));
    QVERIFY(sync.applyPendingHostTextOnMainThread());
    setPasteboardText("B");
    sync.pollLocalClipboardOnMainThread();
    QCOMPARE(sends, 1);
    setPasteboardText("A");
    sync.pollLocalClipboardOnMainThread();
    QCOMPARE(sends, 2);
}

void TestMacClipboardSync::newerOfferReplacesPendingText()
{
    auto yes = [] { return true; };
    MacClipboardSync sync([](const std::uint8_t*, std::size_t) { return true; }, yes, yes, yes);
    sync.start();
    auto frame = oneFrame("A", 1);
    QVERIFY(sync.handleHostOffer(frame.data(), frame.size()));
    QVERIFY(sync.applyPendingHostTextOnMainThread());
    frame = oneFrame("B", 2);
    QVERIFY(sync.handleHostOffer(frame.data(), frame.size()));
    frame = oneFrame("A", 3);
    QVERIFY(sync.handleHostOffer(frame.data(), frame.size()));
    sync.applyPendingHostTextOnMainThread();
    QVERIFY([[plankClipboardTestPasteboard() stringForType:NSPasteboardTypeString] isEqualToString:@"A"]);
}

void TestMacClipboardSync::repeatHostOfferAfterLocalChangeAppliesAgain()
{
    auto yes = [] { return true; };
    MacClipboardSync sync([](const std::uint8_t*, std::size_t) { return true; }, yes, yes, yes);
    sync.start();
    auto frame = oneFrame("A", 1);
    QVERIFY(sync.handleHostOffer(frame.data(), frame.size()));
    QVERIFY(sync.applyPendingHostTextOnMainThread());
    setPasteboardText("B"); // Not observed by the local poll yet.
    frame = oneFrame("A", 2);
    QVERIFY(sync.handleHostOffer(frame.data(), frame.size()));
    QVERIFY(sync.applyPendingHostTextOnMainThread());
    QVERIFY([[plankClipboardTestPasteboard() stringForType:NSPasteboardTypeString] isEqualToString:@"A"]);
}

void TestMacClipboardSync::validatesUnicodeScalars()
{
    const char valid[] = "\xF0\x9F\x94\xA5";
    QVERIFY(plank::clipboard::validUtf8(valid, sizeof(valid) - 1));

    const char overlong[] = "\xC0\xAF";
    QVERIFY(!plank::clipboard::validUtf8(overlong, sizeof(overlong) - 1));

    const char surrogate[] = "\xED\xA0\x80";
    QVERIFY(!plank::clipboard::validUtf8(surrogate, sizeof(surrogate) - 1));

    const char outOfRange[] = "\xF4\x90\x80\x80";
    QVERIFY(!plank::clipboard::validUtf8(outOfRange, sizeof(outOfRange) - 1));

    const char embeddedNull[] = {'a', '\0', 'b'};
    QVERIFY(!plank::clipboard::validUtf8(embeddedNull, sizeof(embeddedNull)));
}

void TestMacClipboardSync::periodicPollingResumesAfterReconnect()
{
    unsigned sends = 0;
    MacClipboardSync sync([&](const std::uint8_t*, std::size_t) { ++sends; return true; },
        [] { return true; }, [] { return true; }, [] { return true; });
    QTimer timer;
    connect(&timer, &QTimer::timeout, [&] { sync.pollLocalClipboardOnMainThread(); });
    timer.start(50);
    setPasteboardText("before reconnect");
    sync.start();
    QTRY_COMPARE(sends, 1u);
    sync.stop();
    setPasteboardText("during reconnect");
    QTest::qWait(150);
    QCOMPARE(sends, 1u);
    sync.start();
    QTRY_COMPARE(sends, 2u);
    setPasteboardText("after reconnect");
    QTRY_COMPARE(sends, 3u);
}

// Pair the real timer/NSPasteboard lifecycle test above with a guard on its
// Session call sites. Full renderer/network handoff remains a paired-system gate.
void TestMacClipboardSync::sessionReconnectPollingWiring()
{
    QFile runtime(QFINDTESTDATA("../../app/streaming/qt/sessionruntime.cpp"));
    QVERIFY(runtime.open(QIODevice::ReadOnly));
    const auto code = runtime.readAll();
    QVERIFY(code.contains("m_Session->m_ClipboardSync->pollLocalClipboardOnMainThread();"));
    QFile session(QFINDTESTDATA("../../app/streaming/session.cpp"));
    QVERIFY(session.open(QIODevice::ReadOnly));
    const auto lifecycle = session.readAll();
    QVERIFY(lifecycle.contains("startClipboardSync();"));
    QVERIFY(lifecycle.contains("stopClipboardSync();"));
}

void TestMacClipboardSync::resumesPartialCopyWithoutRestartingGeneration()
{
    bool congested = true;
    std::vector<std::vector<std::uint8_t>> sent;
    MacClipboardSync sync([&](const std::uint8_t* data, std::size_t size) {
        if (congested && sent.size() == 2) return false;
        sent.emplace_back(data, data + size); return true;
    }, [] { return true; }, [] { return true; }, [] { return true; });
    sync.start();
    const std::string text(PLANK_CLIPBOARD_MAX_TEXT_SIZE, 'x');
    setPasteboardText(text.c_str());
    sync.pollLocalClipboardOnMainThread();
    QCOMPARE(sent.size(), std::size_t(2));
    congested = false;
    sync.pollLocalClipboardOnMainThread();
    QCOMPARE(sent.size(), std::size_t(65));
    std::uint32_t offset = 0;
    for (const auto& frame : sent) {
        PlankClipboardChunk chunk {};
        QVERIFY(plank_clipboard_decode(frame.data(), frame.size(), PLANK_CLIPBOARD_INPUT_BYTES, &chunk));
        QCOMPARE(chunk.generation, std::uint64_t(1));
        QCOMPARE(chunk.offset, offset); offset += chunk.size;
    }
    QCOMPARE(offset, std::uint32_t(text.size()));
    sync.pollLocalClipboardOnMainThread();
    QCOMPARE(sent.size(), std::size_t(65));
}

void TestMacClipboardSync::enforces512KiBAndRejectsEmbeddedNul()
{
    unsigned sent = 0;
    MacClipboardSync sync([&](const std::uint8_t*, std::size_t) { ++sent; return true; },
        [] { return true; }, [] { return true; }, [] { return true; });
    sync.start();
    setPasteboardText(std::string(PLANK_CLIPBOARD_MAX_TEXT_SIZE + 1, 'x').c_str());
    sync.pollLocalClipboardOnMainThread();
    QCOMPARE(sent, 0u);
    const char bytes[] = {'a', 0, 'b'};
    NSString* text = [[NSString alloc] initWithBytes:bytes length:sizeof(bytes) encoding:NSUTF8StringEncoding];
    [plankClipboardTestPasteboard() clearContents];
    [plankClipboardTestPasteboard() setString:text forType:NSPasteboardTypeString];
    [text release];
    sync.pollLocalClipboardOnMainThread();
    QCOMPARE(sent, 0u);
    setPasteboardText("valid next copy");
    sync.pollLocalClipboardOnMainThread();
    QCOMPARE(sent, 1u);
}

void TestMacClipboardSync::replacesUnsentCopyWithNewGeneration()
{
    bool congested = true;
    std::vector<std::vector<std::uint8_t>> sent;
    MacClipboardSync sync([&](const std::uint8_t* data, std::size_t size) {
        if (congested && !sent.empty()) return false;
        sent.emplace_back(data, data + size); return true;
    }, [] { return true; }, [] { return true; }, [] { return true; });
    sync.start();
    setPasteboardText(std::string(20000, 'x').c_str());
    sync.pollLocalClipboardOnMainThread();
    QCOMPARE(sent.size(), std::size_t(1));
    setPasteboardText("replacement"); congested = false;
    sync.pollLocalClipboardOnMainThread();
    QCOMPARE(sent.size(), std::size_t(2));
    PlankClipboardChunk chunk {};
    QVERIFY(plank_clipboard_decode(sent.back().data(), sent.back().size(), PLANK_CLIPBOARD_INPUT_BYTES, &chunk));
    QCOMPARE(chunk.generation, std::uint64_t(2));
    QCOMPARE(chunk.offset, 0u);
    QCOMPARE(chunk.flags, 3u);
}

QTEST_MAIN(TestMacClipboardSync)

#include "test_macclipboardsync.moc"

void TestMacClipboardSync::backgroundHostOfferPreservesLocalCopy()
{
    bool focused = true;
    MacClipboardSync sync(
                [](const std::uint8_t*, std::size_t) { return true; },
                [&] { return focused; },
                [] { return true; },
                [] { return true; });
    sync.start();
    auto initial = oneFrame("old host text", 1);
    QVERIFY(sync.handleHostOffer(initial.data(), initial.size()));
    QVERIFY(sync.applyPendingHostTextOnMainThread());

    // A Host offer is queued before switching to another Mac app.
    auto delayed = oneFrame("old host text", 2);
    QVERIFY(sync.handleHostOffer(delayed.data(), delayed.size()));
    focused = false;
    setPasteboardText("new copy between Mac apps");
    QVERIFY(!sync.applyPendingHostTextOnMainThread());
    QCOMPARE(QString::fromNSString([plankClipboardTestPasteboard()
                  stringForType:NSPasteboardTypeString]),
             QStringLiteral("new copy between Mac apps"));
    // Returning to the stream must not replay the discarded background offer.
    focused = true;
    QVERIFY(!sync.applyPendingHostTextOnMainThread());
    QCOMPARE(QString::fromNSString([plankClipboardTestPasteboard()
                  stringForType:NSPasteboardTypeString]),
             QStringLiteral("new copy between Mac apps"));
}
