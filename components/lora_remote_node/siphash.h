#pragma once

#include <cstddef>
#include <cstdint>

// SipHash-2-4: a fast, secure pseudorandom function
// Reference: https://131002.net/siphash/
// Used for lightweight packet authentication with a 128-bit pre-shared key.
// We truncate the 64-bit output to 16 bits for a compact auth tag.

namespace esphome {
namespace lora_protocol {

inline uint64_t rotl64(uint64_t v, int b) { return (v << b) | (v >> (64 - b)); }

inline uint64_t load_le64(const uint8_t *p) {
  return static_cast<uint64_t>(p[0]) | (static_cast<uint64_t>(p[1]) << 8) | (static_cast<uint64_t>(p[2]) << 16) |
         (static_cast<uint64_t>(p[3]) << 24) | (static_cast<uint64_t>(p[4]) << 32) |
         (static_cast<uint64_t>(p[5]) << 40) | (static_cast<uint64_t>(p[6]) << 48) |
         (static_cast<uint64_t>(p[7]) << 56);
}

inline void sipround(uint64_t &v0, uint64_t &v1, uint64_t &v2, uint64_t &v3) {
  v0 += v1;
  v1 = rotl64(v1, 13);
  v1 ^= v0;
  v0 = rotl64(v0, 32);
  v2 += v3;
  v3 = rotl64(v3, 16);
  v3 ^= v2;
  v0 += v3;
  v3 = rotl64(v3, 21);
  v3 ^= v0;
  v2 += v1;
  v1 = rotl64(v1, 17);
  v1 ^= v2;
  v2 = rotl64(v2, 32);
}

inline uint64_t siphash_2_4(const uint8_t key[16], const uint8_t *data, size_t len) {
  uint64_t k0 = load_le64(key);
  uint64_t k1 = load_le64(key + 8);

  uint64_t v0 = k0 ^ UINT64_C(0x736f6d6570736575);
  uint64_t v1 = k1 ^ UINT64_C(0x646f72616e646f6d);
  uint64_t v2 = k0 ^ UINT64_C(0x6c7967656e657261);
  uint64_t v3 = k1 ^ UINT64_C(0x7465646279746573);

  const size_t block_count = len / 8;
  const uint8_t *end = data + block_count * 8;

  // Process full 8-byte blocks
  for (const uint8_t *p = data; p < end; p += 8) {
    uint64_t m = load_le64(p);
    v3 ^= m;
    sipround(v0, v1, v2, v3);
    sipround(v0, v1, v2, v3);
    v0 ^= m;
  }

  // Process remaining bytes with length tag in high byte
  uint64_t m = static_cast<uint64_t>(len) << 56;
  switch (len & 7) {
    case 7:
      m |= static_cast<uint64_t>(end[6]) << 48;
      // fallthrough
    case 6:
      m |= static_cast<uint64_t>(end[5]) << 40;
      // fallthrough
    case 5:
      m |= static_cast<uint64_t>(end[4]) << 32;
      // fallthrough
    case 4:
      m |= static_cast<uint64_t>(end[3]) << 24;
      // fallthrough
    case 3:
      m |= static_cast<uint64_t>(end[2]) << 16;
      // fallthrough
    case 2:
      m |= static_cast<uint64_t>(end[1]) << 8;
      // fallthrough
    case 1:
      m |= static_cast<uint64_t>(end[0]);
      // fallthrough
    case 0:
      break;
  }

  v3 ^= m;
  sipround(v0, v1, v2, v3);
  sipround(v0, v1, v2, v3);
  v0 ^= m;

  // Finalization
  v2 ^= 0xff;
  sipround(v0, v1, v2, v3);
  sipround(v0, v1, v2, v3);
  sipround(v0, v1, v2, v3);
  sipround(v0, v1, v2, v3);

  return v0 ^ v1 ^ v2 ^ v3;
}

// Compute a 16-bit truncated authentication tag
inline uint16_t compute_auth_tag(const uint8_t key[16], const uint8_t *data, size_t len) {
  return static_cast<uint16_t>(siphash_2_4(key, data, len) & 0xFFFF);
}

}  // namespace lora_protocol
}  // namespace esphome
