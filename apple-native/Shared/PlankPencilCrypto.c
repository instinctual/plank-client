#include "PlankPencilCrypto.h"
#include "noise.h"
#include <stdlib.h>
#include <string.h>
#include <sodium.h>
struct PlankPencilCrypto { PltrNoise noise; int initiator, stage; char code[13]; };
int plank_pencil_crypto_public_key(const uint8_t key[32],uint8_t public_key[32]) { return pltr_noise_public_key(key,public_key); }
static const uint8_t capability[] = "PLANK-NORMALIZED-PEN-KEYS/3";
static int fail(PlankPencilCrypto *c) { if(c) { c->stage=-1; pltr_noise_clear(&c->noise); } return -1; }
static void code(PlankPencilCrypto *c) { sodium_bin2hex(c->code,13,c->noise.handshake_hash,6); }
PlankPencilCrypto *plank_pencil_crypto_create(int initiator,const uint8_t key[32],const uint8_t peer[32]) {
    if(!key || (initiator && !peer)) return NULL;
    PlankPencilCrypto *c=calloc(1,sizeof(*c)); if(!c) return NULL;
    c->initiator=initiator!=0;
    // Reuse the pinned, reviewed IK implementation, with TCP binding and an
    // exact application capability authenticated inside the handshake. No
    // caller-selected prologue, new cipher or raw-frame bypass is introduced.
    if(pltr_noise_init(&c->noise,initiator?PLTR_NOISE_INITIATOR:PLTR_NOISE_RESPONDER,key,peer,2)) { free(c); return NULL; }
    return c;
}
void plank_pencil_crypto_destroy(PlankPencilCrypto *c) { if(c) { pltr_noise_clear(&c->noise); sodium_memzero(c,sizeof(*c)); free(c); } }
int plank_pencil_crypto_first(PlankPencilCrypto *c,uint8_t *out,size_t cap,size_t *n) {
    if(!c || !c->initiator || c->stage!=0) return fail(c);
    if(pltr_noise_write_first(&c->noise,capability,sizeof(capability)-1,out,cap,n)) return fail(c);
    code(c); c->stage=1; return 0;
}
int plank_pencil_crypto_accept_first(PlankPencilCrypto *c,const uint8_t *in,size_t n) {
    uint8_t payload[sizeof(capability)-1], peer[32]; size_t count=0;
    if(!c || c->initiator || c->stage!=0 || n!=96+sizeof(payload)) return fail(c);
    if(pltr_noise_read_first(&c->noise,in,n,peer,payload,sizeof(payload),&count) || count!=sizeof(payload) || sodium_memcmp(payload,capability,sizeof(payload))) return fail(c);
    code(c); c->stage=1; return 0;
}
int plank_pencil_crypto_approve(PlankPencilCrypto *c,uint8_t *out,size_t cap,size_t *n) {
    if(!c || c->initiator || c->stage!=1) return fail(c);
    if(pltr_noise_write_second(&c->noise,c->noise.remote_static,capability,sizeof(capability)-1,out,cap,n)) return fail(c);
    c->stage=2; return 0;
}
int plank_pencil_crypto_accept_second(PlankPencilCrypto *c,const uint8_t *in,size_t n) {
    uint8_t payload[sizeof(capability)-1]; size_t count=0;
    if(!c || !c->initiator || c->stage!=1 || n!=48+sizeof(payload)) return fail(c);
    if(pltr_noise_read_second(&c->noise,in,n,payload,sizeof(payload),&count) || count!=sizeof(payload) || sodium_memcmp(payload,capability,sizeof(payload))) return fail(c);
    c->stage=2; return 0;
}
int plank_pencil_crypto_code(PlankPencilCrypto *c,char out[13]) { if(!c || c->stage<1 || !out) return -1; memcpy(out,c->code,13); return 0; }
int plank_pencil_crypto_peer(PlankPencilCrypto *c,uint8_t key[32]) { if(!c || c->stage<1 || !key) return -1; memcpy(key,c->noise.remote_static,32); return 0; }
int plank_pencil_crypto_encrypt(PlankPencilCrypto *c,const uint8_t *in,size_t n,uint8_t *out,size_t cap,size_t *written) {
    if(!c || c->stage!=2 || n>224 || pltr_noise_encrypt(&c->noise,in,n,out,cap,written)) return fail(c); return 0;
}
int plank_pencil_crypto_decrypt(PlankPencilCrypto *c,const uint8_t *in,size_t n,uint8_t *out,size_t cap,size_t *read) {
    if(!c || c->stage!=2 || n>240 || pltr_noise_decrypt(&c->noise,in,n,out,cap,read)) return fail(c); return 0;
}
