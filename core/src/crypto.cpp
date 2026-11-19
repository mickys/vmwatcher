#include <cstring>

#include "vmw.hpp"

namespace vmw {
namespace {

inline uint32_t rotr(uint32_t x, int n) { return (x >> n) | (x << (32 - n)); }
inline uint32_t rotl(uint32_t x, int n) { return (x << n) | (x >> (32 - n)); }

struct Sha256 {
  uint32_t h[8] = {0x6a09e667, 0xbb67ae85, 0x3c6ef372, 0xa54ff53a, 0x510e527f, 0x9b05688c, 0x1f83d9ab, 0x5be0cd19};
  uint8_t block[64];
  size_t used = 0;
  uint64_t total = 0;

  void compress(const uint8_t* p) {
    static const uint32_t k[64] = {
        0x428a2f98, 0x71374491, 0xb5c0fbcf, 0xe9b5dba5, 0x3956c25b, 0x59f111f1, 0x923f82a4, 0xab1c5ed5, 0xd807aa98, 0x12835b01, 0x243185be,
        0x550c7dc3, 0x72be5d74, 0x80deb1fe, 0x9bdc06a7, 0xc19bf174, 0xe49b69c1, 0xefbe4786, 0x0fc19dc6, 0x240ca1cc, 0x2de92c6f, 0x4a7484aa,
        0x5cb0a9dc, 0x76f988da, 0x983e5152, 0xa831c66d, 0xb00327c8, 0xbf597fc7, 0xc6e00bf3, 0xd5a79147, 0x06ca6351, 0x14292967, 0x27b70a85,
        0x2e1b2138, 0x4d2c6dfc, 0x53380d13, 0x650a7354, 0x766a0abb, 0x81c2c92e, 0x92722c85, 0xa2bfe8a1, 0xa81a664b, 0xc24b8b70, 0xc76c51a3,
        0xd192e819, 0xd6990624, 0xf40e3585, 0x106aa070, 0x19a4c116, 0x1e376c08, 0x2748774c, 0x34b0bcb5, 0x391c0cb3, 0x4ed8aa4a, 0x5b9cca4f,
        0x682e6ff3, 0x748f82ee, 0x78a5636f, 0x84c87814, 0x8cc70208, 0x90befffa, 0xa4506ceb, 0xbef9a3f7, 0xc67178f2};
    uint32_t w[64];
    for (int i = 0; i < 16; i++)
      w[i] = (uint32_t(p[i * 4]) << 24) | (uint32_t(p[i * 4 + 1]) << 16) | (uint32_t(p[i * 4 + 2]) << 8) | p[i * 4 + 3];
    for (int i = 16; i < 64; i++) {
      uint32_t s0 = rotr(w[i - 15], 7) ^ rotr(w[i - 15], 18) ^ (w[i - 15] >> 3);
      uint32_t s1 = rotr(w[i - 2], 17) ^ rotr(w[i - 2], 19) ^ (w[i - 2] >> 10);
      w[i] = w[i - 16] + s0 + w[i - 7] + s1;
    }
    uint32_t a = h[0], b = h[1], c = h[2], d = h[3], e = h[4], f = h[5], g = h[6], hh = h[7];
    for (int i = 0; i < 64; i++) {
      uint32_t t1 = hh + (rotr(e, 6) ^ rotr(e, 11) ^ rotr(e, 25)) + ((e & f) ^ (~e & g)) + k[i] + w[i];
      uint32_t t2 = (rotr(a, 2) ^ rotr(a, 13) ^ rotr(a, 22)) + ((a & b) ^ (a & c) ^ (b & c));
      hh = g; g = f; f = e; e = d + t1; d = c; c = b; b = a; a = t1 + t2;
    }
    h[0] += a; h[1] += b; h[2] += c; h[3] += d; h[4] += e; h[5] += f; h[6] += g; h[7] += hh;
  }
  void update(const void* data, size_t n) {
    const uint8_t* p = static_cast<const uint8_t*>(data);
    total += n;
    while (n) {
      size_t take = std::min(n, sizeof block - used);
      memcpy(block + used, p, take);
      used += take;
      p += take;
      n -= take;
      if (used == 64) {
        compress(block);
        used = 0;
      }
    }
  }
  void finish(uint8_t out[32]) {
    uint64_t bits = total * 8;
    uint8_t pad = 0x80;
    update(&pad, 1);
    uint8_t zero = 0;
    while (used != 56) update(&zero, 1);
    uint8_t length[8];
    for (int i = 0; i < 8; i++) length[i] = uint8_t(bits >> (56 - 8 * i));
    update(length, 8);
    for (int i = 0; i < 8; i++)
      for (int j = 0; j < 4; j++) out[i * 4 + j] = uint8_t(h[i] >> (24 - 8 * j));
  }
};

// HMAC with the padded-key states computed once, so PBKDF2 costs two
// compressions per iteration.
struct Hmac {
  Sha256 inner, outer;
  explicit Hmac(const std::string& key) {
    uint8_t k[64] = {0};
    if (key.size() > 64) {
      Sha256 s;
      s.update(key.data(), key.size());
      s.finish(k);
    } else
      memcpy(k, key.data(), key.size());
    uint8_t pad[64];
    for (int i = 0; i < 64; i++) pad[i] = k[i] ^ 0x36;
    inner.update(pad, 64);
    for (int i = 0; i < 64; i++) pad[i] = k[i] ^ 0x5c;
    outer.update(pad, 64);
  }
  void sign(const void* data, size_t n, uint8_t out[32]) const {
    Sha256 a = inner;
    a.update(data, n);
    uint8_t digest[32];
    a.finish(digest);
    Sha256 b = outer;
    b.update(digest, 32);
    b.finish(out);
  }
};

}  // namespace

std::string sha256(const std::string& data) {
  Sha256 s;
  s.update(data.data(), data.size());
  uint8_t out[32];
  s.finish(out);
  return std::string(reinterpret_cast<char*>(out), 32);
}

std::string hmacSha256(const std::string& key, const std::string& data) {
  uint8_t out[32];
  Hmac(key).sign(data.data(), data.size(), out);
  return std::string(reinterpret_cast<char*>(out), 32);
}

std::string pbkdf2Sha256(const std::string& password, const std::string& salt, uint32_t iterations, size_t length) {
  Hmac mac(password);
  std::string out;
  for (uint32_t index = 1; out.size() < length; index++) {
    std::string first = salt;
    for (int i = 3; i >= 0; i--) first += char(index >> (8 * i));
    uint8_t u[32], t[32];
    mac.sign(first.data(), first.size(), u);
    memcpy(t, u, 32);
    for (uint32_t i = 1; i < iterations; i++) {
      mac.sign(u, 32, u);
      for (int j = 0; j < 32; j++) t[j] ^= u[j];
    }
    out.append(reinterpret_cast<char*>(t), 32);
  }
  out.resize(length);
  return out;
}

Json hashPassword(const std::string& password, uint32_t iterations) {
  std::string salt = randomBytes(16);
  Json record = Json::Object{};
  record.set("algorithm", "pbkdf2-sha256");
  record.set("iterations", int64_t(iterations));
  record.set("salt", hex(salt));
  record.set("hash", hex(pbkdf2Sha256(password, salt, iterations)));
  return record;
}

static std::string unhex(const std::string& text) {
  std::string out;
  auto nibble = [](char c) { return c >= 'a' ? c - 'a' + 10 : c - '0'; };
  for (size_t i = 0; i + 1 < text.size(); i += 2) out += char(nibble(text[i]) * 16 + nibble(text[i + 1]));
  return out;
}

bool verifyPassword(const Json& record, const std::string& password) {
  int64_t iterations = record["iterations"].integer();
  if (record["algorithm"].str() != "pbkdf2-sha256" || iterations < 1000 || iterations > 10000000) return false;
  std::string expected = unhex(record["hash"].str());
  if (expected.size() != 32) return false;
  return constantEq(pbkdf2Sha256(password, unhex(record["salt"].str()), uint32_t(iterations)), expected);
}

std::string sha1(const std::string& data) {
  uint32_t h[5] = {0x67452301, 0xefcdab89, 0x98badcfe, 0x10325476, 0xc3d2e1f0};
  std::string message = data;
  uint64_t bits = uint64_t(data.size()) * 8;
  message += char(0x80);
  while (message.size() % 64 != 56) message += char(0);
  for (int i = 7; i >= 0; i--) message += char(bits >> (8 * i));
  for (size_t offset = 0; offset < message.size(); offset += 64) {
    uint32_t w[80];
    for (int i = 0; i < 16; i++) {
      const uint8_t* p = reinterpret_cast<const uint8_t*>(message.data()) + offset + i * 4;
      w[i] = (uint32_t(p[0]) << 24) | (uint32_t(p[1]) << 16) | (uint32_t(p[2]) << 8) | p[3];
    }
    for (int i = 16; i < 80; i++) w[i] = rotl(w[i - 3] ^ w[i - 8] ^ w[i - 14] ^ w[i - 16], 1);
    uint32_t a = h[0], b = h[1], c = h[2], d = h[3], e = h[4];
    for (int i = 0; i < 80; i++) {
      uint32_t f, k;
      if (i < 20) { f = (b & c) | (~b & d); k = 0x5a827999; }
      else if (i < 40) { f = b ^ c ^ d; k = 0x6ed9eba1; }
      else if (i < 60) { f = (b & c) | (b & d) | (c & d); k = 0x8f1bbcdc; }
      else { f = b ^ c ^ d; k = 0xca62c1d6; }
      uint32_t t = rotl(a, 5) + f + e + k + w[i];
      e = d; d = c; c = rotl(b, 30); b = a; a = t;
    }
    h[0] += a; h[1] += b; h[2] += c; h[3] += d; h[4] += e;
  }
  std::string out;
  for (uint32_t v : h)
    for (int i = 3; i >= 0; i--) out += char(v >> (8 * i));
  return out;
}

}  // namespace vmw
