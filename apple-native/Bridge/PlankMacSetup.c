#include "PlankMacSetupNamespace.h"
#include "ble_lab.h"
#include "PlankMacSetup.h"
void* plank_mac_setup_create(const char* dir) {
    PltrBleLab* lab=pltr_ble_lab_create(dir);
    if(lab && pltr_ble_lab_transport(lab,2)) { pltr_ble_lab_destroy(lab);return NULL; }
    if(lab) pltr_ble_lab_allow_enrollment(lab,1);
    return lab;
}
void plank_mac_setup_destroy(void* p){pltr_ble_lab_destroy(p);}
int plank_mac_setup_key(void* p,uint8_t k[32]){return pltr_ble_lab_public_key(p,k);}
void plank_mac_setup_disconnect(void* p,uint64_t now){pltr_ble_lab_disconnect(p,now);pltr_ble_lab_allow_enrollment(p,1);}
int plank_mac_setup_receive(void* p,const uint8_t* b,size_t n,size_t* c,uint64_t t,uint8_t* o,size_t cap,size_t* w){return pltr_ble_lab_receive(p,b,n,c,t,o,cap,w);}
int plank_mac_setup_tick(void* p,uint64_t t,uint8_t* o,size_t cap,size_t* w){return pltr_ble_lab_tick(p,t,o,cap,w);}
int plank_mac_setup_request(void* p,uint8_t* b,size_t cap){return pltr_ble_lab_take_management(p,b,cap);}
int plank_mac_setup_reply(void* p,const uint8_t* b,size_t n,uint8_t* o,size_t cap,size_t* w){return pltr_ble_lab_management_reply(p,b,n,o,cap,w);}
int plank_mac_setup_authorized(void* p){return pltr_ble_lab_management_authorized(p);}
int plank_mac_setup_pending(void* p){return pltr_ble_lab_enrolling(p);}
int plank_mac_setup_accept(void* p){return pltr_ble_lab_finish_enrollment(p);}
