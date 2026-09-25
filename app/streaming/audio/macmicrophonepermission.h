#pragma once
#include <functional>
// Non-prompting: -1 denied/restricted, 0 not determined, 1 granted.
int plankMacMicrophonePermission();
// Interactive launcher only; completes after consent or an existing decision.
// Granting access does not open a recording device or forward microphone audio.
void plankMacRequestMicrophonePermission(std::function<void()> completed);
