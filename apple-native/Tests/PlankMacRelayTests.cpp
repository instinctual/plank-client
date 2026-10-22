#include "PlankMacRelay.h"
#include "macrawwacom.h"
#include "macwacomcapturelease.h"
#include "macrawwacomlogic.h"
#include "link.h"
#include "client_enrollment.h"
#include <sys/wait.h>
#include <sodium.h>
#include <array>
#include <cstdlib>
#include <cstdio>
#include <cstring>
#include <vector>
#include <sys/stat.h>
#include <unistd.h>
#define CHECK(x) do { ++checks; if (!(x)) { std::fprintf(stderr,"FAIL line %d: %s\n",__LINE__,#x); std::exit(1); } } while(0)
static unsigned checks=0;
static MacRawWacomInput::SendFrame sender;
static MacRawWacomInput::GenerationProvider generations;
static unsigned starts=0,stops=0,controls=0;
class MacRawWacomInput::Impl {};
MacRawWacomInput::MacRawWacomInput(std::function<void()>,SendFrame s,GenerationProvider g,bool) { sender=std::move(s); generations=std::move(g); }
MacRawWacomInput::~MacRawWacomInput() = default;
void MacRawWacomInput::setActive(bool active) { if(active) ++starts; else ++stops; }
void MacRawWacomInput::beginReconnect() { ++stops; }
void MacRawWacomInput::finishReconnect() { ++starts; }
void MacRawWacomInput::handleControl(const unsigned char*,unsigned) { ++controls; }
using Buffer=std::array<uint8_t,17000>;
static void feed(PlankMacRelayConnection* server,const uint8_t* bytes,size_t size, std::vector<uint8_t>& output) {
    while(size) { Buffer reply{}; size_t consumed=0,written=0;
        CHECK(plank_mac_relay_receive(server,bytes,size,&consumed,reply.data(),reply.size(),&written)>=0);
        CHECK(consumed>0 && consumed<=size); output.insert(output.end(),reply.begin(),reply.begin()+written);
        bytes+=consumed; size-=consumed;
    }
}
static void connect(PlankMacRelayConnection* server,PltrLink& client) {
    Buffer bytes{},reply{}; size_t size=0;
    CHECK(pltr_link_start(&client,bytes.data(),bytes.size(),&size)==0);
    std::vector<uint8_t> returned; feed(server,bytes.data(),size,returned);
    std::vector<uint8_t> hello;
    size_t offset=0;
    while(offset<returned.size()) {
        PltrFrame frame{}; size_t consumed=0,written=0;
        CHECK(pltr_link_receive(&client,returned.data()+offset,returned.size()-offset,&consumed,reply.data(),reply.size(),&written,&frame)>=0);
        CHECK(consumed>0); offset+=consumed; hello.insert(hello.end(),reply.begin(),reply.begin()+written);
    }
    returned.clear(); feed(server,hello.data(),hello.size(),returned);
    CHECK(client.stage==PLTR_LINK_READY);
}
static void send(PlankMacRelayConnection* server,PltrLink& client,uint16_t type,const uint8_t* data,size_t size) {
    Buffer out{}; size_t n=0; CHECK(pltr_link_send(&client,type,data,size,out.data(),out.size(),&n)==0);
    std::vector<uint8_t> reply; feed(server,out.data(),n,reply);
}
static PltrFrame next(PlankMacRelayConnection* server,PltrLink& client) {
    static Buffer out{},reply{}; size_t n=0,consumed=0,written=0; PltrFrame frame{};
    CHECK(plank_mac_relay_next(server,out.data(),out.size(),&n)==1);
    CHECK(pltr_link_receive(&client,out.data(),n,&consumed,reply.data(),reply.size(),&written,&frame)==1);
    CHECK(consumed==n); return frame;
}
static bool enroll(PlankMacRelayStore* store,const uint8_t privateKey[32],const uint8_t identity[32],const uint8_t id[16],uint64_t now, bool cancel=false,bool expire=false) {
    auto proof=plank_mac_relay_enrollment_create(store,now); CHECK(proof);
    auto client=pltr_client_enrollment_create(privateKey,identity,id); CHECK(client);
    Buffer request{},response{},confirm{},committed{}; size_t size=0,consumed=0,written=0,confirmation=0;
    CHECK(pltr_client_enrollment_start(client,request.data(),request.size(),&size)==0);
    int result=0;
    // One-byte delivery exercises the whole-operation deadline and no partial
    // handshake authorization. Both sides use production Noise, never mocks.
    for(size_t i=0;i<size;++i) {
        result=plank_mac_relay_enrollment_receive(proof,request.data()+i,1,&consumed,response.data(),response.size(),&written,now+(expire?10000:1));
        if(result<0) break; CHECK(consumed==1); if(i+1<size) CHECK(result==0 && written==0);
    }
    bool success=false;
    if(result==1) {
        CHECK(pltr_client_enrollment_receive(client,response.data(),written,&consumed,confirm.data(),confirm.size(),&confirmation)==1);
        if(cancel) {
            uint8_t key[32]; CHECK(pltr_noise_public_key(privateKey,key)==0);
            CHECK(plank_mac_relay_grant(store,true,id,key,identity,now+2));
        }
        result=plank_mac_relay_enrollment_receive(proof,confirm.data(),confirmation,&consumed,committed.data(),committed.size(),&written,now+3);
        if(result==2) {
            size_t ignored=0;
            CHECK(pltr_client_enrollment_receive(client,committed.data(),written,&consumed,response.data(),response.size(),&ignored)==2);
            CHECK(plank_mac_relay_enrollment_receive(proof,confirm.data(),confirmation,&consumed,response.data(),response.size(),&ignored,now+4)<0);
            success=true;
        }
    }
    plank_mac_relay_enrollment_destroy(proof);pltr_client_enrollment_destroy(client);return success;
}
#ifndef PLANK_MAC_RELAY_TEST_NO_MAIN
int main() {
    CHECK(sodium_init()>=0);
    char dir[]="/tmp/plank-mac-relay-test-XXXXXX"; CHECK(mkdtemp(dir));
    auto store=plank_mac_relay_store_create(dir); CHECK(store);
    uint8_t identity[32],clientPublic[32],clientPrivate[32]; CHECK(plank_mac_relay_public_key(store,identity));
    crypto_box_keypair(clientPublic,clientPrivate);
    PltrLink unknown{}; CHECK(pltr_link_init(&unknown,PLTR_NOISE_INITIATOR,clientPrivate,identity,nullptr,nullptr,2)==0);
    auto server=plank_mac_relay_connection_create(store); CHECK(server); CHECK(starts==0);
    Buffer out{},reply{}; size_t n=0,offset=0; CHECK(pltr_link_start(&unknown,out.data(),out.size(),&n)==0);
    bool rejected=false; while(offset<n) { size_t consumed=0,written=0;
        if(plank_mac_relay_receive(server,out.data()+offset,n-offset,&consumed,reply.data(),reply.size(),&written)<0) { rejected=true; break; }
        CHECK(consumed>0); offset+=consumed;
    }
    CHECK(rejected && starts==0); plank_mac_relay_connection_destroy(server); pltr_link_clear(&unknown);
    uint8_t requestID[16]={1};
    CHECK(!enroll(store,clientPrivate,identity,requestID,1000)); // no grant
    CHECK(plank_mac_relay_grant(store,false,requestID,clientPublic,identity,1000));
    CHECK(!plank_mac_relay_grant(store,false,requestID,clientPublic,identity,1000));
    CHECK(enroll(store,clientPrivate,identity,requestID,1001));
    CHECK(!enroll(store,clientPrivate,identity,requestID,1002)); // consumed grant
    uint8_t otherPublic[32],otherPrivate[32];crypto_box_keypair(otherPublic,otherPrivate);
    requestID[0]=2; CHECK(plank_mac_relay_grant(store,false,requestID,otherPublic,identity,2000));
    CHECK(!enroll(store,otherPrivate,identity,requestID,2001,true)); // cancel after claim
    requestID[0]=3; CHECK(plank_mac_relay_grant(store,false,requestID,otherPublic,identity,3000));
    CHECK(!enroll(store,otherPrivate,identity,requestID,3001,false,true)); // whole 10s proof bound
    requestID[0]=4; CHECK(plank_mac_relay_grant(store,false,requestID,otherPublic,identity,4000));
    CHECK(!enroll(store,otherPrivate,identity,requestID,124000)); // grant expires at boundary
    requestID[0]=5; CHECK(plank_mac_relay_grant(store,false,requestID,otherPublic,identity,125000));
    CHECK(!enroll(store,clientPrivate,identity,requestID,125001)); // key mismatch cannot claim
    uint8_t zero[32]{}; CHECK(!plank_mac_relay_grant(store,false,requestID,zero,identity,125002));
    CHECK(!plank_mac_relay_grant(store,false,requestID,otherPublic,zero,125002));
    server=plank_mac_relay_connection_create(store); CHECK(server);
    PltrLink client{}; CHECK(pltr_link_init(&client,PLTR_NOISE_INITIATOR,clientPrivate,identity,nullptr,nullptr,2)==0);
    connect(server,client); CHECK(starts==0 && !plank_mac_relay_ready(server));
    const uint8_t ready[]={0x24,0,0,0,1}; send(server,client,PLTR_SESSION_READY,ready,5);
    CHECK(starts==1 && plank_mac_relay_ready(server)); CHECK(next(server,client).type==PLTR_STATUS);
    const auto firstGeneration=generations(); CHECK(firstGeneration!=0);
    const auto secondGeneration=generations(); CHECK(secondGeneration!=0 && firstGeneration!=secondGeneration);
    // Exact fake raw input reaches authenticated Client with no coalescing.
    PLANK_RAW_HID_WIRE_HEADER header{}; header.magic=PLANK_RAW_HID_WIRE_MAGIC; header.version=PLANK_RAW_HID_WIRE_VERSION;
    header.type=PLANK_RAW_HID_INPUT; header.generation=secondGeneration; header.payloadLength=2;
    std::array<uint8_t,sizeof(header)+2> raw{}; std::memcpy(raw.data(),&header,sizeof(header)); raw[20]=2; raw[21]=0x42;
    CHECK(sender(raw.data(),raw.size())); auto frame=next(server,client);
    CHECK(frame.type==PLTR_CLIENT_FRAME && frame.payload_size==raw.size()+8);
    CHECK(std::memcmp(frame.payload+8,raw.data(),raw.size())==0);
    header.type=PLANK_RAW_HID_GET_REPORT; std::memcpy(raw.data(),&header,sizeof(header));
    send(server,client,PLTR_HOST_FRAME,raw.data(),raw.size()); CHECK(controls==1);
    send(server,client,PLTR_RECONNECT_BEGIN,nullptr,0); CHECK(stops==1);
    send(server,client,PLTR_RECONNECT_FINISH,nullptr,0); CHECK(starts==2);
    header.type=PLANK_RAW_HID_INPUT; std::memcpy(raw.data(),&header,sizeof(header));
    for(unsigned i=0;i<256;++i) CHECK(sender(raw.data(),raw.size()));
    CHECK(!sender(raw.data(),raw.size())); CHECK(plank_mac_relay_next(server,out.data(),out.size(),&n)==-1);
    auto late=sender; plank_mac_relay_connection_destroy(server); CHECK(!late(raw.data(),raw.size()));
    // Stalled driver retains a shared store; destroying UI store is safe.
    auto lateGenerations=generations; plank_mac_relay_store_destroy(store);
    CHECK(lateGenerations()!=0); lateGenerations={}; generations={};
    store=plank_mac_relay_store_create(dir); CHECK(store); uint8_t persisted[32]; CHECK(plank_mac_relay_public_key(store,persisted));
    CHECK(std::memcmp(identity,persisted,32)==0); server=plank_mac_relay_connection_create(store); CHECK(server);
    CHECK(generations()>secondGeneration); plank_mac_relay_connection_destroy(server); generations={}; sender={};
    plank_mac_relay_store_destroy(store); pltr_link_clear(&client);
    // Same-process and separate-process capture exclusion, release and no symlink following.
    char leaseDir[]="/tmp/plank-mac-lease-test-XXXXXX"; CHECK(mkdtemp(leaseDir));
    MacWacomCaptureLease a,b; CHECK(a.acquire(leaseDir)); CHECK(!b.acquire(leaseDir));
    pid_t child=fork(); CHECK(child>=0);
    if(child==0) { a.release(); MacWacomCaptureLease separate; _exit(separate.acquire(leaseDir)?1:0); }
    int status=0; CHECK(waitpid(child,&status,0)==child); CHECK(WIFEXITED(status)&&WEXITSTATUS(status)==0);
    a.release(); CHECK(b.acquire(leaseDir)); b.release();
    CHECK(chmod(leaseDir,0777)==0); CHECK(!a.acquire(leaseDir)); CHECK(chmod(leaseDir,0700)==0);
    CHECK(unlink((std::string(leaseDir)+"/capture.lock").c_str())==0);
    CHECK(symlink("/dev/null",(std::string(leaseDir)+"/capture.lock").c_str())==0); CHECK(!a.acquire(leaseDir));
    std::printf("Mac Relay authenticated raw framing, gates, bounded queue, durable generations and physical lease: %u checks passed\n",checks);
}

#endif

#ifdef PLANK_MAC_RELAY_TEST_NO_MAIN
#include "PlankMacRelaySocketFixture.h"
struct MacRelaySocketClient { PltrLink link{}; unsigned reports=0; bool pong=false; };
extern "C" MacRelaySocketClient* mac_relay_socket_client(PlankMacRelayStore* store) {
    CHECK(sodium_init()>=0);
    uint8_t identity[32],pub[32],priv[32],id[16]={1};
    CHECK(plank_mac_relay_public_key(store,identity)); crypto_box_keypair(pub,priv);
    CHECK(plank_mac_relay_grant(store,false,id,pub,identity,1000));
    CHECK(enroll(store,priv,identity,id,1001));
    auto client=new MacRelaySocketClient;
    CHECK(pltr_link_init(&client->link,PLTR_NOISE_INITIATOR,priv,identity,nullptr,nullptr,2)==0);
    sodium_memzero(priv,sizeof(priv)); return client;
}
extern "C" void mac_relay_socket_destroy(MacRelaySocketClient* c) { pltr_link_clear(&c->link); delete c; }
extern "C" int mac_relay_socket_start(MacRelaySocketClient* c,uint8_t* out,size_t cap,size_t* n) { return pltr_link_start(&c->link,out,cap,n); }
extern "C" int mac_relay_socket_receive(MacRelaySocketClient* c,const uint8_t* data,size_t size,uint8_t* out,size_t cap,size_t* n) {
    *n=0;
    while(size) {
        PltrFrame f{};size_t used=0,written=0;
        if(pltr_link_receive(&c->link,data,size,&used,out+*n,cap-*n,&written,&f)<0 || !used || used>size) return -1;
        *n+=written;data+=used;size-=used;
        if(f.type==PLTR_CLIENT_FRAME) {
            if(f.payload_size!=32 || f.payload[28]!=2 || f.payload[29]!=(c->reports&255) || f.payload[30]!=((c->reports>>8)&255) || f.payload[31]!=0x42) return -1;
            ++c->reports;
        } else if(f.type==PLTR_PONG) { c->pong=true; }
    }
    return 0;
}
extern "C" bool mac_relay_socket_ready(MacRelaySocketClient* c) { return c->link.stage==PLTR_LINK_READY; }
extern "C" int mac_relay_socket_session_ready(MacRelaySocketClient* c,uint8_t* out,size_t cap,size_t* n) {
    const uint8_t ready[]={0x24,0,0,0,1}; return pltr_link_send(&c->link,PLTR_SESSION_READY,ready,sizeof(ready),out,cap,n);
}
extern "C" int mac_relay_socket_ping(MacRelaySocketClient* c,uint8_t* out,size_t cap,size_t* n) {
    uint8_t ping[16]={1};return pltr_link_send(&c->link,PLTR_PING,ping,sizeof(ping),out,cap,n);
}
extern "C" bool mac_relay_socket_burst(unsigned count) {
    if(!sender || !generations || count>256) return false;
    PLANK_RAW_HID_WIRE_HEADER header{};header.magic=PLANK_RAW_HID_WIRE_MAGIC;header.version=PLANK_RAW_HID_WIRE_VERSION;
    header.type=PLANK_RAW_HID_INPUT;header.generation=generations();header.payloadLength=4;
    std::array<uint8_t,24> raw{};std::memcpy(raw.data(),&header,sizeof(header));raw[20]=2;raw[23]=0x42;
    for(unsigned i=0;i<count;++i) {raw[21]=uint8_t(i);raw[22]=uint8_t(i>>8);if(!sender(raw.data(),raw.size())) return false;}
    return true;
}
extern "C" unsigned mac_relay_socket_reports(MacRelaySocketClient* c) { return c->reports; }
extern "C" bool mac_relay_socket_pong(MacRelaySocketClient* c) { return c->pong; }
#endif
