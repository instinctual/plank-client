#include "PlankPencilCrypto.h"
#include "noise.h"
#include <assert.h>
#include <string.h>
#include <stdio.h>
int main(void) {
    uint8_t a[32]={1},b[32]={2},pk[32],first[256],second[256],cipher[256],plain[256];
    size_t n=0,m=0,k=0; char acode[13],bcode[13];
    assert(!plank_pencil_crypto_public_key(b,pk));
    PlankPencilCrypto *sender=plank_pencil_crypto_create(1,a,pk),*receiver=plank_pencil_crypto_create(0,b,NULL);
    assert(sender && receiver);
    assert(!plank_pencil_crypto_first(sender,first,sizeof(first),&n));
    assert(!plank_pencil_crypto_accept_first(receiver,first,n));
    assert(!plank_pencil_crypto_code(sender,acode) && !plank_pencil_crypto_code(receiver,bcode));
    assert(!memcmp(acode,bcode,13));
    assert(!plank_pencil_crypto_approve(receiver,second,sizeof(second),&m));
    assert(!plank_pencil_crypto_accept_second(sender,second,m));
    const uint8_t message[]={0x50,0x4c,0x50,0x4e,3,4};
    assert(!plank_pencil_crypto_encrypt(sender,message,sizeof(message),cipher,sizeof(cipher),&k));
    assert(!plank_pencil_crypto_decrypt(receiver,cipher,k,plain,sizeof(plain),&m));
    assert(m==sizeof(message) && !memcmp(message,plain,m));
    // Authenticated record replay must fail and latch the connection closed.
    assert(plank_pencil_crypto_decrypt(receiver,cipher,k,plain,sizeof(plain),&m)<0);
    assert(plank_pencil_crypto_approve(receiver,second,sizeof(second),&m)<0);
    plank_pencil_crypto_destroy(sender); plank_pencil_crypto_destroy(receiver);
    // Empty raw drawing handshake is not this capability, despite reuse of IK.
    PltrNoise raw; assert(!pltr_noise_init(&raw,PLTR_NOISE_INITIATOR,a,pk,2));
    assert(!pltr_noise_write_first(&raw,NULL,0,first,sizeof(first),&n));
    receiver=plank_pencil_crypto_create(0,b,NULL); assert(receiver);
    assert(plank_pencil_crypto_accept_first(receiver,first,n)<0);
    plank_pencil_crypto_destroy(receiver); pltr_noise_clear(&raw);
    // Neither pen-only v1 nor five-modifier v2 authority approves v3 keys.
    const char *old_purposes[]={"PLANK-NORMALIZED-PEN/1","PLANK-NORMALIZED-PEN/2"};
    for(size_t i=0;i<2;i++) {
        assert(!pltr_noise_init(&raw,PLTR_NOISE_INITIATOR,a,pk,2));
        assert(!pltr_noise_write_first(&raw,(const uint8_t*)old_purposes[i],strlen(old_purposes[i]),first,sizeof(first),&n));
        receiver=plank_pencil_crypto_create(0,b,NULL); assert(receiver);
        assert(plank_pencil_crypto_accept_first(receiver,first,n)<0);
        plank_pencil_crypto_destroy(receiver); pltr_noise_clear(&raw);
    }
    // Wrong purpose with the SAME length cannot negotiate this service.
    uint8_t purpose[sizeof("PLANK-NORMALIZED-PEN-KEYS/3")-1]={0}; assert(!pltr_noise_init(&raw,PLTR_NOISE_INITIATOR,a,pk,2));
    assert(!pltr_noise_write_first(&raw,purpose,sizeof(purpose),first,sizeof(first),&n));
    receiver=plank_pencil_crypto_create(0,b,NULL);
    assert(plank_pencil_crypto_accept_first(receiver,first,n)<0);
    plank_pencil_crypto_destroy(receiver); pltr_noise_clear(&raw);
    // Before physical approval no application record can be decrypted.
    receiver=plank_pencil_crypto_create(0,b,NULL);
    assert(plank_pencil_crypto_decrypt(receiver,cipher,k,plain,sizeof(plain),&m)<0);
    plank_pencil_crypto_destroy(receiver);
    puts("Pinned IK reuse, transcript comparison, authenticated purpose, replay and preapproval rejection passed");
}
