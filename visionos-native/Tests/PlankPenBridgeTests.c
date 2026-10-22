// Exercise the production type-7 bridge and its canonical payload encoder.
#include "../Bridge/PlankTransportBridge.c"
#include <assert.h>
static unsigned sends;
static uint8_t sent[32];
static int32_t result = PLANK_TRANSPORT_OK;
int32_t plank_transport_native_input_send(PlankTransportNativeEndpoint *endpoint,
        uint8_t type, const uint8_t *payload, size_t size) {
    assert(endpoint && type == 7 && size == 32);
    memcpy(sent, payload, size); ++sends; return result;
}
uint32_t plank_transport_native_endpoint_state(const PlankTransportNativeEndpoint *endpoint) {
    (void)endpoint; return PLANK_TRANSPORT_STATE_READY;
}
int main(void) {
    PlankVisionTransport transport = { .endpoint = (PlankTransportNativeEndpoint *)1 };
    assert(plank_vision_transport_send_pen(&transport, 1, .5f, 1, .25f, 45, 270) == PLANK_VISION_TRANSPORT_OK);
    const uint8_t expected[32] = {1,1,0,45,1,14,0,0, 0x3f,0,0,0, 0x3f,0x80,0,0, 0x3e,0x80,0,0};
    assert(sends == 1 && memcmp(sent, expected, 32) == 0);
    assert(plank_vision_transport_send_pen(&transport, 4, .5f, 1, 0, 255, 65535) == PLANK_VISION_TRANSPORT_OK);
    assert(plank_vision_transport_send_pen(&transport, 6, .5f, 1, 0, 0, 0) == PLANK_VISION_TRANSPORT_OK);
    assert(plank_vision_transport_send_pen(&transport, 8, 0, 0, 0, 0, 0) == PLANK_VISION_TRANSPORT_ERROR);
    assert(plank_vision_transport_send_pen(&transport, 1, NAN, 0, 0, 0, 0) == PLANK_VISION_TRANSPORT_ERROR);
    assert(plank_vision_transport_send_pen(&transport, 1, 0, 0, INFINITY, 0, 0) == PLANK_VISION_TRANSPORT_ERROR);
    assert(plank_vision_transport_send_pen(&transport, 1, -1, 0, 0, 0, 0) == PLANK_VISION_TRANSPORT_ERROR);
    assert(plank_vision_transport_send_pen(&transport, 1, 0, 0, 0, 91, 0) == PLANK_VISION_TRANSPORT_ERROR);
    assert(plank_vision_transport_send_pen(&transport, 1, 0, 0, 0, 0, 360) == PLANK_VISION_TRANSPORT_ERROR);
    assert(sends == 3);
    result = PLANK_TRANSPORT_TIMEOUT;
    assert(plank_vision_transport_send_pen(&transport, 1, 0, 0, 1, 0, 0) == PLANK_VISION_TRANSPORT_ERROR);
    assert((atomic_load(&transport.first_failure) & 0xff) == PLANK_VISION_FAILURE_QUEUE_FULL);
    puts("Normalized pen bridge wire/validation/failure checks passed");
}
