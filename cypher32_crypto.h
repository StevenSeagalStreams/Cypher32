#pragma once
#include <Arduino.h>

// ─────────────────────────────────────────────
//  CYPHER32 — SHA-256 / HMAC-SHA256          T4.1
// ─────────────────────────────────────────────
//
//  Used to sign LoRa frames with a truncated 4-byte tag.
//
//  WHAT THIS IS FOR, AND WHAT IT IS NOT
//  The threat is a player with a spare SX1262 injecting HACK_RESULT frames to
//  hand themselves XP. A shared compiled-in secret stops that: you cannot forge
//  a tag without the key.
//
//  It is NOT real security. The key ships inside every device's firmware, so
//  anyone willing to dump flash can extract it and forge freely. There is no
//  key exchange and no per-device identity, because the premise of the game is
//  no infrastructure. Treat it as a lock on a garden gate: it stops the casual
//  case, which is the actual problem.
//
//  Implemented in portable C rather than calling mbedTLS so the exact same code
//  runs under the host test harness. At these packet rates the cost is
//  irrelevant — a few hundred microseconds per frame.

// ── SHA-256 ──────────────────────────────────
struct Sha256Ctx {
  uint32_t state[8];
  uint64_t bitlen;
  uint8_t  data[64];
  uint32_t datalen;
};

static const uint32_t _sha256K[64] = {
  0x428a2f98,0x71374491,0xb5c0fbcf,0xe9b5dba5,0x3956c25b,0x59f111f1,0x923f82a4,0xab1c5ed5,
  0xd807aa98,0x12835b01,0x243185be,0x550c7dc3,0x72be5d74,0x80deb1fe,0x9bdc06a7,0xc19bf174,
  0xe49b69c1,0xefbe4786,0x0fc19dc6,0x240ca1cc,0x2de92c6f,0x4a7484aa,0x5cb0a9dc,0x76f988da,
  0x983e5152,0xa831c66d,0xb00327c8,0xbf597fc7,0xc6e00bf3,0xd5a79147,0x06ca6351,0x14292967,
  0x27b70a85,0x2e1b2138,0x4d2c6dfc,0x53380d13,0x650a7354,0x766a0abb,0x81c2c92e,0x92722c85,
  0xa2bfe8a1,0xa81a664b,0xc24b8b70,0xc76c51a3,0xd192e819,0xd6990624,0xf40e3585,0x106aa070,
  0x19a4c116,0x1e376c08,0x2748774c,0x34b0bcb5,0x391c0cb3,0x4ed8aa4a,0x5b9cca4f,0x682e6ff3,
  0x748f82ee,0x78a5636f,0x84c87814,0x8cc70208,0x90befffa,0xa4506ceb,0xbef9a3f7,0xc67178f2
};

static inline uint32_t _rotr32(uint32_t x, int n) { return (x >> n) | (x << (32 - n)); }

static void _sha256Transform(Sha256Ctx* c, const uint8_t* d) {
  uint32_t m[64], a, b, e, f, g, h, t1, t2, cc;
  for (int i = 0, j = 0; i < 16; i++, j += 4)
    m[i] = ((uint32_t)d[j] << 24) | ((uint32_t)d[j+1] << 16) |
           ((uint32_t)d[j+2] << 8) | (uint32_t)d[j+3];
  for (int i = 16; i < 64; i++) {
    uint32_t s0 = _rotr32(m[i-15],7) ^ _rotr32(m[i-15],18) ^ (m[i-15] >> 3);
    uint32_t s1 = _rotr32(m[i-2],17) ^ _rotr32(m[i-2],19)  ^ (m[i-2] >> 10);
    m[i] = m[i-16] + s0 + m[i-7] + s1;
  }
  a=c->state[0]; b=c->state[1]; cc=c->state[2]; uint32_t dd=c->state[3];
  e=c->state[4]; f=c->state[5]; g=c->state[6]; h=c->state[7];
  for (int i = 0; i < 64; i++) {
    uint32_t S1 = _rotr32(e,6) ^ _rotr32(e,11) ^ _rotr32(e,25);
    uint32_t ch = (e & f) ^ ((~e) & g);
    t1 = h + S1 + ch + _sha256K[i] + m[i];
    uint32_t S0 = _rotr32(a,2) ^ _rotr32(a,13) ^ _rotr32(a,22);
    uint32_t mj = (a & b) ^ (a & cc) ^ (b & cc);
    t2 = S0 + mj;
    h=g; g=f; f=e; e=dd+t1; dd=cc; cc=b; b=a; a=t1+t2;
  }
  c->state[0]+=a; c->state[1]+=b; c->state[2]+=cc; c->state[3]+=dd;
  c->state[4]+=e; c->state[5]+=f; c->state[6]+=g; c->state[7]+=h;
}

inline void sha256Init(Sha256Ctx* c) {
  c->datalen = 0; c->bitlen = 0;
  c->state[0]=0x6a09e667; c->state[1]=0xbb67ae85;
  c->state[2]=0x3c6ef372; c->state[3]=0xa54ff53a;
  c->state[4]=0x510e527f; c->state[5]=0x9b05688c;
  c->state[6]=0x1f83d9ab; c->state[7]=0x5be0cd19;
}

inline void sha256Update(Sha256Ctx* c, const uint8_t* d, size_t len) {
  for (size_t i = 0; i < len; i++) {
    c->data[c->datalen++] = d[i];
    if (c->datalen == 64) { _sha256Transform(c, c->data); c->bitlen += 512; c->datalen = 0; }
  }
}

inline void sha256Final(Sha256Ctx* c, uint8_t* out) {
  uint32_t i = c->datalen;
  if (i < 56) { c->data[i++] = 0x80; while (i < 56) c->data[i++] = 0x00; }
  else        { c->data[i++] = 0x80; while (i < 64) c->data[i++] = 0x00;
                _sha256Transform(c, c->data); memset(c->data, 0, 56); }
  c->bitlen += (uint64_t)c->datalen * 8;
  for (int k = 0; k < 8; k++) c->data[63-k] = (uint8_t)(c->bitlen >> (8*k));
  _sha256Transform(c, c->data);
  for (int k = 0; k < 4; k++)
    for (int j = 0; j < 8; j++)
      out[k + j*4] = (uint8_t)((c->state[j] >> (24 - k*8)) & 0xFF);
}

inline void sha256(const uint8_t* d, size_t len, uint8_t* out) {
  Sha256Ctx c; sha256Init(&c); sha256Update(&c, d, len); sha256Final(&c, out);
}

// ── HMAC-SHA256 ──────────────────────────────
inline void hmacSha256(const uint8_t* key, size_t keyLen,
                       const uint8_t* msg, size_t msgLen, uint8_t* out32) {
  uint8_t k[64], ipad[64], opad[64], inner[32];
  memset(k, 0, sizeof(k));
  if (keyLen > 64) sha256(key, keyLen, k);
  else             memcpy(k, key, keyLen);
  for (int i = 0; i < 64; i++) { ipad[i] = k[i] ^ 0x36; opad[i] = k[i] ^ 0x5c; }

  Sha256Ctx c;
  sha256Init(&c); sha256Update(&c, ipad, 64); sha256Update(&c, msg, msgLen);
  sha256Final(&c, inner);
  sha256Init(&c); sha256Update(&c, opad, 64); sha256Update(&c, inner, 32);
  sha256Final(&c, out32);
}
