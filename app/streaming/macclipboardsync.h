#pragma once

#include "plankclipboard.h"

#include <cstdint>
#include <functional>
#include <mutex>
#include <optional>
#include <string>
#include <vector>

class MacClipboardSync {
public:
    using SendInputFrame = std::function<bool(const std::uint8_t*, std::size_t)>;
    using FocusPredicate = std::function<bool()>;
    using EnabledPredicate = std::function<bool()>;
    using QueueHostText = std::function<bool()>;

    MacClipboardSync(SendInputFrame sendInputFrame,
                     FocusPredicate hasStreamFocus,
                     EnabledPredicate isEnabled,
                     QueueHostText queueHostText);
    ~MacClipboardSync();

    void start();
    void stop();
    bool handleHostOffer(const std::uint8_t* data, std::size_t length);
    bool applyPendingHostTextOnMainThread();
    void pollLocalClipboardOnMainThread();

private:
    struct PendingHostText {
        std::uint64_t sessionEpoch = 0;
        std::uint64_t generation = 0;
        std::vector<std::uint8_t> text;
    };

    bool sendLocalClipboard(const std::string& text, std::uint64_t expectedEpoch);

    SendInputFrame m_SendInputFrame;
    FocusPredicate m_HasStreamFocus;
    EnabledPredicate m_IsEnabled;
    QueueHostText m_QueueHostText;
    std::mutex m_StateMutex;
    bool m_Running = false;
    bool m_ApplyingRemote = false;
    std::uint64_t m_SessionEpoch = 0;
    plank::clipboard::Assembly m_Assembly;
    std::optional<PendingHostText> m_PendingHostText;
    std::int64_t m_LastPasteboardChangeCount = -1;
    std::int64_t m_RemotePasteboardChangeCount = -1;
    std::uint64_t m_OutboundGeneration = 0;
    std::uint64_t m_LastAppliedHostGeneration = 0;
    std::string m_LastAppliedHostText;
    std::string m_OutgoingText;
    std::vector<std::vector<std::uint8_t>> m_OutgoingFrames;
    std::size_t m_NextOutgoingFrame = 0;
};
