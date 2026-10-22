#include "PlankMacRelay.h"
#include "macrawwacom.h"
#include "macrawwacomlogic.h"
#include "identity.h"
#include "link.h"
#include <sodium.h>
#include <algorithm>
#include <array>
#include <chrono>
#include <cstring>
#include <cstdio>
#include <deque>
#include <memory>
#include <mutex>
#include <vector>

namespace {
using Clock = std::chrono::steady_clock;
struct Store {
    PltrIdentityStore value{};
    std::mutex mutex;
    struct Grant { std::array<uint8_t,16> id{}; std::array<uint8_t,32> key{}; uint64_t deadline=0; unsigned state=0; };
    std::array<Grant,16> grants{};
    Store() { value.directory_fd = value.lock_fd = -1; }
    ~Store() { pltr_identity_store_close(&value); }
    static int approved(void* context, const uint8_t key[32]) {
        auto& store = *static_cast<Store*>(context);
        std::lock_guard<std::mutex> guard(store.mutex);
        return pltr_identity_store_approve(&store.value, key);
    }
    uint16_t generation() {
        std::lock_guard<std::mutex> guard(mutex);
        uint16_t result = 0;
        return pltr_identity_store_next_generation(&value, &result) == 0 ? result : 0;
    }
};
struct Frames {
    std::mutex mutex;
    std::deque<std::vector<uint8_t>> queue;
    size_t bytes = 0;
    bool closed = false, failed = false;
    std::array<uint8_t,8> status{0,0,0,0,0,0,1,0};
    uint16_t generation = 0;
    uint64_t statusEpoch = 1;
    bool offer(const unsigned char* data, size_t size) {
        std::array<uint8_t, PLTR_MAX_FRAME_SIZE> validate{};
        if (!data || size < 20 || size > PLTR_MAX_PAYLOAD_SIZE - 8) return fail();
        std::vector<uint8_t> payload(8 + size);
        const auto type = uint16_t(data[6]) | uint16_t(data[7]) << 8;
        if (type == PLANK_RAW_HID_INPUT) {
            const auto us = std::chrono::duration_cast<std::chrono::microseconds>(Clock::now().time_since_epoch()).count();
            for (unsigned i=0; i<8; ++i) payload[i] = uint8_t(uint64_t(us) >> (8*i));
        }
        std::copy(data,data+size,payload.begin()+8);
        size_t count = 0;
        if (pltr_encode_frame(PLTR_CLIENT_FRAME,1,payload.data(),payload.size(),
            PLTR_RELAY_TO_CLIENT,PLTR_SECURE,validate.data(),validate.size(),&count)) return fail();
        std::lock_guard<std::mutex> guard(mutex);
        if (closed || failed) return false;
        if (queue.size() >= 256 || payload.size() > 256*1024-bytes) {
            std::fprintf(stderr,"PLANK Mac Relay: raw inbox exhausted (%zu records, %zu bytes); closing link\n",queue.size(),bytes);
            failed=true; return false;
        }
        bytes += payload.size(); queue.push_back(std::move(payload));
        if (type == PLANK_RAW_HID_DEVICE && size >= 20 + sizeof(PLANK_RAW_HID_DEVICE_MESSAGE)) {
            // Device fields retain the exact PLWH descriptor/message semantics.
            generation = uint16_t(data[10]) | uint16_t(data[11]) << 8;
            const uint8_t* d = data+20;
            status = {2,d[4],d[5],d[8],d[9],d[0],1,0}; ++statusEpoch;
        } else if (type == PLANK_RAW_HID_SUSPEND || type == PLANK_RAW_HID_DETACH) {
            status[0] = type == PLANK_RAW_HID_SUSPEND ? 4 : 0; ++statusEpoch;
            if (type == PLANK_RAW_HID_DETACH) generation = 0;
        }
        return true;
    }
    bool fail() { std::lock_guard<std::mutex> guard(mutex); failed=true; return false; }
};
}
struct PlankMacRelayStore { std::shared_ptr<Store> store; };
struct PlankMacRelayConnection {
    std::shared_ptr<Store> store;
    std::shared_ptr<Frames> frames = std::make_shared<Frames>();
    std::unique_ptr<MacRawWacomInput> worker;
    PltrLink link{};
    bool ready = false, ended = false;
    uint64_t sentStatus = 0, pingAt = std::chrono::duration_cast<std::chrono::milliseconds>(Clock::now().time_since_epoch()).count();
    ~PlankMacRelayConnection() {
        // Revoke output before bounded shutdown; a stalled worker retains its
        // own frames/store/physical lease without touching disposed network UI.
        { std::lock_guard<std::mutex> guard(frames->mutex); frames->closed=true; }
        worker.reset(); pltr_link_clear(&link);
    }
};
extern "C" PlankMacRelayStore* plank_mac_relay_store_create(const char* directory) {
    if (!directory) return nullptr;
    auto result = std::make_unique<PlankMacRelayStore>(); result->store=std::make_shared<Store>();
    if (pltr_identity_store_open(&result->store->value,directory)) return nullptr;
    return result.release();
}
extern "C" void plank_mac_relay_store_destroy(PlankMacRelayStore* store) { delete store; }
extern "C" bool plank_mac_relay_public_key(PlankMacRelayStore* store,uint8_t key[32]) {
    if (!store || !key) return false;
    std::copy_n(store->store->value.public_key,32,key); return true;
}
extern "C" PlankMacRelayConnection* plank_mac_relay_connection_create(PlankMacRelayStore* store) {
    if (!store) return nullptr;
    auto result=std::make_unique<PlankMacRelayConnection>(); result->store=store->store;
    if (pltr_link_init(&result->link,PLTR_NOISE_RESPONDER,result->store->value.private_key,
        nullptr,Store::approved,result->store.get(),2)) return nullptr;
    result->worker=std::make_unique<MacRawWacomInput>([] {},
        [frames=result->frames](const unsigned char* data,size_t size) { return frames->offer(data,size); },
        [store=result->store] { return store->generation(); },false);
    return result.release();
}
extern "C" void plank_mac_relay_connection_destroy(PlankMacRelayConnection* connection) { delete connection; }
extern "C" bool plank_mac_relay_ready(PlankMacRelayConnection* connection) { return connection && connection->ready && !connection->ended; }
extern "C" int plank_mac_relay_receive(PlankMacRelayConnection* c,const uint8_t* bytes,size_t size,
    size_t* consumed,uint8_t* output,size_t capacity,size_t* written) {
    if (!c || c->ended || !consumed || !written) return -1;
    PltrFrame frame{};
    const int result=pltr_link_receive(&c->link,bytes,size,consumed,output,capacity,written,&frame);
    if (result < 0) { c->ended=true; return -1; }
    if (result==1) switch (frame.type) {
    case PLTR_SESSION_READY:
        if (c->ready || frame.payload_size!=5) return -1;
        c->ready=true; c->worker->setActive(frame.payload[4]!=0); break;
    case PLTR_SESSION_ACTIVE: c->worker->setActive(frame.payload[0]!=0); break;
    case PLTR_RECONNECT_BEGIN: c->worker->beginReconnect(); break;
    case PLTR_RECONNECT_FINISH: c->worker->finishReconnect(); break;
    case PLTR_HOST_FRAME: {
        c->worker->handleControl(frame.payload,frame.payload_size);
        MacWacomWire::Control control;
        if (MacWacomWire::parse(frame.payload,frame.payload_size,control) && control.type==PLANK_RAW_HID_ATTACH_RESULT) {
            std::lock_guard<std::mutex> guard(c->frames->mutex);
            if (control.generation==c->frames->generation) {
                c->frames->status[0]=frame.payload[20]==0 && frame.payload[21]==0 && frame.payload[22]==0 && frame.payload[23]==0 ? 3 : 7;
                ++c->frames->statusEpoch;
            }
        }
        break;
    }
    case PLTR_PING: {
        uint8_t pong[32]{}; std::copy_n(frame.payload,16,pong);
        const auto us=std::chrono::duration_cast<std::chrono::microseconds>(Clock::now().time_since_epoch()).count();
        for (unsigned i=0;i<8;++i) pong[16+i]=pong[24+i]=uint8_t(uint64_t(us)>>(8*i));
        if (*written || pltr_link_send(&c->link,PLTR_PONG,pong,32,output,capacity,written)) return -1;
        break;
    }
    case PLTR_SESSION_END: case PLTR_GOODBYE:
        c->ended=true; c->worker->setActive(false); break;
    default: break; // HELLO/Noise/OPEN/PONG validated by the shared codec.
    }
    return *written ? 1 : 0;
}
extern "C" int plank_mac_relay_next(PlankMacRelayConnection* c,uint8_t* out,size_t capacity,size_t* written) {
    if (!c || !written || c->ended) return -1;
    *written=0;
    if (c->link.stage!=PLTR_LINK_READY) return 0;
    // Public status follows HELLO even before SESSION_READY, as on Linux.
    // Capture and HID controls still require the shared session ordering.
    const auto now=std::chrono::duration_cast<std::chrono::milliseconds>(Clock::now().time_since_epoch()).count();
    if (c->sentStatus && uint64_t(now)-c->pingAt>=1000) {
        uint8_t ping[16]{}; for(unsigned i=0;i<8;++i) ping[i]=ping[8+i]=uint8_t((uint64_t(now)*1000)>>(8*i));
        c->pingAt=now;
        return pltr_link_send(&c->link,PLTR_PING,ping,16,out,capacity,written)==0?1:-1;
    }
    std::vector<uint8_t> payload; std::array<uint8_t,8> status{}; bool statusChanged=false;
    { std::lock_guard<std::mutex> guard(c->frames->mutex);
        if (c->frames->failed) return -1;
        if (c->frames->statusEpoch!=c->sentStatus) {
            status=c->frames->status; c->sentStatus=c->frames->statusEpoch; statusChanged=true;
        } else if (!c->frames->queue.empty()) {
            payload=std::move(c->frames->queue.front()); c->frames->queue.pop_front(); c->frames->bytes-=payload.size();
        }
    }
    if (!statusChanged && payload.empty()) return 0;
    if (pltr_link_send(&c->link,statusChanged?PLTR_STATUS:PLTR_CLIENT_FRAME,
        statusChanged?status.data():payload.data(),statusChanged?status.size():payload.size(),out,capacity,written)) {
        c->ended=true; return -1;
    }
    return 1;
}

// Local consent-authorized grant adapter. Same PLEN/1 proof, expiry, one claim
// and durable commit semantics as the Linux drawing enrollment endpoint.
extern "C" bool plank_mac_relay_grant(PlankMacRelayStore* owner, bool cancel,
    const uint8_t id[16], const uint8_t client[32], const uint8_t target[32], uint64_t now) {
    if(!owner || !id || !client || !target || sodium_is_zero(id,16) || sodium_is_zero(client,32)) return false;
    auto& store=*owner->store;
    if(sodium_memcmp(target,store.value.public_key,32) || sodium_memcmp(client,target,32)==0) return false;
    uint8_t shared[32]; const bool valid=crypto_scalarmult(shared,store.value.private_key,client)==0;
    sodium_memzero(shared,32); if(!valid) return false;
    std::lock_guard<std::mutex> guard(store.mutex);
    for(auto& g:store.grants) if(g.state && now>=g.deadline) g=Store::Grant{};
    for(auto& g:store.grants) if(g.state && sodium_memcmp(g.id.data(),id,16)==0) {
        if(!cancel || sodium_memcmp(g.key.data(),client,32)) return false;
        g.state=3; return true;
    }
    if(cancel) return false;
    for(auto& g:store.grants) if(!g.state) {
        std::copy_n(id,16,g.id.data()); std::copy_n(client,32,g.key.data()); g.state=1; g.deadline=now+120000; return true;
    }
    return false;
}
struct PlankMacRelayEnrollment {
    std::shared_ptr<Store> store;
    PltrNoise noise{};
    std::array<uint8_t,119> input{};
    std::array<uint8_t,16> id{};
    std::array<uint8_t,32> client{};
    size_t filled=0; unsigned stage=0; uint64_t deadline=0;
    ~PlankMacRelayEnrollment(){ pltr_noise_clear(&noise); sodium_memzero(input.data(),input.size()); }
};
extern "C" PlankMacRelayEnrollment* plank_mac_relay_enrollment_create(PlankMacRelayStore* store,uint64_t now) {
    if(!store) return nullptr;
    auto result=std::make_unique<PlankMacRelayEnrollment>(); result->store=store->store; result->deadline=now+10000;
    if(pltr_noise_init_enrollment(&result->noise,PLTR_NOISE_RESPONDER,result->store->value.private_key,nullptr)) return nullptr;
    return result.release();
}
extern "C" void plank_mac_relay_enrollment_destroy(PlankMacRelayEnrollment* proof){ delete proof; }
extern "C" int plank_mac_relay_enrollment_receive(PlankMacRelayEnrollment* proof,const uint8_t* bytes,size_t size,
    size_t* consumed,uint8_t* output,size_t capacity,size_t* written,uint64_t now) {
    if(!proof || !bytes || !consumed || !written || now>=proof->deadline || proof->stage>=2 || capacity<50) return -1;
    *consumed=*written=0;
    const size_t needed=proof->stage==0?119:39;
    const size_t take=std::min(size,needed-proof->filled);
    std::copy_n(bytes,take,proof->input.data()+proof->filled); proof->filled+=take; *consumed=take;
    if(proof->filled<needed) return 0;
    size_t n=0; auto& store=*proof->store;
    std::lock_guard<std::mutex> guard(store.mutex);
    if(proof->stage==0) {
        if(std::memcmp(proof->input.data(),"PLEN\1",5) || proof->input[5]!=112 || proof->input[6]!=0 ||
            pltr_noise_read_first(&proof->noise,proof->input.data()+7,112,proof->client.data(),proof->id.data(),16,&n) || n!=16) return -1;
        bool claimed=false;
        for(auto& g:store.grants) if(g.state==1 && now<g.deadline &&
            !sodium_memcmp(g.id.data(),proof->id.data(),16) && !sodium_memcmp(g.key.data(),proof->client.data(),32)) {
            g.state=2; claimed=true; break;
        }
        if(!claimed || pltr_noise_write_second(&proof->noise,proof->client.data(),nullptr,0,output+2,capacity-2,&n) || n!=48) return -1;
        output[0]=48; output[1]=0; *written=50; proof->stage=1; proof->filled=0; return 1;
    }
    uint8_t plain[21];
    if(proof->input[0]!=37 || proof->input[1]!=0 ||
        pltr_noise_decrypt(&proof->noise,proof->input.data()+2,37,plain,21,&n) || n!=21 ||
        std::memcmp(plain,"PLEN\1",5) || sodium_memcmp(plain+5,proof->id.data(),16)) return -1;
    bool committed=false;
    for(auto& g:store.grants) if(g.state==2 && now<g.deadline &&
        !sodium_memcmp(g.id.data(),proof->id.data(),16) && !sodium_memcmp(g.key.data(),proof->client.data(),32)) {
        g.state=3; committed=pltr_identity_store_add(&store.value,proof->client.data())==0; break;
    }
    if(!committed || pltr_noise_encrypt(&proof->noise,plain,21,output+2,capacity-2,&n) || n!=37) return -1;
    output[0]=37;output[1]=0;*written=39;proof->stage=2;return 2;
}

extern "C" unsigned plank_mac_relay_tablet_state(PlankMacRelayConnection* c) {
    if(!c) return 0; std::lock_guard<std::mutex> guard(c->frames->mutex); return c->frames->status[0];
}
