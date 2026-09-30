// SHA-256, FIPS 180-4. Sixty lines, and the only thing that proves a download
// is the file its manifest entry named.
#include "installer/core.h"

#include <cstdio>
#include <cstring>

namespace coopiii::installer {
namespace {

constexpr uint32_t K[64] = {
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

uint32_t Ror(uint32_t x, int n) { return (x >> n) | (x << (32 - n)); }

struct Hasher {
	uint32_t state[8] = {0x6a09e667, 0xbb67ae85, 0x3c6ef372, 0xa54ff53a,
	                     0x510e527f, 0x9b05688c, 0x1f83d9ab, 0x5be0cd19};
	uint64_t bits      = 0;
	uint8_t  block[64] = {};
	size_t   filled    = 0;

	void Transform(const uint8_t *p) {
		uint32_t w[64];
		for (int i = 0; i < 16; ++i)
			w[i] = (static_cast<uint32_t>(p[i * 4]) << 24) |
			       (static_cast<uint32_t>(p[i * 4 + 1]) << 16) |
			       (static_cast<uint32_t>(p[i * 4 + 2]) << 8) |
			       static_cast<uint32_t>(p[i * 4 + 3]);
		for (int i = 16; i < 64; ++i) {
			const uint32_t s0 = Ror(w[i - 15], 7) ^ Ror(w[i - 15], 18) ^ (w[i - 15] >> 3);
			const uint32_t s1 = Ror(w[i - 2], 17) ^ Ror(w[i - 2], 19) ^ (w[i - 2] >> 10);
			w[i]              = w[i - 16] + s0 + w[i - 7] + s1;
		}

		uint32_t a = state[0], b = state[1], c = state[2], d = state[3];
		uint32_t e = state[4], f = state[5], g = state[6], h = state[7];
		for (int i = 0; i < 64; ++i) {
			const uint32_t S1    = Ror(e, 6) ^ Ror(e, 11) ^ Ror(e, 25);
			const uint32_t ch    = (e & f) ^ (~e & g);
			const uint32_t temp1 = h + S1 + ch + K[i] + w[i];
			const uint32_t S0    = Ror(a, 2) ^ Ror(a, 13) ^ Ror(a, 22);
			const uint32_t maj   = (a & b) ^ (a & c) ^ (b & c);
			const uint32_t temp2 = S0 + maj;

			h = g; g = f; f = e; e = d + temp1;
			d = c; c = b; b = a; a = temp1 + temp2;
		}
		state[0] += a; state[1] += b; state[2] += c; state[3] += d;
		state[4] += e; state[5] += f; state[6] += g; state[7] += h;
	}

	void Update(const uint8_t *data, size_t len) {
		bits += static_cast<uint64_t>(len) * 8;
		while (len > 0) {
			const size_t take = (64 - filled) < len ? (64 - filled) : len;
			std::memcpy(block + filled, data, take);
			filled += take;
			data += take;
			len -= take;
			if (filled == 64) {
				Transform(block);
				filled = 0;
			}
		}
	}

	std::string Finish() {
		const uint64_t total = bits;
		uint8_t        one   = 0x80;
		Update(&one, 1);
		const uint8_t zero = 0x00;
		while (filled != 56)
			Update(&zero, 1);
		for (int i = 7; i >= 0; --i) {
			const uint8_t byte = static_cast<uint8_t>((total >> (8 * i)) & 0xFF);
			block[filled++]    = byte;
		}
		Transform(block);

		char out[65];
		for (int i = 0; i < 8; ++i)
			std::snprintf(out + i * 8, 9, "%08x", state[i]);
		return std::string(out, 64);
	}
};

} // namespace

std::string Sha256(const void *data, size_t length) {
	Hasher h;
	h.Update(static_cast<const uint8_t *>(data), length);
	return h.Finish();
}

std::string Sha256File(const std::string &path) {
	FILE *fh = std::fopen(path.c_str(), "rb");
	if (!fh)
		return std::string();

	Hasher  h;
	uint8_t buf[64 * 1024];
	size_t  n;
	while ((n = std::fread(buf, 1, sizeof(buf), fh)) > 0)
		h.Update(buf, n);
	std::fclose(fh);
	return h.Finish();
}

} // namespace coopiii::installer
