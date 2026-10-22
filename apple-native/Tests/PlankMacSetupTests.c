#include "PlankMacSetupNamespace.h"
#include "client_link.h"
#include "protocol.h"
#include <unistd.h>
#include "PlankMacSetup.h"
#include <sodium.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
static unsigned checks;
#define CHECK(x) do { ++checks; if(!(x)){fprintf(stderr,"FAIL %d: %s\n",__LINE__,#x);exit(1);} } while(0)
static void handshake(void* lab,PltrClientLink* client) {
    uint8_t request[8448],output[8448],reply[8448],payload[8192];size_t size=0,n=0,consumed=0,p=0;uint16_t type=0;
    CHECK(pltr_client_link_start(client,request,sizeof(request),&size)==0);
    size_t at=0;
    while(at<size) {
        CHECK(plank_mac_setup_receive(lab,request+at,size-at,&consumed,1001,output,sizeof(output),&n)>=0);CHECK(consumed>0);at+=consumed;
        size_t outAt=0,back=0;
        while(outAt<n) {
            size_t took=0,wrote=0;
            CHECK(pltr_client_link_receive(client,output+outAt,n-outAt,&took,reply+back,sizeof(reply)-back,&wrote,&type,payload,sizeof(payload),&p)>=0);
            CHECK(took>0);outAt+=took;back+=wrote;
        }
        if(back) { size_t used=0,wrote=0;CHECK(plank_mac_setup_receive(lab,reply,back,&used,1002,output,sizeof(output),&wrote)>=0);CHECK(used==back && wrote==0); }
    }
    CHECK(pltr_client_link_peer_version(client)!=NULL);
}
int main(void) {
    CHECK(sodium_init()>=0);char dir[]="/tmp/plank-mac-setup-test-XXXXXX";CHECK(mkdtemp(dir));
    void* lab=plank_mac_setup_create(dir);CHECK(lab);uint8_t key[32],pub[32],priv[32];CHECK(plank_mac_setup_key(lab,key)==0);crypto_box_keypair(pub,priv);
    PltrClientLink* client=pltr_client_link_create(priv,key,2);CHECK(client);CHECK(pltr_client_link_enable_tablet_management(client)==0);
    handshake(lab,client);CHECK(plank_mac_setup_pending(lab)==1);CHECK(plank_mac_setup_authorized(lab)==0);
    const uint8_t json[]="{\"version\":1,\"id\":1,\"op\":\"status\"}";
    uint8_t output[8448],request[4096],reply[8448],payload[8192];size_t n=0,used=0,written=0,p=0;uint16_t type=0;
    CHECK(pltr_client_link_send(client,PLTR_TABLET_REQUEST,json,sizeof(json)-1,output,sizeof(output),&n)==0);
    CHECK(plank_mac_setup_receive(lab,output,n,&used,1003,reply,sizeof(reply),&written)>=0);CHECK(used==n);
    CHECK(plank_mac_setup_request(lab,request,sizeof(request))==sizeof(json)-1);CHECK(memcmp(request,json,sizeof(json)-1)==0);
    CHECK(plank_mac_setup_authorized(lab)==0); // requesting is not consent
    CHECK(plank_mac_setup_accept(lab)==0);CHECK(plank_mac_setup_authorized(lab)==1);CHECK(plank_mac_setup_pending(lab)==0);
    CHECK(plank_mac_setup_reply(lab,json,sizeof(json)-1,output,sizeof(output),&n)==0);
    CHECK(pltr_client_link_receive(client,output,n,&used,reply,sizeof(reply),&written,&type,payload,sizeof(payload),&p)==1);
    CHECK(type==PLTR_TABLET_RESPONSE && p==sizeof(json)-1 && memcmp(json,payload,p)==0);
    plank_mac_setup_disconnect(lab,1004);pltr_client_link_destroy(client);plank_mac_setup_destroy(lab);
    lab=plank_mac_setup_create(dir);CHECK(lab);uint8_t persisted[32];CHECK(plank_mac_setup_key(lab,persisted)==0);CHECK(memcmp(persisted,key,32)==0);
    client=pltr_client_link_create(priv,key,2);CHECK(client);CHECK(pltr_client_link_enable_tablet_management(client)==0);handshake(lab,client);
    CHECK(plank_mac_setup_authorized(lab)==1 && plank_mac_setup_pending(lab)==0);
    plank_mac_setup_disconnect(lab,1005);pltr_client_link_destroy(client);
    uint8_t unknownPublic[32],unknownPrivate[32];crypto_box_keypair(unknownPublic,unknownPrivate);
    client=pltr_client_link_create(unknownPrivate,key,2);CHECK(client);CHECK(pltr_client_link_start(client,output,sizeof(output),&n)==0);
    size_t offset=0;int failed=0;
    while(offset<n) { int result=plank_mac_setup_receive(lab,output+offset,n-offset,&used,1006,reply,sizeof(reply),&written);
        if(result<0){failed=1;break;}CHECK(used>0);offset+=used; }
    CHECK(failed);CHECK(plank_mac_setup_authorized(lab)==0);pltr_client_link_destroy(client);plank_mac_setup_destroy(lab);
    printf("Mac Setup authenticated local consent, durable approval, raw management parity and unknown-peer refusal: %u checks passed\n",checks);
}
