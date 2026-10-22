#ifndef PLANK_TRANSPORT_BRIDGE_H
#define PLANK_TRANSPORT_BRIDGE_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct PlankVisionTransport PlankVisionTransport;

typedef struct PlankVisionVideoFrame {
    uint32_t codec;
    uint32_t flags;
    uint64_t frame_number;
    uint64_t pts_90khz;
    uint16_t host_processing_latency;
} PlankVisionVideoFrame;

typedef struct PlankVisionVideoStats {
    uint64_t frames_received;
    uint64_t receive_drops;
    uint64_t fec_symbols_unrecovered;
    // Input packets the native endpoint has handed to QUIC. Comparing this
    // with the Client's accepted count gives the native send backlog.
    uint64_t input_packets_sent;
    uint64_t quic_rtt_us;
    uint64_t quic_packets_lost;
    uint64_t kyproto_packets_dropped;
} PlankVisionVideoStats;

typedef struct PlankVisionAudioPacket {
    // Samples per Opus frame announced by the Host's codec header.
    uint16_t frame_samples;
    // Non-zero for a transport hole: the packet has no payload and this many
    // samples were lost. The receiver conceals them; it never invents data.
    uint32_t missing_samples;
    uint64_t pts_48khz;
} PlankVisionAudioPacket;

typedef struct PlankVisionAudioStats {
    uint64_t packets_received;
    uint64_t bytes_received;
    // Packets the transport's bounded receive queue evicted before the
    // Client claimed them.
    uint64_t receive_drops;
} PlankVisionAudioStats;

// The first terminal transport result is preserved for diagnostics. The
// public calls keep their existing return values.
enum {
    PLANK_VISION_FAILURE_NONE = 0,
    // The endpoint failed: the peer closed or the connection broke.
    PLANK_VISION_FAILURE_TERMINATED = 1,
    // The endpoint was stopped locally.
    PLANK_VISION_FAILURE_CANCELLED = 2,
    PLANK_VISION_FAILURE_INVALID_PAYLOAD = 3,
    PLANK_VISION_FAILURE_BUFFER_LIMIT = 4,
    // The native input send queue was full.
    PLANK_VISION_FAILURE_QUEUE_FULL = 5,
    // Invalid argument or a contained native panic.
    PLANK_VISION_FAILURE_INTERNAL = 6,
};

enum {
    PLANK_VISION_LANE_VIDEO = 1,
    PLANK_VISION_LANE_INPUT = 2,
    PLANK_VISION_LANE_DATA = 3,
    PLANK_VISION_LANE_AUDIO = 4,
};

typedef struct PlankVisionTransportFailure {
    uint32_t kind;
    uint32_t lane;
    int32_t native_result;
    uint32_t endpoint_state;
} PlankVisionTransportFailure;

typedef struct PlankVisionCursorEvent {
    uint16_t type;
    uint32_t x;
    uint32_t y;
    uint32_t frame_width;
    uint32_t frame_height;
    uint64_t sequence;
    uint64_t generation;
    uint32_t flags;
    uint32_t width;
    uint32_t height;
    uint32_t hotspot_x;
    uint32_t hotspot_y;
    uint32_t image_size;
    uint32_t chunk_offset;
    // PLANK_VISION_BITRATE_APPLIED: the Host's acknowledgement of a live
    // target: as requested, after its ceiling, and the resulting peak.
    uint32_t bitrate_requested_kbps;
    uint32_t bitrate_applied_kbps;
    uint32_t bitrate_peak_kbps;
} PlankVisionCursorEvent;

enum {
    PLANK_VISION_RAW_HID_EVENT = 2,
    PLANK_VISION_CURSOR_SHAPE = 3,
    PLANK_VISION_CURSOR_POSITION = 4,
    PLANK_VISION_BITRATE_APPLIED = 5,
    PLANK_VISION_CURSOR_VISIBLE = 1,
    PLANK_VISION_CURSOR_FIRST_CHUNK = 2,
    PLANK_VISION_CURSOR_LAST_CHUNK = 4,
    PLANK_VISION_CURSOR_MAX_CHUNK_SIZE = 48 * 1024,
};

enum {
    PLANK_VISION_TRANSPORT_OK = 0,
    PLANK_VISION_TRANSPORT_TIMEOUT = 1,
    PLANK_VISION_TRANSPORT_BUFFER_TOO_SMALL = 2,
    // A valid data event was consumed but has no presentation handler.
    PLANK_VISION_TRANSPORT_DATA_IGNORED = 3,
    PLANK_VISION_TRANSPORT_ERROR = -1,
    PLANK_VISION_TRANSPORT_UNAVAILABLE = -2,
};

PlankVisionTransport *plank_vision_transport_connect(
    const char *remote_host,
    uint16_t remote_port,
    const char *certificate_sha256,
    const char *session_token,
    uint32_t maximum_udp_payload,
    char *error,
    size_t error_capacity);

int32_t plank_vision_transport_negotiate(
    PlankVisionTransport *transport,
    const uint8_t *request_json,
    size_t request_size,
    uint8_t *response_json,
    size_t response_capacity,
    size_t *response_size,
    char *error,
    size_t error_capacity);

int32_t plank_vision_transport_receive_video(
    PlankVisionTransport *transport,
    PlankVisionVideoFrame *frame,
    uint8_t *payload,
    size_t payload_capacity,
    size_t *payload_size,
    uint32_t timeout_ms);

int32_t plank_vision_transport_video_stats(
    PlankVisionTransport *transport, PlankVisionVideoStats *stats);

// Bounded wait for one Host audio packet or hole. Safe to call from a
// dedicated thread concurrently with the video, data and input lanes. A hole
// returns OK with a zero payload size and non-zero missing_samples.
int32_t plank_vision_transport_receive_audio(
    PlankVisionTransport *transport,
    PlankVisionAudioPacket *packet,
    uint8_t *payload,
    size_t payload_capacity,
    size_t *payload_size,
    uint32_t timeout_ms);

int32_t plank_vision_transport_audio_stats(
    PlankVisionTransport *transport, PlankVisionAudioStats *stats);

int32_t plank_vision_transport_request_idr(PlankVisionTransport *transport);

// Live encoder target for the running session (native control
// SET_VIDEO_BITRATE). Only for Hosts advertising feature 0x08. The Host ends
// the session for a value outside 500-500000 kbps, so those are refused here.
// The Host answers with a PLANK_VISION_BITRATE_APPLIED data event.
int32_t plank_vision_transport_set_video_bitrate(
    PlankVisionTransport *transport, uint32_t bitrate_kbps);

int32_t plank_vision_transport_send_mouse_position(
    PlankVisionTransport *transport,
    uint16_t x,
    uint16_t y,
    uint16_t maximum_x,
    uint16_t maximum_y);

// Existing normalized pen type 7; always pen tool, no synthetic barrel buttons.
int32_t plank_vision_transport_send_pen(
    PlankVisionTransport *transport, uint8_t event_type,
    float x, float y, float pressure_or_distance, uint8_t tilt, uint16_t rotation);

int32_t plank_vision_transport_send_mouse_button(
    PlankVisionTransport *transport,
    uint8_t button,
    uint8_t pressed);

int32_t plank_vision_transport_send_scroll(
    PlankVisionTransport *transport,
    int16_t amount,
    uint8_t horizontal);

int32_t plank_vision_transport_send_key(
    PlankVisionTransport *transport,
    uint16_t key_code,
    uint8_t pressed,
    uint8_t modifiers);

int32_t plank_vision_transport_send_utf8(
    PlankVisionTransport *transport,
    const uint8_t *text,
    size_t text_size);

int32_t plank_vision_transport_send_raw_hid(
    PlankVisionTransport *transport,
    const uint8_t *frame,
    size_t frame_size);

// Receives cursor updates and Host raw-HID control frames from the data lane.
int32_t plank_vision_transport_receive_data_event(
    PlankVisionTransport *transport,
    PlankVisionCursorEvent *cursor_event,
    uint8_t *chunk,
    size_t chunk_capacity,
    size_t *chunk_size,
    uint32_t timeout_ms);

// Copies the first recorded terminal failure (kind NONE when there is none)
// and the native endpoint's own error text, which carries any close reason.
int32_t plank_vision_transport_first_failure(
    PlankVisionTransport *transport,
    PlankVisionTransportFailure *failure,
    char *reason,
    size_t reason_capacity);

void plank_vision_transport_disconnect(PlankVisionTransport *transport);

#ifdef __cplusplus
}
#endif

#endif
