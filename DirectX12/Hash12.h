// 3DMigoto DX12 - hashing helpers (portable, no Windows dependency)
#pragma once
#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>

#if defined(_MSC_VER) && defined(_M_X64)
#include <intrin.h>
#include <nmmintrin.h>
#define MIG12_HAVE_HW_CRC 1
#endif

namespace mig12 {

inline const uint32_t *crc32c_table()
{
	static const std::array<uint32_t, 256> t = []() {
		std::array<uint32_t, 256> a{};
		for (uint32_t i = 0; i < 256; i++) {
			uint32_t c = i;
			for (int k = 0; k < 8; k++)
				c = (c & 1) ? (c >> 1) ^ 0x82F63B78u : (c >> 1);
			a[i] = c;
		}
		return a;
	}();
	return t.data();
}

// Same convention as crc32c_append() in crc32c-hw: pre/post inverted, so
// crc32c(0, data) is the standard CRC-32C of data and calls can be chained.
inline uint32_t crc32c_sw(uint32_t crc, const void *data, size_t len)
{
	const uint32_t *t = crc32c_table();
	const uint8_t *p = (const uint8_t *)data;
	crc = ~crc;
	while (len--)
		crc = t[(crc ^ *p++) & 0xff] ^ (crc >> 8);
	return ~crc;
}

#ifdef MIG12_HAVE_HW_CRC
inline bool cpu_has_sse42()
{
	int r[4];
	__cpuid(r, 1);
	return (r[2] >> 20) & 1;
}
#endif

inline uint32_t crc32c(uint32_t crc, const void *data, size_t len)
{
#ifdef MIG12_HAVE_HW_CRC
	static const bool hw = cpu_has_sse42();
	if (hw) {
		const uint8_t *p = (const uint8_t *)data;
		crc = ~crc;
		while (len && ((uintptr_t)p & 7)) {
			crc = _mm_crc32_u8(crc, *p++);
			len--;
		}
		uint64_t c64 = crc;
		while (len >= 8) {
			c64 = _mm_crc32_u64(c64, *(const uint64_t *)p);
			p += 8;
			len -= 8;
		}
		crc = (uint32_t)c64;
		while (len--)
			crc = _mm_crc32_u8(crc, *p++);
		return ~crc;
	}
#endif
	return crc32c_sw(crc, data, len);
}

// Identical to fnv_64_buf() in 3DMigoto's util.h (FNV-1 with a zero offset
// basis) so that shader hashes of identical bytecode match the DX11 tool.
inline uint64_t fnv64(const void *buf, size_t len)
{
	uint64_t h = 0;
	const uint8_t *p = (const uint8_t *)buf;
	for (size_t i = 0; i < len; i++) {
		h *= 0x100000001b3ULL;
		h ^= (uint64_t)p[i];
	}
	return h;
}

} // namespace mig12
