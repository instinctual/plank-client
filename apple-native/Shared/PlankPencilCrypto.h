#pragma once
#include <stddef.h>
#include <stdint.h>
typedef struct PlankPencilCrypto PlankPencilCrypto;
int plank_pencil_crypto_public_key(const uint8_t private_key[32], uint8_t public_key[32]);
PlankPencilCrypto *plank_pencil_crypto_create(int initiator, const uint8_t private_key[32], const uint8_t peer[32]);
void plank_pencil_crypto_destroy(PlankPencilCrypto *);
// Fixed v3 normalized-pen + finite keyboard capability in BOTH handshake
// messages; v1/v2 pins cannot broaden into it. This adapter is
// not a raw PLTR link and cannot admit its empty handshake payloads.
int plank_pencil_crypto_first(PlankPencilCrypto *, uint8_t *, size_t, size_t *);
int plank_pencil_crypto_accept_first(PlankPencilCrypto *, const uint8_t *, size_t);
int plank_pencil_crypto_approve(PlankPencilCrypto *, uint8_t *, size_t, size_t *);
int plank_pencil_crypto_accept_second(PlankPencilCrypto *, const uint8_t *, size_t);
int plank_pencil_crypto_code(PlankPencilCrypto *, char code[13]);
int plank_pencil_crypto_peer(PlankPencilCrypto *, uint8_t key[32]);
int plank_pencil_crypto_encrypt(PlankPencilCrypto *, const uint8_t *, size_t, uint8_t *, size_t, size_t *);
int plank_pencil_crypto_decrypt(PlankPencilCrypto *, const uint8_t *, size_t, uint8_t *, size_t, size_t *);
