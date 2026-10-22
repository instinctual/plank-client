#pragma once
#include <stdint.h>
#include <stddef.h>
#ifdef __cplusplus
extern "C" {
#endif
void* plank_mac_setup_create(const char* directory);
void plank_mac_setup_destroy(void*);
int plank_mac_setup_key(void*,uint8_t[32]);
void plank_mac_setup_disconnect(void*,uint64_t);
int plank_mac_setup_receive(void*,const uint8_t*,size_t,size_t*,uint64_t,uint8_t*,size_t,size_t*);
int plank_mac_setup_tick(void*,uint64_t,uint8_t*,size_t,size_t*);
int plank_mac_setup_request(void*,uint8_t*,size_t);
int plank_mac_setup_reply(void*,const uint8_t*,size_t,uint8_t*,size_t,size_t*);
int plank_mac_setup_authorized(void*);
int plank_mac_setup_pending(void*);
int plank_mac_setup_accept(void*);
#ifdef __cplusplus
}
#endif
