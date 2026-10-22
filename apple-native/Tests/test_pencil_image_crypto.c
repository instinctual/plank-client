#include "PlankPencilImageCrypto.h"
#include "PlankPencilCrypto.h"
#include "noise.h"
#include <sodium.h>
#include <assert.h>
#include <stdio.h>
#include <string.h>

// Curve25519 clamps the low3 bits: use distinct non-clamped bits so this fixture
// actually proves wrong-key rejection rather than generating the same identity.
static uint8_t akey[32]={16},bkey[32]={32},other[32]={64},apub[32],bpub[32],opub[32],hash[64];
static void exchange(PlankPencilImageCrypto *a,PlankPencilImageCrypto *b) {
    uint8_t first[256],second[256]; size_t n=0;
    assert(!plank_pencil_image_crypto_first(a,first,sizeof(first),&n));
    assert(!plank_pencil_image_crypto_accept_first(b,first,n));
    assert(!plank_pencil_image_crypto_second(b,second,sizeof(second),&n));
    assert(!plank_pencil_image_crypto_accept_second(a,second,n));
}
static void purpose_payload(uint8_t *out,size_t *size,int bad) {
    static const uint8_t purpose[]="PLANK-PENCIL-REFERENCE-IMAGE/1";
    memcpy(out,purpose,sizeof(purpose)-1);
    assert(!crypto_generichash(out+sizeof(purpose)-1,32,hash,64,NULL,0));
    *size=sizeof(purpose)-1+32;
    if(bad) out[0]^=1;
}
int main(void) {
    assert(sodium_init()>=0);
    assert(!plank_pencil_crypto_public_key(akey,apub));
    assert(!plank_pencil_crypto_public_key(bkey,bpub));
    assert(!plank_pencil_crypto_public_key(other,opub));
    assert(memcmp(apub,bpub,32) && memcmp(apub,opub,32) && memcmp(bpub,opub,32));
    // Existing pen handshake remains byte/capability-identical. Final hash is
    // unavailable at stages0/1 and equal only after both authenticated messages.
    PlankPencilCrypto *penA=plank_pencil_crypto_create(1,akey,bpub),*penB=plank_pencil_crypto_create(0,bkey,NULL);
    uint8_t first[256],second[256],h2[64]; size_t n=0;
    assert(plank_pencil_crypto_final_hash(penA,hash));
    assert(!plank_pencil_crypto_first(penA,first,sizeof(first),&n));
    assert(!plank_pencil_crypto_accept_first(penB,first,n));
    assert(plank_pencil_crypto_final_hash(penA,hash) && plank_pencil_crypto_final_hash(penB,hash));
    assert(!plank_pencil_crypto_approve(penB,second,sizeof(second),&n));
    assert(!plank_pencil_crypto_accept_second(penA,second,n));
    assert(!plank_pencil_crypto_final_hash(penA,hash)); assert(!plank_pencil_crypto_final_hash(penB,h2));
    assert(!memcmp(hash,h2,64));
    PlankPencilImageCrypto *a=plank_pencil_image_crypto_create(1,akey,bpub,hash),*b=plank_pencil_image_crypto_create(0,bkey,apub,hash);
    assert(a && b); exchange(a,b);
    uint8_t plain[7681],cipher[7697],output[7681]; memset(plain,0x4a,sizeof(plain)); size_t c=0,d=0;
    assert(!plank_pencil_image_crypto_encrypt(a,plain,7680,cipher,sizeof(cipher),&c) && c==7696);
    assert(!plank_pencil_image_crypto_decrypt(b,cipher,c,output,sizeof(output),&d) && d==7680 && !memcmp(output,plain,d));
    assert(plank_pencil_image_crypto_decrypt(b,cipher,c,output,sizeof(output),&d)); // replay poisons only media
    plank_pencil_image_crypto_destroy(a); plank_pencil_image_crypto_destroy(b);
    assert(!plank_pencil_crypto_encrypt(penA,plain,1,cipher,sizeof(cipher),&c));
    assert(!plank_pencil_crypto_decrypt(penB,cipher,c,output,sizeof(output),&d)); // media failure never mutates pen
    plank_pencil_crypto_destroy(penA);
    // Binding regression flips ONLY the last of all64 transcript bytes.
    memcpy(h2,hash,64); h2[63]^=1;
    a=plank_pencil_image_crypto_create(1,akey,bpub,hash); b=plank_pencil_image_crypto_create(0,bkey,apub,h2);
    assert(!plank_pencil_image_crypto_first(a,first,sizeof(first),&n));
    assert(plank_pencil_image_crypto_accept_first(b,first,n));
    plank_pencil_image_crypto_destroy(a); plank_pencil_image_crypto_destroy(b);
    // Wrong currently allowed static peer rejects a cryptographically valid IK
    // first message. A valid binding cannot authorize a different identity.
    a=plank_pencil_image_crypto_create(1,akey,bpub,hash); b=plank_pencil_image_crypto_create(0,bkey,opub,hash);
    assert(!plank_pencil_image_crypto_first(a,first,sizeof(first),&n));
    assert(plank_pencil_image_crypto_accept_first(b,first,n));
    plank_pencil_image_crypto_destroy(a); plank_pencil_image_crypto_destroy(b);
    a=plank_pencil_image_crypto_create(1,akey,opub,hash); b=plank_pencil_image_crypto_create(0,bkey,apub,hash);
    assert(!plank_pencil_image_crypto_first(a,first,sizeof(first),&n));
    assert(plank_pencil_image_crypto_accept_first(b,first,n));
    plank_pencil_image_crypto_destroy(a); plank_pencil_image_crypto_destroy(b);
    // Raw pen purpose cannot cross into image, even with both authentic keys.
    penA=plank_pencil_crypto_create(1,akey,bpub); b=plank_pencil_image_crypto_create(0,bkey,apub,hash);
    assert(!plank_pencil_crypto_first(penA,first,sizeof(first),&n));
    assert(plank_pencil_image_crypto_accept_first(b,first,n));
    plank_pencil_crypto_destroy(penA); plank_pencil_image_crypto_destroy(b);
    // Purpose must match in each authenticated handshake direction.
    PltrNoise raw; uint8_t p[128],peer[32],received[128]; size_t psize=0,read=0;
    purpose_payload(p,&psize,1);
    assert(!pltr_noise_init(&raw,PLTR_NOISE_INITIATOR,akey,bpub,2));
    assert(!pltr_noise_write_first(&raw,p,psize,first,sizeof(first),&n));
    b=plank_pencil_image_crypto_create(0,bkey,apub,hash);
    assert(plank_pencil_image_crypto_accept_first(b,first,n));
    pltr_noise_clear(&raw); plank_pencil_image_crypto_destroy(b);
    a=plank_pencil_image_crypto_create(1,akey,bpub,hash);
    assert(!plank_pencil_image_crypto_first(a,first,sizeof(first),&n));
    assert(!pltr_noise_init(&raw,PLTR_NOISE_RESPONDER,bkey,NULL,2));
    assert(!pltr_noise_read_first(&raw,first,n,peer,received,sizeof(received),&read));
    purpose_payload(p,&psize,1);
    assert(!pltr_noise_write_second(&raw,apub,p,psize,second,sizeof(second),&n));
    assert(plank_pencil_image_crypto_accept_second(a,second,n));
    pltr_noise_clear(&raw); plank_pencil_image_crypto_destroy(a);
    a=plank_pencil_image_crypto_create(1,akey,bpub,hash); b=plank_pencil_image_crypto_create(0,bkey,apub,hash); exchange(a,b);
    assert(plank_pencil_image_crypto_encrypt(a,plain,7681,cipher,sizeof(cipher),&c));
    assert(plank_pencil_image_crypto_encrypt(a,plain,1,cipher,sizeof(cipher),&c)); // fail-closed
    plank_pencil_image_crypto_destroy(a); plank_pencil_image_crypto_destroy(b);
    plank_pencil_crypto_destroy(penB);
    puts("Pinned Noise image purpose/binding/static-key/replay/size failure isolation and full pen transcript passed");
    return 0;
}
