#pragma once

// FFmpeg EAGAIN is a direction change, not a reason to sleep or discard input.
// drain() returns the number of frames consumed, or a negative error; accepted()
// attaches metadata only after the codec has retained this packet.
template<class Send, class Drain, class Accepted>
int plankSendAndDrain(int again, int invalidState, Send send, Drain drain, Accepted accepted)
{
    int result = send();
    if (result == again) {
        const int drained = drain();
        if (drained < 0) return drained;
        if (drained == 0) return invalidState; // No progress: never spin forever.
        result = send(); // The caller still owns the exact same packet.
        if (result == again) return invalidState;
    }
    if (result < 0) return result;
    accepted();
    result = drain();
    return result < 0 ? result : 0;
}
