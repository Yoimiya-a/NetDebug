#pragma once

#include <algorithm>
#include <cstdint>
#include <cstring>

// 紧凑 SHA-256 / HMAC，供口令校验使用。启动时用已知向量自检。

namespace ndbg {

inline uint32_t rotr(uint32_t x, int n) {
  return (x >> n) | (x << (32 - n));
}

inline void sha256_transform(uint32_t s[8], const uint8_t block[64]) {
  static const uint32_t K[64] = {
      0x428a2f98, 0x71374491, 0xb5c0fbcf, 0xe9b5dba5, 0x3956c25b, 0x59f111f1, 0x923f82a4,
      0xab1c5ed5, 0xd807aa98, 0x12835b01, 0x243185be, 0x550c7dc3, 0x72be5d74, 0x80deb1fe,
      0x9bdc06a7, 0xc19bf174, 0xe49b69c1, 0xefbe4786, 0x0fc19dc6, 0x240ca1cc, 0x2de92c6f,
      0x4a7484aa, 0x5cb0a9dc, 0x76f988da, 0x983e5152, 0xa831c66d, 0xb00327c8, 0xbf597fc7,
      0xc6e00bf3, 0xd5a79147, 0x06ca6351, 0x14292967, 0x27b70a85, 0x2e1b2138, 0x4d2c6dfc,
      0x53380d13, 0x650a7354, 0x766a0abb, 0x81c2c92e, 0x92722c85, 0xa2bfe8a1, 0xa81a664b,
      0xc24b8b70, 0xc76c51a3, 0xd192e819, 0xd6990624, 0xf40e3585, 0x106aa070, 0x19a4c116,
      0x1e376c08, 0x2748774c, 0x34b0bcb5, 0x391c0cb3, 0x4ed8aa4a, 0x5b9cca4f, 0x682e6ff3,
      0x748f82ee, 0x78a5636f, 0x84c87814, 0x8cc70208, 0x90befffa, 0xa4506ceb, 0xbef9a3f7,
      0xc67178f2};

  uint32_t w[64];
  for (int i = 0; i < 16; ++i) {
    w[i] = (uint32_t)block[i * 4] << 24 | (uint32_t)block[i * 4 + 1] << 16 |
           (uint32_t)block[i * 4 + 2] << 8 | (uint32_t)block[i * 4 + 3];
  }
  for (int i = 16; i < 64; ++i) {
    const uint32_t s0 = rotr(w[i - 15], 7) ^ rotr(w[i - 15], 18) ^ (w[i - 15] >> 3);
    const uint32_t s1 = rotr(w[i - 2], 17) ^ rotr(w[i - 2], 19) ^ (w[i - 2] >> 10);
    w[i] = w[i - 16] + s0 + w[i - 7] + s1;
  }

  uint32_t a = s[0], b = s[1], c = s[2], d = s[3], e = s[4], f = s[5], g = s[6], h = s[7];
  for (int i = 0; i < 64; ++i) {
    const uint32_t S1 = rotr(e, 6) ^ rotr(e, 11) ^ rotr(e, 25);
    const uint32_t ch = (e & f) ^ ((~e) & g);
    const uint32_t t1 = h + S1 + ch + K[i] + w[i];
    const uint32_t S0 = rotr(a, 2) ^ rotr(a, 13) ^ rotr(a, 22);
    const uint32_t maj = (a & b) ^ (a & c) ^ (b & c);
    const uint32_t t2 = S0 + maj;
    h = g;
    g = f;
    f = e;
    e = d + t1;
    d = c;
    c = b;
    b = a;
    a = t1 + t2;
  }
  s[0] += a;
  s[1] += b;
  s[2] += c;
  s[3] += d;
  s[4] += e;
  s[5] += f;
  s[6] += g;
  s[7] += h;
}

struct Sha256 {
  uint32_t s[8];
  uint64_t bits = 0;
  uint8_t buf[64]{};
  size_t n = 0;

  Sha256() {
    s[0] = 0x6a09e667;
    s[1] = 0xbb67ae85;
    s[2] = 0x3c6ef372;
    s[3] = 0xa54ff53a;
    s[4] = 0x510e527f;
    s[5] = 0x9b05688c;
    s[6] = 0x1f83d9ab;
    s[7] = 0x5be0cd19;
  }

  void update(const uint8_t* data, size_t len) {
    bits += (uint64_t)len * 8;
    while (len > 0) {
      const size_t take = std::min(len, (size_t)64 - n);
      std::memcpy(buf + n, data, take);
      n += take;
      data += take;
      len -= take;
      if (n == 64) {
        sha256_transform(s, buf);
        n = 0;
      }
    }
  }

  void final(uint8_t out[32]) {
    buf[n++] = 0x80;
    if (n > 56) {
      while (n < 64) buf[n++] = 0;
      sha256_transform(s, buf);
      n = 0;
    }
    while (n < 56) buf[n++] = 0;
    for (int i = 7; i >= 0; --i) buf[n++] = (uint8_t)(bits >> (i * 8));
    sha256_transform(s, buf);
    for (int i = 0; i < 8; ++i) {
      out[i * 4] = (uint8_t)(s[i] >> 24);
      out[i * 4 + 1] = (uint8_t)(s[i] >> 16);
      out[i * 4 + 2] = (uint8_t)(s[i] >> 8);
      out[i * 4 + 3] = (uint8_t)s[i];
    }
  }
};

inline void sha256(const uint8_t* data, size_t len, uint8_t out[32]) {
  Sha256 h;
  h.update(data, len);
  h.final(out);
}

inline void hmac_sha256(const uint8_t* key, size_t key_len, const uint8_t* msg, size_t msg_len,
                        uint8_t out[32]) {
  uint8_t k[64]{};
  if (key_len > 64) {
    sha256(key, key_len, k);
  } else if (key_len > 0) {
    std::memcpy(k, key, key_len);
  }
  uint8_t ipad[64], opad[64];
  for (int i = 0; i < 64; ++i) {
    ipad[i] = (uint8_t)(k[i] ^ 0x36);
    opad[i] = (uint8_t)(k[i] ^ 0x5c);
  }
  Sha256 inner;
  inner.update(ipad, 64);
  inner.update(msg, msg_len);
  uint8_t inner_hash[32];
  inner.final(inner_hash);
  Sha256 outer;
  outer.update(opad, 64);
  outer.update(inner_hash, 32);
  outer.final(out);
}

inline bool ct_equal(const uint8_t* a, const uint8_t* b, size_t n) {
  uint8_t diff = 0;
  for (size_t i = 0; i < n; ++i) diff = (uint8_t)(diff | (a[i] ^ b[i]));
  return diff == 0;
}

inline bool sha256_self_test() {
  uint8_t out[32];
  static const uint8_t empty_hash[32] = {
      0xe3, 0xb0, 0xc4, 0x42, 0x98, 0xfc, 0x1c, 0x14, 0x9a, 0xfb, 0xf4, 0xc8, 0x99, 0x6f, 0xb9, 0x24,
      0x27, 0xae, 0x41, 0xe4, 0x64, 0x9b, 0x93, 0x4c, 0xa4, 0x95, 0x99, 0x1b, 0x78, 0x52, 0xb8, 0x55};
  sha256(nullptr, 0, out);
  if (!ct_equal(out, empty_hash, 32)) return false;

  const auto* abc = reinterpret_cast<const uint8_t*>("abc");
  static const uint8_t abc_hash[32] = {
      0xba, 0x78, 0x16, 0xbf, 0x8f, 0x01, 0xcf, 0xea, 0x41, 0x41, 0x40, 0xde, 0x5d, 0xae, 0x22, 0x23,
      0xb0, 0x03, 0x61, 0xa3, 0x96, 0x17, 0x7a, 0x9c, 0xb4, 0x10, 0xff, 0x61, 0xf2, 0x00, 0x15, 0xad};
  sha256(abc, 3, out);
  if (!ct_equal(out, abc_hash, 32)) return false;

  uint8_t key[20];
  std::memset(key, 0x0b, sizeof(key));
  const auto* hi = reinterpret_cast<const uint8_t*>("Hi There");
  static const uint8_t hmac_hash[32] = {
      0xb0, 0x34, 0x4c, 0x61, 0xd8, 0xdb, 0x38, 0x53, 0x5c, 0xa8, 0xaf, 0xce, 0xaf, 0x0b, 0xf1, 0x2b,
      0x88, 0x1d, 0xc2, 0x00, 0xc9, 0x83, 0x3d, 0xa7, 0x26, 0xe9, 0x37, 0x6c, 0x2e, 0x32, 0xcf, 0xf7};
  hmac_sha256(key, 20, hi, 8, out);
  return ct_equal(out, hmac_hash, 32);
}

}  // namespace ndbg
