#include "PlankPencilImageCrypto.h"
#include "noise.h"
#include <sodium.h>
#include <stdlib.h>
#include <string.h>

static const uint8_t purpose[]="PLANK-PENCIL-REFERENCE-IMAGE/1";
enum { PAYLOAD_SIZE=sizeof(purpose)-1+32, MAX_PLAIN=7680, MAX_CIPHER=7696 };
struct PlankPencilImageCrypto {
    PltrNoise noise;
    uint8_t peer[32], binding[32];
    int initiator, stage;
};
static int fail(PlankPencilImageCrypto *c) {
    if(c) { sodium_memzero(c,sizeof(*c)); c->stage=-1; }
    return -1;
}
static void payload(const PlankPencilImageCrypto *c,uint8_t out[PAYLOAD_SIZE]) {
    memcpy(out,purpose,sizeof(purpose)-1);
    memcpy(out+sizeof(purpose)-1,c->binding,32);
}
static int matches(const PlankPencilImageCrypto *c,const uint8_t *bytes,size_t size) {
    uint8_t expected[PAYLOAD_SIZE]; payload(c,expected);
    int valid=size==sizeof(expected) && !sodium_memcmp(bytes,expected,sizeof(expected));
    sodium_memzero(expected,sizeof(expected)); return valid;
}
PlankPencilImageCrypto *plank_pencil_image_crypto_create(int initiator,const uint8_t key[32],const uint8_t peer[32],const uint8_t hash[64]) {
    if(!key || !peer || !hash || sodium_init()<0) return NULL;
    PlankPencilImageCrypto *c=calloc(1,sizeof(*c)); if(!c) return NULL;
    c->initiator=initiator!=0; memcpy(c->peer,peer,32);
    // Hash all64 bytes; truncating the final handshake hash would weaken the
    // intended binding and miss changes outside its first32 bytes.
    if(crypto_generichash(c->binding,32,hash,64,NULL,0) ||
       pltr_noise_init(&c->noise,initiator?PLTR_NOISE_INITIATOR:PLTR_NOISE_RESPONDER,key,peer,2)) {
        sodium_memzero(c,sizeof(*c)); free(c); return NULL;
    }
    return c;
}
void plank_pencil_image_crypto_destroy(PlankPencilImageCrypto *c) {
    if(c) { sodium_memzero(c,sizeof(*c)); free(c); }
}
int plank_pencil_image_crypto_first(PlankPencilImageCrypto *c,uint8_t *out,size_t cap,size_t *written) {
    if(!c || !c->initiator || c->stage!=0) return fail(c);
    uint8_t p[PAYLOAD_SIZE]; payload(c,p);
    int result=pltr_noise_write_first(&c->noise,p,sizeof(p),out,cap,written);
    sodium_memzero(p,sizeof(p)); if(result) return fail(c); c->stage=1; return 0;
}
int plank_pencil_image_crypto_accept_first(PlankPencilImageCrypto *c,const uint8_t *in,size_t n) {
    if(!c || c->initiator || c->stage!=0 || n!=96+PAYLOAD_SIZE) return fail(c);
    uint8_t p[PAYLOAD_SIZE], peer[32]; size_t read=0;
    int result=pltr_noise_read_first(&c->noise,in,n,peer,p,sizeof(p),&read);
    int valid=!result && matches(c,p,read) && !sodium_memcmp(peer,c->peer,32);
    sodium_memzero(p,sizeof(p)); sodium_memzero(peer,sizeof(peer));
    if(!valid) return fail(c); c->stage=1; return 0;
}
int plank_pencil_image_crypto_second(PlankPencilImageCrypto *c,uint8_t *out,size_t cap,size_t *written) {
    if(!c || c->initiator || c->stage!=1) return fail(c);
    uint8_t p[PAYLOAD_SIZE]; payload(c,p);
    int result=pltr_noise_write_second(&c->noise,c->peer,p,sizeof(p),out,cap,written);
    sodium_memzero(p,sizeof(p)); if(result) return fail(c); c->stage=2; return 0;
}
int plank_pencil_image_crypto_accept_second(PlankPencilImageCrypto *c,const uint8_t *in,size_t n) {
    if(!c || !c->initiator || c->stage!=1 || n!=48+PAYLOAD_SIZE) return fail(c);
    uint8_t p[PAYLOAD_SIZE]; size_t read=0;
    int result=pltr_noise_read_second(&c->noise,in,n,p,sizeof(p),&read);
    int valid=!result && matches(c,p,read);
    sodium_memzero(p,sizeof(p)); if(!valid) return fail(c); c->stage=2; return 0;
}
int plank_pencil_image_crypto_encrypt(PlankPencilImageCrypto *c,const uint8_t *in,size_t n,uint8_t *out,size_t cap,size_t *written) {
    if(!c || c->stage!=2 || !n || n>MAX_PLAIN || pltr_noise_encrypt(&c->noise,in,n,out,cap,written)) return fail(c);
    return 0;
}
int plank_pencil_image_crypto_decrypt(PlankPencilImageCrypto *c,const uint8_t *in,size_t n,uint8_t *out,size_t cap,size_t *read) {
    if(!c || c->stage!=2 || n<=16 || n>MAX_CIPHER || pltr_noise_decrypt(&c->noise,in,n,out,cap,read)) return fail(c);
    return 0;
}
