#pragma once
#include <stddef.h>
#include <stdint.h>

typedef struct PlankPencilImageCrypto PlankPencilImageCrypto;
// Both roles require the currently approved pen peer and its full final64-byte
// transcript. Purpose + fixed32-byte BLAKE2b digest are authenticated in BOTH
// messages. No persistent approval, caller-selected purpose or raw bypass.
PlankPencilImageCrypto *plank_pencil_image_crypto_create(int initiator,
    const uint8_t private_key[32],const uint8_t peer_key[32],const uint8_t control_hash[64]);
void plank_pencil_image_crypto_destroy(PlankPencilImageCrypto *);
int plank_pencil_image_crypto_first(PlankPencilImageCrypto *,uint8_t *,size_t,size_t *);
int plank_pencil_image_crypto_accept_first(PlankPencilImageCrypto *,const uint8_t *,size_t);
int plank_pencil_image_crypto_second(PlankPencilImageCrypto *,uint8_t *,size_t,size_t *);
int plank_pencil_image_crypto_accept_second(PlankPencilImageCrypto *,const uint8_t *,size_t);
int plank_pencil_image_crypto_encrypt(PlankPencilImageCrypto *,const uint8_t *,size_t,uint8_t *,size_t,size_t *);
int plank_pencil_image_crypto_decrypt(PlankPencilImageCrypto *,const uint8_t *,size_t,uint8_t *,size_t,size_t *);
