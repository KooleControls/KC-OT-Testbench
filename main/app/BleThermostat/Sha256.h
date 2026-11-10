#pragma once

#include <cstddef>
#include <cstdint>
#include <cstring>

// ──────────────────────────────────────────────────────────────
// SHA-256 (FIPS 180-4), in software, for the image a gateway pushes to the
// simulated thermostat.
//
// Not mbedTLS: from ESP-IDF 6 its hash is reached through PSA, and on the ESP32 an
// operation that started on the SHA engine holds the engine until it finishes -- for
// the minutes a push takes, pushing every TLS handshake onto the software fallback.
// This one holds nothing, and its state is a plain struct, so the state before a
// block can be kept and put back when the gateway writes the block again. A push
// moves a few KB a second over BLE; this hashes a few MB a second.
// ──────────────────────────────────────────────────────────────
class Sha256
{
public:
    static constexpr size_t DIGEST_LEN = 32;

    Sha256() { Reset(); }

    void Reset()
    {
        static constexpr uint32_t INIT[8] = {
            0x6a09e667, 0xbb67ae85, 0x3c6ef372, 0xa54ff53a,
            0x510e527f, 0x9b05688c, 0x1f83d9ab, 0x5be0cd19,
        };
        memcpy(h_, INIT, sizeof(h_));
        total_ = 0;
        used_ = 0;
    }

    void Update(const uint8_t* data, size_t len)
    {
        total_ += len;
        while (len > 0)
        {
            const size_t take = (len < 64 - used_) ? len : 64 - used_;
            memcpy(block_ + used_, data, take);
            used_ += take;
            data += take;
            len -= take;
            if (used_ == 64)
            {
                Compress(block_);
                used_ = 0;
            }
        }
    }

    /// The digest of everything so far. Leaves this state as it was, so hashing can
    /// go on and the digest be taken again.
    void Digest(uint8_t out[DIGEST_LEN]) const
    {
        Sha256 t = *this;
        const uint64_t bits = t.total_ * 8;
        const uint8_t pad = 0x80;
        t.Update(&pad, 1);
        const uint8_t zero = 0;
        while (t.used_ != 56)
            t.Update(&zero, 1);
        uint8_t length[8];
        for (int i = 0; i < 8; ++i)
            length[i] = static_cast<uint8_t>(bits >> (56 - 8 * i));
        t.Update(length, sizeof(length));
        for (int i = 0; i < 8; ++i)
        {
            out[4 * i + 0] = static_cast<uint8_t>(t.h_[i] >> 24);
            out[4 * i + 1] = static_cast<uint8_t>(t.h_[i] >> 16);
            out[4 * i + 2] = static_cast<uint8_t>(t.h_[i] >> 8);
            out[4 * i + 3] = static_cast<uint8_t>(t.h_[i]);
        }
    }

    /// The digest as 64 lowercase hex characters; `out` holds 65.
    void DigestHex(char* out) const
    {
        uint8_t d[DIGEST_LEN];
        Digest(d);
        static constexpr char HEX[] = "0123456789abcdef";
        for (size_t i = 0; i < DIGEST_LEN; ++i)
        {
            out[2 * i] = HEX[d[i] >> 4];
            out[2 * i + 1] = HEX[d[i] & 0x0F];
        }
        out[2 * DIGEST_LEN] = '\0';
    }

private:
    uint32_t h_[8];
    uint64_t total_;
    size_t used_;
    uint8_t block_[64];

    static uint32_t Rotr(uint32_t x, int n) { return (x >> n) | (x << (32 - n)); }

    void Compress(const uint8_t* p)
    {
        static constexpr uint32_t K[64] = {
            0x428a2f98, 0x71374491, 0xb5c0fbcf, 0xe9b5dba5, 0x3956c25b, 0x59f111f1, 0x923f82a4, 0xab1c5ed5,
            0xd807aa98, 0x12835b01, 0x243185be, 0x550c7dc3, 0x72be5d74, 0x80deb1fe, 0x9bdc06a7, 0xc19bf174,
            0xe49b69c1, 0xefbe4786, 0x0fc19dc6, 0x240ca1cc, 0x2de92c6f, 0x4a7484aa, 0x5cb0a9dc, 0x76f988da,
            0x983e5152, 0xa831c66d, 0xb00327c8, 0xbf597fc7, 0xc6e00bf3, 0xd5a79147, 0x06ca6351, 0x14292967,
            0x27b70a85, 0x2e1b2138, 0x4d2c6dfc, 0x53380d13, 0x650a7354, 0x766a0abb, 0x81c2c92e, 0x92722c85,
            0xa2bfe8a1, 0xa81a664b, 0xc24b8b70, 0xc76c51a3, 0xd192e819, 0xd6990624, 0xf40e3585, 0x106aa070,
            0x19a4c116, 0x1e376c08, 0x2748774c, 0x34b0bcb5, 0x391c0cb3, 0x4ed8aa4a, 0x5b9cca4f, 0x682e6ff3,
            0x748f82ee, 0x78a5636f, 0x84c87814, 0x8cc70208, 0x90befffa, 0xa4506ceb, 0xbef9a3f7, 0xc67178f2,
        };

        uint32_t w[64];
        for (int i = 0; i < 16; ++i)
            w[i] = (static_cast<uint32_t>(p[4 * i]) << 24) | (static_cast<uint32_t>(p[4 * i + 1]) << 16)
                 | (static_cast<uint32_t>(p[4 * i + 2]) << 8) | static_cast<uint32_t>(p[4 * i + 3]);
        for (int i = 16; i < 64; ++i)
        {
            const uint32_t s0 = Rotr(w[i - 15], 7) ^ Rotr(w[i - 15], 18) ^ (w[i - 15] >> 3);
            const uint32_t s1 = Rotr(w[i - 2], 17) ^ Rotr(w[i - 2], 19) ^ (w[i - 2] >> 10);
            w[i] = w[i - 16] + s0 + w[i - 7] + s1;
        }

        uint32_t a = h_[0], b = h_[1], c = h_[2], d = h_[3], e = h_[4], f = h_[5], g = h_[6], h = h_[7];
        for (int i = 0; i < 64; ++i)
        {
            const uint32_t t1 = h + (Rotr(e, 6) ^ Rotr(e, 11) ^ Rotr(e, 25)) + ((e & f) ^ (~e & g)) + K[i] + w[i];
            const uint32_t t2 = (Rotr(a, 2) ^ Rotr(a, 13) ^ Rotr(a, 22)) + ((a & b) ^ (a & c) ^ (b & c));
            h = g; g = f; f = e; e = d + t1;
            d = c; c = b; b = a; a = t1 + t2;
        }
        h_[0] += a; h_[1] += b; h_[2] += c; h_[3] += d;
        h_[4] += e; h_[5] += f; h_[6] += g; h_[7] += h;
    }
};
