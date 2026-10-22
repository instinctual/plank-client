#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#ifdef __cplusplus
extern "C" {
#endif
typedef struct PlankMacRelayStore PlankMacRelayStore;
typedef struct PlankMacRelayConnection PlankMacRelayConnection;
// Owned 0700 directory; one durable identity/approval store, never a Host key.
PlankMacRelayStore* plank_mac_relay_store_create(const char* directory);
void plank_mac_relay_store_destroy(PlankMacRelayStore*);
bool plank_mac_relay_public_key(PlankMacRelayStore*, uint8_t key[32]);
// Create on the UI thread; permission is requested only by the local Share action.
// Capture stays inactive until approved Noise + SESSION_READY + active=true.
PlankMacRelayConnection* plank_mac_relay_connection_create(PlankMacRelayStore*);
void plank_mac_relay_connection_destroy(PlankMacRelayConnection*);
// One network executor owns receive/next. 1 = output, 0 = none, -1 = close.
int plank_mac_relay_receive(PlankMacRelayConnection*, const uint8_t*, size_t,
    size_t* consumed, uint8_t* output, size_t capacity, size_t* written);
int plank_mac_relay_next(PlankMacRelayConnection*, uint8_t* output, size_t capacity, size_t* written);
void plank_mac_relay_request_capture_permission(void);
unsigned plank_mac_relay_tablet_state(PlankMacRelayConnection*);
bool plank_mac_relay_ready(PlankMacRelayConnection*);
// These are local calls from an authenticated, locally approved Setup session.
bool plank_mac_relay_grant(PlankMacRelayStore*, bool cancel, const uint8_t id[16],
    const uint8_t client[32], const uint8_t target[32], uint64_t now_ms);
typedef struct PlankMacRelayEnrollment PlankMacRelayEnrollment;
PlankMacRelayEnrollment* plank_mac_relay_enrollment_create(PlankMacRelayStore*, uint64_t now_ms);
void plank_mac_relay_enrollment_destroy(PlankMacRelayEnrollment*);
// 0 incomplete, 1 reply, 2 durable enrollment confirmed, -1 refuse.
int plank_mac_relay_enrollment_receive(PlankMacRelayEnrollment*, const uint8_t*,size_t,
    size_t* consumed,uint8_t*,size_t,size_t* written,uint64_t now_ms);
#ifdef __cplusplus
}
#endif
