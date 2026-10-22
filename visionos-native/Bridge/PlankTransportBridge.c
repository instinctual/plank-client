#include "PlankTransportBridge.h"
#include "PlankAddress.h"
#include "PlankRawHidFrame.h"
#include "../../moonlight-common-c/moonlight-common-c/src/plank.h"

#include <CommonCrypto/CommonDigest.h>
#include <stdatomic.h>
#include <stdio.h>
#include <math.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <unistd.h>

#if PLANK_NATIVE_TRANSPORT
#include <plank_transport.h>
#include <plank_transport_control.h>
#include <plank_transport_event.h>
#include <plank_transport_input.h>
#include <plank_transport_setup.h>
#endif

struct PlankVisionTransport {
#if PLANK_NATIVE_TRANSPORT
    PlankTransportNativeEndpoint *endpoint;
#else
    void *endpoint;
#endif
    // Packed kind | lane << 8 | state << 16 | native result << 32. Zero until
    // the first terminal result; later failures never replace it.
    _Atomic uint64_t first_failure;
    // Owned by the serial input sender: 0 = fresh endpoint, 1 = reset queued,
    // 2 = reset failed. A Relay reconnect does not create a new Host endpoint.
    uint8_t raw_hid_session_state;
};

static void set_error(char *error, size_t capacity, const char *message) {
    if (error == NULL || capacity == 0) return;
    snprintf(error, capacity, "%s", message == NULL ? "Unknown transport error" : message);
}

#if PLANK_NATIVE_TRANSPORT
// The endpoint state decides first: once it has failed or stopped, every lane
// reports that, whatever the individual call returned.
static void record_failure(
    PlankVisionTransport *transport, uint32_t lane, int32_t native_result,
    uint32_t kind_when_ready) {
    const uint32_t state = plank_transport_native_endpoint_state(transport->endpoint);
    uint32_t kind = kind_when_ready;
    if (state == PLANK_TRANSPORT_STATE_FAILED) {
        kind = PLANK_VISION_FAILURE_TERMINATED;
    } else if (state == PLANK_TRANSPORT_STATE_STOPPING ||
               state == PLANK_TRANSPORT_STATE_STOPPED) {
        kind = PLANK_VISION_FAILURE_CANCELLED;
    } else if (kind == PLANK_VISION_FAILURE_NONE) {
        switch (native_result) {
        case PLANK_TRANSPORT_ERROR_RUNTIME:
            kind = PLANK_VISION_FAILURE_TERMINATED;
            break;
        case PLANK_TRANSPORT_ERROR_INVALID_STATE:
            kind = PLANK_VISION_FAILURE_CANCELLED;
            break;
        case PLANK_TRANSPORT_ERROR_BUFFER_TOO_SMALL:
            kind = PLANK_VISION_FAILURE_BUFFER_LIMIT;
            break;
        default:
            kind = PLANK_VISION_FAILURE_INTERNAL;
            break;
        }
    }
    const uint64_t packed = (uint64_t)(kind & 0xffu) |
                            ((uint64_t)(lane & 0xffu) << 8) |
                            ((uint64_t)(state & 0xffu) << 16) |
                            ((uint64_t)(uint32_t)native_result << 32);
    uint64_t expected = 0;
    atomic_compare_exchange_strong(&transport->first_failure, &expected, packed);
}

// Keeps the existing OK/ERROR contract while preserving why a send failed.
// A full native input queue returns TIMEOUT from the endpoint.
static int32_t finish_input_send(PlankVisionTransport *transport, int32_t result) {
    if (result == PLANK_TRANSPORT_OK) return PLANK_VISION_TRANSPORT_OK;
    record_failure(transport, PLANK_VISION_LANE_INPUT, result,
                   result == PLANK_TRANSPORT_TIMEOUT ?
                       PLANK_VISION_FAILURE_QUEUE_FULL : PLANK_VISION_FAILURE_NONE);
    return PLANK_VISION_TRANSPORT_ERROR;
}

static int32_t invalid_data_event(PlankVisionTransport *transport) {
    record_failure(transport, PLANK_VISION_LANE_DATA, PLANK_TRANSPORT_OK,
                   PLANK_VISION_FAILURE_INVALID_PAYLOAD);
    return PLANK_VISION_TRANSPORT_ERROR;
}

static int approve_expected_peer_certificate(
    PlankTransportNativeEndpoint *endpoint,
    const char *expected_sha256,
    char *error,
    size_t error_capacity) {
    size_t certificate_size = 0;
    int32_t result = plank_transport_native_endpoint_peer_certificate(
        endpoint, NULL, 0, &certificate_size);
    if (result != PLANK_TRANSPORT_ERROR_BUFFER_TOO_SMALL ||
            certificate_size == 0 || certificate_size > 1024 * 1024) {
        set_error(error, error_capacity,
                  "The Host did not provide a valid transport certificate");
        return -1;
    }

    uint8_t *certificate = malloc(certificate_size);
    if (certificate == NULL) {
        set_error(error, error_capacity,
                  "Unable to inspect the Host transport certificate");
        return -1;
    }
    result = plank_transport_native_endpoint_peer_certificate(
        endpoint, certificate, certificate_size, &certificate_size);
    if (result != PLANK_TRANSPORT_OK) {
        free(certificate);
        set_error(error, error_capacity,
                  "Unable to read the Host transport certificate");
        return -1;
    }

    unsigned char digest[CC_SHA256_DIGEST_LENGTH];
    CC_SHA256(certificate, (CC_LONG)certificate_size, digest);
    free(certificate);

    char actual_sha256[CC_SHA256_DIGEST_LENGTH * 2 + 1];
    for (size_t index = 0; index < CC_SHA256_DIGEST_LENGTH; ++index) {
        snprintf(actual_sha256 + index * 2, 3, "%02x", digest[index]);
    }
    actual_sha256[sizeof(actual_sha256) - 1] = '\0';
    if (expected_sha256 == NULL || strcasecmp(actual_sha256, expected_sha256) != 0) {
        set_error(error, error_capacity,
                  "The Host transport certificate changed after sign-in");
        return -1;
    }
    if (plank_transport_native_endpoint_approve_peer_certificate(endpoint) !=
            PLANK_TRANSPORT_OK) {
        set_error(error, error_capacity,
                  "Unable to approve the Host transport certificate");
        return -1;
    }
    return 0;
}
#endif

PlankVisionTransport *plank_vision_transport_connect(
    const char *remote_host,
    uint16_t remote_port,
    const char *certificate_sha256,
    const char *session_token,
    uint32_t maximum_udp_payload,
    char *error,
    size_t error_capacity) {
#if !PLANK_NATIVE_TRANSPORT
    (void)remote_host; (void)remote_port; (void)certificate_sha256;
    (void)session_token; (void)maximum_udp_payload;
    set_error(error, error_capacity, "Native PLANK transport is not linked");
    return NULL;
#else
    if (remote_host == NULL || remote_host[0] == '\0' || remote_port == 0 ||
            certificate_sha256 == NULL || session_token == NULL) {
        set_error(error, error_capacity, "Invalid native transport configuration");
        return NULL;
    }

    char remote_address[512];
    if (plank_vision_numeric_remote_address(
            remote_host, remote_port,
            remote_address, sizeof(remote_address)) != 0) {
        set_error(error, error_capacity, "Unable to resolve the Host for native transport");
        return NULL;
    }

    PlankVisionTransport *transport = calloc(1, sizeof(*transport));
    if (transport == NULL) {
        set_error(error, error_capacity, "Unable to allocate native transport");
        return NULL;
    }

    PlankTransportConfig config = {0};
    config.struct_size = sizeof(config);
    config.abi_version = PLANK_TRANSPORT_ABI_VERSION;
    config.mode = PLANK_TRANSPORT_MODE_CLIENT;
    config.handshake_timeout_ms = 10000;
    config.idle_timeout_ms = 30000;
    config.keep_alive_interval_ms = 5000;
    config.session_mode = PLANK_TRANSPORT_SESSION_ACTIVE;
    config.max_udp_payload_size = maximum_udp_payload;
    config.remote_address = remote_address;
    config.server_name = "plank";
    config.certificate_sha256 = certificate_sha256;
    config.session_token = session_token;

    int32_t result = plank_transport_native_endpoint_create(
        &config, &transport->endpoint);
    if (result == PLANK_TRANSPORT_OK) {
        result = plank_transport_native_endpoint_start(transport->endpoint);
    }
    if (result == PLANK_TRANSPORT_OK) {
        const unsigned int attempts = 1200;
        unsigned int attempt = 0;
        int peer_certificate_approved = 0;
        for (; attempt < attempts; ++attempt) {
            const uint32_t state = plank_transport_native_endpoint_state(
                transport->endpoint);
            if (state == PLANK_TRANSPORT_STATE_PEER_VALIDATION &&
                    !peer_certificate_approved) {
                if (approve_expected_peer_certificate(
                        transport->endpoint, certificate_sha256,
                        error, error_capacity) != 0) {
                    result = PLANK_TRANSPORT_ERROR_RUNTIME;
                    break;
                }
                peer_certificate_approved = 1;
            }
            if (state == PLANK_TRANSPORT_STATE_READY) break;
            if (state == PLANK_TRANSPORT_STATE_FAILED ||
                    state == PLANK_TRANSPORT_STATE_STOPPED) {
                result = PLANK_TRANSPORT_ERROR_RUNTIME;
                break;
            }
            usleep(10000);
        }
        if (attempt == attempts) result = PLANK_TRANSPORT_TIMEOUT;
    }
    if (result != PLANK_TRANSPORT_OK) {
        if (transport->endpoint != NULL) {
            if (error == NULL || error_capacity == 0 || error[0] == '\0') {
                plank_transport_native_endpoint_last_error(
                    transport->endpoint, error, error_capacity);
            }
            if (error != NULL && error[0] == '\0') {
                set_error(error, error_capacity,
                          "The secure transport setup did not become ready");
            }
            plank_transport_native_endpoint_stop(transport->endpoint);
            plank_transport_native_endpoint_destroy(transport->endpoint);
        } else {
            set_error(error, error_capacity, "Unable to create native transport");
        }
        free(transport);
        return NULL;
    }
    return transport;
#endif
}

int32_t plank_vision_transport_negotiate(
    PlankVisionTransport *transport,
    const uint8_t *request_json,
    size_t request_size,
    uint8_t *response_json,
    size_t response_capacity,
    size_t *response_size,
    char *error,
    size_t error_capacity) {
#if !PLANK_NATIVE_TRANSPORT
    (void)transport; (void)request_json; (void)request_size;
    (void)response_json; (void)response_capacity; (void)response_size;
    set_error(error, error_capacity, "Native PLANK transport is not linked");
    return PLANK_VISION_TRANSPORT_UNAVAILABLE;
#else
    if (transport == NULL || transport->endpoint == NULL || request_json == NULL ||
            request_size == 0 || response_json == NULL || response_size == NULL) {
        set_error(error, error_capacity, "Invalid session negotiation state");
        return PLANK_VISION_TRANSPORT_ERROR;
    }

    uint8_t packet[PLANK_TRANSPORT_SETUP_MAX_PACKET_SIZE];
    size_t packet_size = 0;
    if (plank_transport_setup_encode(
            PLANK_TRANSPORT_SETUP_LAUNCH_REQUEST, 0,
            PLANK_TRANSPORT_SETUP_STATUS_OK, 1,
            request_json, request_size,
            packet, sizeof(packet), &packet_size) != 0 ||
        plank_transport_native_data_send(
            transport->endpoint, packet, packet_size) != PLANK_TRANSPORT_OK) {
        set_error(error, error_capacity, "Unable to send native session request");
        return PLANK_VISION_TRANSPORT_ERROR;
    }

    packet_size = 0;
    if (plank_transport_native_data_receive(
            transport->endpoint, packet, sizeof(packet), &packet_size, 12000) !=
            PLANK_TRANSPORT_OK) {
        set_error(error, error_capacity, "The Host did not complete session negotiation");
        return PLANK_VISION_TRANSPORT_ERROR;
    }

    PlankTransportSetupPacket response = {0};
    if (plank_transport_setup_decode(packet, packet_size, &response) != 0 ||
            response.request_id != 1 ||
            (response.flags & PLANK_TRANSPORT_SETUP_FLAG_RESPONSE) == 0 ||
            (response.type != PLANK_TRANSPORT_SETUP_LAUNCH_RESPONSE &&
             response.type != PLANK_TRANSPORT_SETUP_ERROR)) {
        set_error(error, error_capacity, "The Host returned an invalid session response");
        return PLANK_VISION_TRANSPORT_ERROR;
    }
    if (response.payload_size > response_capacity) {
        *response_size = response.payload_size;
        set_error(error, error_capacity, "Session response buffer is too small");
        return PLANK_VISION_TRANSPORT_BUFFER_TOO_SMALL;
    }
    memcpy(response_json, response.payload, response.payload_size);
    *response_size = response.payload_size;
    if (response.status != PLANK_TRANSPORT_SETUP_STATUS_OK ||
            response.type == PLANK_TRANSPORT_SETUP_ERROR) {
        if (response.payload_size > 0 && error != NULL && error_capacity > 0) {
            snprintf(error, error_capacity, "Host rejected session: %.*s",
                     (int)response.payload_size, (const char *)response.payload);
        } else {
            set_error(error, error_capacity,
                      "The Host rejected native session negotiation");
        }
        return PLANK_VISION_TRANSPORT_ERROR;
    }
    return PLANK_VISION_TRANSPORT_OK;
#endif
}

int32_t plank_vision_transport_receive_video(
    PlankVisionTransport *transport,
    PlankVisionVideoFrame *frame,
    uint8_t *payload,
    size_t payload_capacity,
    size_t *payload_size,
    uint32_t timeout_ms) {
#if !PLANK_NATIVE_TRANSPORT
    (void)transport; (void)frame; (void)payload; (void)payload_capacity;
    (void)payload_size; (void)timeout_ms;
    return PLANK_VISION_TRANSPORT_UNAVAILABLE;
#else
    if (transport == NULL || transport->endpoint == NULL || frame == NULL) {
        return PLANK_VISION_TRANSPORT_ERROR;
    }
    PlankTransportNativeVideoFrameInfo info = {0};
    info.struct_size = sizeof(info);
    const int32_t result = plank_transport_native_video_receive(
        transport->endpoint, &info, payload, payload_capacity,
        payload_size, timeout_ms);
    if (result == PLANK_TRANSPORT_TIMEOUT) return PLANK_VISION_TRANSPORT_TIMEOUT;
    if (result == PLANK_TRANSPORT_ERROR_BUFFER_TOO_SMALL) {
        return PLANK_VISION_TRANSPORT_BUFFER_TOO_SMALL;
    }
    if (result != PLANK_TRANSPORT_OK) {
        record_failure(transport, PLANK_VISION_LANE_VIDEO, result,
                       PLANK_VISION_FAILURE_NONE);
        return PLANK_VISION_TRANSPORT_ERROR;
    }
    if (payload_size != NULL && *payload_size == 0) {
        record_failure(transport, PLANK_VISION_LANE_VIDEO, result,
                       PLANK_VISION_FAILURE_INVALID_PAYLOAD);
    }

    frame->codec = info.codec;
    frame->flags = info.flags;
    frame->frame_number = info.frame_number;
    frame->pts_90khz = info.pts;
    frame->host_processing_latency = info.host_processing_latency;
    return PLANK_VISION_TRANSPORT_OK;
#endif
}

int32_t plank_vision_transport_video_stats(
    PlankVisionTransport *transport, PlankVisionVideoStats *stats) {
#if !PLANK_NATIVE_TRANSPORT
    (void)transport; (void)stats;
    return PLANK_VISION_TRANSPORT_UNAVAILABLE;
#else
    if (transport == NULL || transport->endpoint == NULL || stats == NULL) {
        return PLANK_VISION_TRANSPORT_ERROR;
    }
    PlankTransportNativeStats native_stats = {0};
    native_stats.struct_size = sizeof(native_stats);
    if (plank_transport_native_endpoint_stats(
            transport->endpoint, &native_stats) != PLANK_TRANSPORT_OK) {
        return PLANK_VISION_TRANSPORT_ERROR;
    }
    stats->frames_received = native_stats.video_frames_received;
    stats->receive_drops = native_stats.video_receive_drops;
    stats->fec_symbols_unrecovered =
        native_stats.video_fec_source_symbols_unrecovered;
    stats->input_packets_sent = native_stats.input_packets_sent;
    stats->quic_rtt_us = native_stats.quic_rtt_us;
    stats->quic_packets_lost = native_stats.quic_packets_lost;
    stats->kyproto_packets_dropped = native_stats.kyproto_packets_dropped;
    return PLANK_VISION_TRANSPORT_OK;
#endif
}

int32_t plank_vision_transport_receive_audio(
    PlankVisionTransport *transport,
    PlankVisionAudioPacket *packet,
    uint8_t *payload,
    size_t payload_capacity,
    size_t *payload_size,
    uint32_t timeout_ms) {
#if !PLANK_NATIVE_TRANSPORT
    (void)transport; (void)packet; (void)payload; (void)payload_capacity;
    (void)payload_size; (void)timeout_ms;
    return PLANK_VISION_TRANSPORT_UNAVAILABLE;
#else
    if (transport == NULL || transport->endpoint == NULL || packet == NULL ||
            payload == NULL || payload_size == NULL) {
        return PLANK_VISION_TRANSPORT_ERROR;
    }
    *payload_size = 0;
    PlankTransportNativeAudioPacketInfo info = {0};
    info.struct_size = sizeof(info);
    const int32_t result = plank_transport_native_audio_receive(
        transport->endpoint, &info, payload, payload_capacity,
        payload_size, timeout_ms);
    if (result == PLANK_TRANSPORT_TIMEOUT) return PLANK_VISION_TRANSPORT_TIMEOUT;
    if (result == PLANK_TRANSPORT_ERROR_BUFFER_TOO_SMALL) {
        return PLANK_VISION_TRANSPORT_BUFFER_TOO_SMALL;
    }
    if (result != PLANK_TRANSPORT_OK) {
        record_failure(transport, PLANK_VISION_LANE_AUDIO, result,
                       PLANK_VISION_FAILURE_NONE);
        return PLANK_VISION_TRANSPORT_ERROR;
    }
    packet->frame_samples = info.frame_samples;
    packet->missing_samples = info.missing_samples;
    packet->pts_48khz = info.pts;
    return PLANK_VISION_TRANSPORT_OK;
#endif
}

int32_t plank_vision_transport_audio_stats(
    PlankVisionTransport *transport, PlankVisionAudioStats *stats) {
#if !PLANK_NATIVE_TRANSPORT
    (void)transport; (void)stats;
    return PLANK_VISION_TRANSPORT_UNAVAILABLE;
#else
    if (transport == NULL || transport->endpoint == NULL || stats == NULL) {
        return PLANK_VISION_TRANSPORT_ERROR;
    }
    PlankTransportNativeStats native_stats = {0};
    native_stats.struct_size = sizeof(native_stats);
    if (plank_transport_native_endpoint_stats(
            transport->endpoint, &native_stats) != PLANK_TRANSPORT_OK) {
        return PLANK_VISION_TRANSPORT_ERROR;
    }
    stats->packets_received = native_stats.audio_packets_received;
    stats->bytes_received = native_stats.audio_bytes_received;
    stats->receive_drops = native_stats.audio_receive_drops;
    return PLANK_VISION_TRANSPORT_OK;
#endif
}

int32_t plank_vision_transport_set_video_bitrate(
    PlankVisionTransport *transport, uint32_t bitrate_kbps) {
#if !PLANK_NATIVE_TRANSPORT
    (void)transport; (void)bitrate_kbps;
    return PLANK_VISION_TRANSPORT_UNAVAILABLE;
#else
    if (transport == NULL || transport->endpoint == NULL ||
            bitrate_kbps < 500 || bitrate_kbps > 500000) {
        return PLANK_VISION_TRANSPORT_ERROR;
    }
    uint8_t packet[PLANK_TRANSPORT_CONTROL_MAX_PACKET_SIZE];
    size_t packet_size = 0;
    if (plank_transport_control_encode(
            PLANK_TRANSPORT_CONTROL_SET_VIDEO_BITRATE, &bitrate_kbps, 1,
            packet, sizeof(packet), &packet_size) != 0) {
        return PLANK_VISION_TRANSPORT_ERROR;
    }
    const int32_t result = plank_transport_native_data_send(
        transport->endpoint, packet, packet_size);
    if (result != PLANK_TRANSPORT_OK) {
        record_failure(transport, PLANK_VISION_LANE_DATA, result,
                       PLANK_VISION_FAILURE_NONE);
        return PLANK_VISION_TRANSPORT_ERROR;
    }
    return PLANK_VISION_TRANSPORT_OK;
#endif
}

int32_t plank_vision_transport_request_idr(PlankVisionTransport *transport) {
#if !PLANK_NATIVE_TRANSPORT
    (void)transport;
    return PLANK_VISION_TRANSPORT_UNAVAILABLE;
#else
    if (transport == NULL || transport->endpoint == NULL) {
        return PLANK_VISION_TRANSPORT_ERROR;
    }
    uint8_t packet[PLANK_TRANSPORT_CONTROL_MAX_PACKET_SIZE];
    size_t packet_size = 0;
    if (plank_transport_control_encode(
            PLANK_TRANSPORT_CONTROL_REQUEST_IDR, NULL, 0,
            packet, sizeof(packet), &packet_size) != 0) {
        return PLANK_VISION_TRANSPORT_ERROR;
    }
    const int32_t result = plank_transport_native_data_send(
        transport->endpoint, packet, packet_size);
    if (result != PLANK_TRANSPORT_OK) {
        record_failure(transport, PLANK_VISION_LANE_DATA, result,
                       PLANK_VISION_FAILURE_NONE);
        return PLANK_VISION_TRANSPORT_ERROR;
    }
    return PLANK_VISION_TRANSPORT_OK;
#endif
}

int32_t plank_vision_transport_send_mouse_position(
    PlankVisionTransport *transport,
    uint16_t x,
    uint16_t y,
    uint16_t maximum_x,
    uint16_t maximum_y) {
#if !PLANK_NATIVE_TRANSPORT
    (void)transport; (void)x; (void)y; (void)maximum_x; (void)maximum_y;
    return PLANK_VISION_TRANSPORT_UNAVAILABLE;
#else
    if (transport == NULL || transport->endpoint == NULL ||
            maximum_x == 0 || maximum_y == 0 ||
            x > maximum_x || y > maximum_y) {
        return PLANK_VISION_TRANSPORT_ERROR;
    }
    uint8_t payload[PLANK_TRANSPORT_INPUT_ABSOLUTE_MOUSE_SIZE];
    plank_transport_input_encode_absolute_mouse(
        payload, x, y, maximum_x, maximum_y);
    return finish_input_send(transport, plank_transport_native_input_send(
        transport->endpoint, PLANK_TRANSPORT_INPUT_ABSOLUTE_MOUSE,
        payload, sizeof(payload)));
#endif
}

int32_t plank_vision_transport_send_pen(
    PlankVisionTransport *transport, uint8_t event_type,
    float x, float y, float pressure_or_distance, uint8_t tilt, uint16_t rotation) {
#if !PLANK_NATIVE_TRANSPORT
    (void)transport; (void)event_type; (void)x; (void)y;
    (void)pressure_or_distance; (void)tilt; (void)rotation;
    return PLANK_VISION_TRANSPORT_UNAVAILABLE;
#else
    if (transport == NULL || transport->endpoint == NULL ||
        !(event_type <= 4 || event_type == 6) ||
        !isfinite(x) || !isfinite(y) || !isfinite(pressure_or_distance) ||
        x < 0 || x > 1 || y < 0 || y > 1 ||
        pressure_or_distance < 0 || pressure_or_distance > 1 ||
        !(tilt <= 90 || tilt == 255) || !(rotation < 360 || rotation == 65535)) {
        return PLANK_VISION_TRANSPORT_ERROR;
    }
    uint8_t payload[PLANK_TRANSPORT_INPUT_PEN_SIZE];
    plank_transport_input_encode_pen(payload, event_type, 1, 0, tilt, rotation,
                                     x, y, pressure_or_distance, 0, 0);
    return finish_input_send(transport, plank_transport_native_input_send(
        transport->endpoint, PLANK_TRANSPORT_INPUT_PEN, payload, sizeof(payload)));
#endif
}

int32_t plank_vision_transport_send_mouse_button(
    PlankVisionTransport *transport,
    uint8_t button,
    uint8_t pressed) {
#if !PLANK_NATIVE_TRANSPORT
    (void)transport; (void)button; (void)pressed;
    return PLANK_VISION_TRANSPORT_UNAVAILABLE;
#else
    if (transport == NULL || transport->endpoint == NULL || button == 0) {
        return PLANK_VISION_TRANSPORT_ERROR;
    }
    const uint8_t payload[PLANK_TRANSPORT_INPUT_MOUSE_BUTTON_SIZE] = {
        button,
        pressed ? PLANK_TRANSPORT_INPUT_ACTION_PRESS :
                  PLANK_TRANSPORT_INPUT_ACTION_RELEASE,
    };
    return finish_input_send(transport, plank_transport_native_input_send(
        transport->endpoint, PLANK_TRANSPORT_INPUT_MOUSE_BUTTON,
        payload, sizeof(payload)));
#endif
}

int32_t plank_vision_transport_send_scroll(
    PlankVisionTransport *transport,
    int16_t amount,
    uint8_t horizontal) {
#if !PLANK_NATIVE_TRANSPORT
    (void)transport; (void)amount; (void)horizontal;
    return PLANK_VISION_TRANSPORT_UNAVAILABLE;
#else
    if (transport == NULL || transport->endpoint == NULL || amount == 0) {
        return PLANK_VISION_TRANSPORT_ERROR;
    }
    uint8_t payload[PLANK_TRANSPORT_INPUT_SCROLL_SIZE];
    plank_transport_input_write_u16(payload, (uint16_t)amount);
    const uint8_t type = horizontal ?
        PLANK_TRANSPORT_INPUT_HORIZONTAL_SCROLL :
        PLANK_TRANSPORT_INPUT_VERTICAL_SCROLL;
    return finish_input_send(transport, plank_transport_native_input_send(
        transport->endpoint, type, payload, sizeof(payload)));
#endif
}

int32_t plank_vision_transport_send_key(
    PlankVisionTransport *transport,
    uint16_t key_code,
    uint8_t pressed,
    uint8_t modifiers) {
#if !PLANK_NATIVE_TRANSPORT
    (void)transport; (void)key_code; (void)pressed; (void)modifiers;
    return PLANK_VISION_TRANSPORT_UNAVAILABLE;
#else
    if (transport == NULL || transport->endpoint == NULL) {
        return PLANK_VISION_TRANSPORT_ERROR;
    }
    uint8_t payload[PLANK_TRANSPORT_INPUT_KEYBOARD_SIZE];
    plank_transport_input_write_u16(payload, key_code);
    payload[2] = pressed ? PLANK_TRANSPORT_INPUT_ACTION_PRESS :
                           PLANK_TRANSPORT_INPUT_ACTION_RELEASE;
    payload[3] = modifiers;
    payload[4] = 0;
    return finish_input_send(transport, plank_transport_native_input_send(
        transport->endpoint, PLANK_TRANSPORT_INPUT_KEYBOARD,
        payload, sizeof(payload)));
#endif
}

int32_t plank_vision_transport_send_utf8(
    PlankVisionTransport *transport,
    const uint8_t *text,
    size_t text_size) {
#if !PLANK_NATIVE_TRANSPORT
    (void)transport; (void)text; (void)text_size;
    return PLANK_VISION_TRANSPORT_UNAVAILABLE;
#else
    if (transport == NULL || transport->endpoint == NULL || text == NULL ||
            text_size == 0 || text_size > PLANK_TRANSPORT_INPUT_MAX_PAYLOAD_SIZE) {
        return PLANK_VISION_TRANSPORT_ERROR;
    }
    return finish_input_send(transport, plank_transport_native_input_send(
        transport->endpoint, PLANK_TRANSPORT_INPUT_UTF8_TEXT,
        text, text_size));
#endif
}

static uint16_t read_le16(const uint8_t *input) {
    return (uint16_t)(input[0] | ((uint16_t)input[1] << 8));
}

static uint32_t read_le32(const uint8_t *input) {
    return (uint32_t)input[0] |
           ((uint32_t)input[1] << 8) |
           ((uint32_t)input[2] << 16) |
           ((uint32_t)input[3] << 24);
}

static uint64_t read_le64(const uint8_t *input) {
    return (uint64_t)read_le32(input) |
           ((uint64_t)read_le32(input + 4) << 32);
}

int32_t plank_vision_transport_send_raw_hid(
    PlankVisionTransport *transport,
    const uint8_t *frame,
    size_t frame_size) {
#if !PLANK_NATIVE_TRANSPORT
    (void)transport; (void)frame; (void)frame_size;
    return PLANK_VISION_TRANSPORT_UNAVAILABLE;
#else
    if (transport == NULL || transport->endpoint == NULL ||
            !plank_vision_raw_hid_frame_valid(
                frame, frame_size, PLANK_VISION_RAW_HID_TO_HOST)) {
        return PLANK_VISION_TRANSPORT_ERROR;
    }
    if (transport->raw_hid_session_state == 2) return PLANK_VISION_TRANSPORT_ERROR;
    if (transport->raw_hid_session_state == 0 &&
            read_le16(frame + 6) == PLANK_RAW_HID_DEVICE) {
        const uint16_t generation = read_le16(frame + 10);
        uint8_t detach[sizeof(PLANK_RAW_HID_WIRE_HEADER)];
        if (frame_size != sizeof(PLANK_RAW_HID_WIRE_HEADER) + sizeof(PLANK_RAW_HID_DEVICE_MESSAGE) ||
                read_le16(frame + sizeof(PLANK_RAW_HID_WIRE_HEADER)) == 0 ||
                read_le16(frame + sizeof(PLANK_RAW_HID_WIRE_HEADER)) > PLANK_RAW_HID_MAX_INTERFACES ||
                !plank_vision_raw_hid_make_detach(generation, detach, sizeof(detach))) {
            return PLANK_VISION_TRANSPORT_ERROR;
        }
        // DEVICE establishes the generation required by the existing Host's
        // DETACH handler, without creating interfaces. DETACH then destroys any
        // retained group; the identical DEVICE starts the real attachment.
        // Descriptors, reports and replies remain untouched and ordered on the
        // same input lane. Do this once per desktop endpoint, never on focus
        // suspension or a temporary Relay reconnect within that endpoint.
        const uint8_t *frames[] = {frame, detach, frame};
        const size_t sizes[] = {frame_size, sizeof(detach), frame_size};
        for (size_t index = 0; index < 3; ++index) {
            const int32_t result = finish_input_send(transport, plank_transport_native_input_send(
                transport->endpoint, PLANK_TRANSPORT_INPUT_RAW_HID_WACOM,
                frames[index], sizes[index]));
            if (result != PLANK_VISION_TRANSPORT_OK) {
                transport->raw_hid_session_state = 2;
                return result;
            }
        }
        transport->raw_hid_session_state = 1;
        fprintf(stderr, "PLANK tablet session reset requested: generation=%u reason=new-desktop-session\n",
                (unsigned)generation);
        return PLANK_VISION_TRANSPORT_OK;
    }
    return finish_input_send(transport, plank_transport_native_input_send(
        transport->endpoint, PLANK_TRANSPORT_INPUT_RAW_HID_WACOM,
        frame, frame_size));
#endif
}

int32_t plank_vision_transport_receive_data_event(
    PlankVisionTransport *transport,
    PlankVisionCursorEvent *cursor_event,
    uint8_t *chunk,
    size_t chunk_capacity,
    size_t *chunk_size,
    uint32_t timeout_ms) {
#if !PLANK_NATIVE_TRANSPORT
    (void)transport; (void)cursor_event; (void)chunk; (void)chunk_capacity;
    (void)chunk_size; (void)timeout_ms;
    return PLANK_VISION_TRANSPORT_UNAVAILABLE;
#else
    if (transport == NULL || transport->endpoint == NULL || cursor_event == NULL ||
            chunk == NULL || chunk_size == NULL) {
        return PLANK_VISION_TRANSPORT_ERROR;
    }
    *chunk_size = 0;
    uint8_t packet[PLANK_TRANSPORT_EVENT_MAX_PACKET_SIZE];
    size_t packet_size = 0;
    const int32_t result = plank_transport_native_data_receive(
        transport->endpoint, packet, sizeof(packet), &packet_size, timeout_ms);
    if (result == PLANK_TRANSPORT_TIMEOUT) return PLANK_VISION_TRANSPORT_TIMEOUT;
    if (result != PLANK_TRANSPORT_OK) {
        record_failure(transport, PLANK_VISION_LANE_DATA, result,
                       PLANK_VISION_FAILURE_NONE);
        return PLANK_VISION_TRANSPORT_ERROR;
    }

    memset(cursor_event, 0, sizeof(*cursor_event));
    // Native control records share the reliable data lane. Only the bitrate
    // acknowledgement is handled here; any other control record is still
    // rejected as before.
    if (packet_size >= sizeof(uint32_t) &&
            plank_transport_control_read_u32(packet) == PLANK_TRANSPORT_CONTROL_MAGIC) {
        PlankTransportControlPacket control = {0};
        if (plank_transport_control_decode(packet, packet_size, &control) != 0 ||
                control.type != PLANK_TRANSPORT_CONTROL_VIDEO_BITRATE_APPLIED ||
                control.payload_size != 3 * sizeof(uint32_t)) {
            return invalid_data_event(transport);
        }
        cursor_event->type = PLANK_VISION_BITRATE_APPLIED;
        cursor_event->bitrate_requested_kbps = plank_transport_control_read_u32(control.payload);
        cursor_event->bitrate_applied_kbps = plank_transport_control_read_u32(control.payload + 4);
        cursor_event->bitrate_peak_kbps = plank_transport_control_read_u32(control.payload + 8);
        return PLANK_VISION_TRANSPORT_OK;
    }

    PlankTransportEventPacket event = {0};
    if (plank_transport_event_decode(packet, packet_size, &event) != 0) {
        return invalid_data_event(transport);
    }
    const uint8_t *payload = event.payload;
    if (event.type == PLANK_TRANSPORT_EVENT_RAW_HID_WACOM) {
        if (!plank_vision_raw_hid_frame_valid(
                payload, event.payload_size, PLANK_VISION_RAW_HID_FROM_HOST) ||
                event.payload_size > chunk_capacity) {
            return invalid_data_event(transport);
        }
        cursor_event->type = PLANK_VISION_RAW_HID_EVENT;
        memcpy(chunk, payload, event.payload_size);
        *chunk_size = event.payload_size;
        return PLANK_VISION_TRANSPORT_OK;
    }
    if (event.type != PLANK_TRANSPORT_EVENT_CURSOR_POSITION &&
            event.type != PLANK_TRANSPORT_EVENT_CURSOR_SHAPE) {
        return PLANK_VISION_TRANSPORT_DATA_IGNORED;
    }
    if (event.type == PLANK_TRANSPORT_EVENT_CURSOR_POSITION) {
        if (event.payload_size != 32 || read_le32(payload) != 0x504c4350u ||
                read_le16(payload + 4) != 1 || read_le16(payload + 6) != 0) {
            return invalid_data_event(transport);
        }
        cursor_event->type = PLANK_VISION_CURSOR_POSITION;
        cursor_event->sequence = read_le64(payload + 8);
        cursor_event->x = read_le32(payload + 16);
        cursor_event->y = read_le32(payload + 20);
        cursor_event->frame_width = read_le32(payload + 24);
        cursor_event->frame_height = read_le32(payload + 28);
        if (cursor_event->sequence == 0 || cursor_event->frame_width == 0 ||
                cursor_event->frame_height == 0 ||
                cursor_event->x >= cursor_event->frame_width ||
                cursor_event->y >= cursor_event->frame_height) {
            return invalid_data_event(transport);
        }
        return PLANK_VISION_TRANSPORT_OK;
    }

    // The Host sends a bounded ARGB8888 image in ordered 48 KiB chunks.
    // Validate every peer supplied size before copying into the caller's buffer.
    if (event.payload_size < 52 || read_le32(payload) != 0x504c4352u ||
            read_le16(payload + 4) != 1 || read_le16(payload + 6) != 1) {
        return invalid_data_event(transport);
    }
    cursor_event->type = PLANK_VISION_CURSOR_SHAPE;
    cursor_event->flags = read_le32(payload + 8);
    cursor_event->generation = read_le64(payload + 12);
    cursor_event->width = read_le32(payload + 20);
    cursor_event->height = read_le32(payload + 24);
    cursor_event->hotspot_x = read_le32(payload + 28);
    cursor_event->hotspot_y = read_le32(payload + 32);
    cursor_event->image_size = read_le32(payload + 36);
    cursor_event->chunk_offset = read_le32(payload + 40);
    const uint32_t size = read_le32(payload + 44);
    // The packed header has 48 bytes; no payload beyond its declared chunk.
    if ((cursor_event->flags & ~7u) != 0 ||
            cursor_event->width == 0 || cursor_event->height == 0 ||
            cursor_event->width > 512 || cursor_event->height > 512 ||
            cursor_event->hotspot_x >= cursor_event->width ||
            cursor_event->hotspot_y >= cursor_event->height ||
            cursor_event->image_size !=
                cursor_event->width * cursor_event->height * 4u ||
            size == 0 || size > PLANK_VISION_CURSOR_MAX_CHUNK_SIZE ||
            size > chunk_capacity || event.payload_size != 48u + size ||
            cursor_event->chunk_offset > cursor_event->image_size ||
            size > cursor_event->image_size - cursor_event->chunk_offset) {
        return invalid_data_event(transport);
    }
    memcpy(chunk, payload + 48, size);
    *chunk_size = size;
    return PLANK_VISION_TRANSPORT_OK;
#endif
}

int32_t plank_vision_transport_first_failure(
    PlankVisionTransport *transport,
    PlankVisionTransportFailure *failure,
    char *reason,
    size_t reason_capacity) {
    if (reason != NULL && reason_capacity > 0) reason[0] = '\0';
    if (transport == NULL || failure == NULL) return PLANK_VISION_TRANSPORT_ERROR;
    const uint64_t packed = atomic_load(&transport->first_failure);
    failure->kind = (uint32_t)(packed & 0xffu);
    failure->lane = (uint32_t)((packed >> 8) & 0xffu);
    failure->endpoint_state = (uint32_t)((packed >> 16) & 0xffu);
    failure->native_result = (int32_t)(uint32_t)(packed >> 32);
#if PLANK_NATIVE_TRANSPORT
    if (transport->endpoint != NULL && reason != NULL && reason_capacity > 0) {
        plank_transport_native_endpoint_last_error(
            transport->endpoint, reason, reason_capacity);
    }
#endif
    return PLANK_VISION_TRANSPORT_OK;
}

void plank_vision_transport_disconnect(PlankVisionTransport *transport) {
    if (transport == NULL) return;
#if PLANK_NATIVE_TRANSPORT
    if (transport->endpoint != NULL) {
        uint8_t packet[PLANK_TRANSPORT_CONTROL_MAX_PACKET_SIZE];
        size_t packet_size = 0;
        if (plank_transport_control_encode(
                PLANK_TRANSPORT_CONTROL_CLIENT_DISCONNECT, NULL, 0,
                packet, sizeof(packet), &packet_size) == 0) {
            if (plank_transport_native_data_send(
                    transport->endpoint, packet, packet_size) == PLANK_TRANSPORT_OK) {
                // data_send only queues the reliable control packet. Stopping
                // the worker immediately can discard it, leaving the Host's
                // reservation active until its idle timeout. Let the Host
                // process the disconnect and close its endpoint, with a hard
                // bound for an unreachable peer.
                for (unsigned int attempt = 0; attempt < 150; ++attempt) {
                    const uint32_t state = plank_transport_native_endpoint_state(
                        transport->endpoint);
                    if (state != PLANK_TRANSPORT_STATE_READY &&
                            state != PLANK_TRANSPORT_STATE_SETUP_READY) {
                        break;
                    }
                    usleep(10000);
                }
            }
        }
        plank_transport_native_endpoint_stop(transport->endpoint);
        plank_transport_native_endpoint_destroy(transport->endpoint);
    }
#endif
    free(transport);
}
