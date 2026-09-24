#pragma once
#include <opus.h>

// The authenticated PMIC v2 contract is stereo, 48 kHz, 10 ms. Force coupled
// stereo even for silence/mono devices so every packet matches that contract.
enum { PlankMicrophoneRate = 48000, PlankMicrophoneChannels = 2,
       PlankMicrophoneFrames = 480, PlankMicrophoneBitrate = 192000 };

static inline OpusEncoder* plankMicrophoneCreateEncoder(void)
{
    int error = OPUS_OK;
    OpusEncoder* encoder = opus_encoder_create(PlankMicrophoneRate,
        PlankMicrophoneChannels, OPUS_APPLICATION_AUDIO, &error);
    if (!encoder || error != OPUS_OK) {
        if (encoder) opus_encoder_destroy(encoder);
        return 0;
    }
    if (opus_encoder_ctl(encoder, OPUS_SET_BITRATE(PlankMicrophoneBitrate)) != OPUS_OK ||
        opus_encoder_ctl(encoder, OPUS_SET_VBR(1)) != OPUS_OK ||
        opus_encoder_ctl(encoder, OPUS_SET_VBR_CONSTRAINT(1)) != OPUS_OK ||
        opus_encoder_ctl(encoder, OPUS_SET_FORCE_CHANNELS(PlankMicrophoneChannels)) != OPUS_OK ||
        opus_encoder_ctl(encoder, OPUS_SET_COMPLEXITY(5)) != OPUS_OK) {
        opus_encoder_destroy(encoder);
        return 0;
    }
    return encoder;
}
