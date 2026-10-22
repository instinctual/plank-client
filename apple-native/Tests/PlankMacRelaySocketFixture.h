#pragma once
#include "../Bridge/PlankMacRelay.h"
#ifdef __cplusplus
extern "C" {
#endif
typedef struct MacRelaySocketClient MacRelaySocketClient;
// Test-only real Noise client plus the existing fake physical HID worker.
MacRelaySocketClient* mac_relay_socket_client(PlankMacRelayStore*);
void mac_relay_socket_destroy(MacRelaySocketClient*);
int mac_relay_socket_start(MacRelaySocketClient*,uint8_t*,size_t,size_t*);
int mac_relay_socket_receive(MacRelaySocketClient*,const uint8_t*,size_t,uint8_t*,size_t,size_t*);
bool mac_relay_socket_ready(MacRelaySocketClient*);
int mac_relay_socket_session_ready(MacRelaySocketClient*,uint8_t*,size_t,size_t*);
int mac_relay_socket_ping(MacRelaySocketClient*,uint8_t*,size_t,size_t*);
bool mac_relay_socket_burst(unsigned);
unsigned mac_relay_socket_reports(MacRelaySocketClient*);
bool mac_relay_socket_pong(MacRelaySocketClient*);
#ifdef __cplusplus
}
#endif
