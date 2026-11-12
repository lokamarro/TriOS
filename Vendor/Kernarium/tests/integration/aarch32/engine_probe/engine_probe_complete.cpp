// SPDX-License-Identifier: GPL-2.0-or-later
/* Group "complete": CRC32, AES, PMULL, SHA-1, SHA-256,
 * the dot product, half-precision arithmetic and FMLAL against known answers,
 * from A32 and from T32, for each extension the HWCAP words advertise. */
#include "engine_probe.hpp"

extern "C" {
#define CRC(name) probe::u32 name##_arm(probe::u32, const probe::u8 *, probe::u32); \
		  probe::u32 name##_thumb(probe::u32, const probe::u8 *, probe::u32);
CRC(crc32_b) CRC(crc32_h) CRC(crc32_w) CRC(crc32c_b) CRC(crc32c_h) CRC(crc32c_w)
#undef CRC
void aes_encrypt_arm(const probe::u8 *, const probe::u8 *, probe::u8 *);
void aes_encrypt_thumb(const probe::u8 *, const probe::u8 *, probe::u8 *);
void aes_decrypt_arm(const probe::u8 *, const probe::u8 *, probe::u8 *);
void aes_decrypt_thumb(const probe::u8 *, const probe::u8 *, probe::u8 *);
void aes_imc_arm(const probe::u8 *, probe::u8 *);
void aes_imc_thumb(const probe::u8 *, probe::u8 *);
void pmull64_arm(const probe::u64 *, probe::u64 *);
void pmull64_thumb(const probe::u64 *, probe::u64 *);
void sha1_schedule_arm(const probe::u32 *, probe::u32 *);
void sha1_schedule_thumb(const probe::u32 *, probe::u32 *);
void sha1_rounds_arm(probe::u32 *, const probe::u32 *, probe::u32, probe::u32);
void sha1_rounds_thumb(probe::u32 *, const probe::u32 *, probe::u32, probe::u32);
void sha256_schedule_arm(const probe::u32 *, probe::u32 *);
void sha256_schedule_thumb(const probe::u32 *, probe::u32 *);
void sha256_rounds_arm(probe::u32 *, const probe::u32 *, probe::u32);
void sha256_rounds_thumb(probe::u32 *, const probe::u32 *, probe::u32);
void dot_s8_arm(probe::u32 *, const probe::u8 *, const probe::u8 *);
void dot_s8_thumb(probe::u32 *, const probe::u8 *, const probe::u8 *);
void dot_u8_arm(probe::u32 *, const probe::u8 *, const probe::u8 *);
void dot_u8_thumb(probe::u32 *, const probe::u8 *, const probe::u8 *);
void dot_s8_lane_arm(probe::u32 *, const probe::u8 *, const probe::u8 *);
void dot_s8_lane_thumb(probe::u32 *, const probe::u8 *, const probe::u8 *);
probe::u32 fp16_add_arm(probe::u32, probe::u32);
probe::u32 fp16_add_thumb(probe::u32, probe::u32);
probe::u32 fp16_mul_arm(probe::u32, probe::u32);
probe::u32 fp16_mul_thumb(probe::u32, probe::u32);
void fp16_add_vec_arm(probe::u16 *, const probe::u16 *, const probe::u16 *);
void fp16_add_vec_thumb(probe::u16 *, const probe::u16 *, const probe::u16 *);
void fhm_fmlal_arm(probe::u32 *, probe::u32, probe::u32);
void fhm_fmlal_thumb(probe::u32 *, probe::u32, probe::u32);
}

namespace probe {
namespace {

const char *labelled(char *buffer, const char *prefix, const char *text)
{
	word length = 0;
	for (const char *p = prefix; *p; ++p)
		buffer[length++] = *p;
	for (const char *p = text; *p; ++p)
		buffer[length++] = *p;
	buffer[length] = 0;
	return buffer;
}

/* ---- AES-128 key expansion in software ---- */

u8 sbox[256];

u8 gf_mul(u8 a, u8 b)
{
	u8 product = 0;
	for (int bit = 0; bit < 8; ++bit) {
		if (b & 1)
			product ^= a;
		const bool carry = (a & 0x80) != 0;
		a = static_cast<u8>(a << 1);
		if (carry)
			a ^= 0x1b;
		b >>= 1;
	}
	return product;
}

void init_sbox()
{
	for (int x = 0; x < 256; ++x) {
		u8 inverse = 0;
		if (x) {
			/* x^254 is the inverse in GF(2^8). */
			u8 power = 1, base = static_cast<u8>(x);
			for (int exponent = 254; exponent; exponent >>= 1) {
				if (exponent & 1)
					power = gf_mul(power, base);
				base = gf_mul(base, base);
			}
			inverse = power;
		}
		auto rotl = [](u8 v, int n) { return static_cast<u8>((v << n) | (v >> (8 - n))); };
		sbox[x] = static_cast<u8>(inverse ^ rotl(inverse, 1) ^ rotl(inverse, 2) ^ rotl(inverse, 3) ^
					   rotl(inverse, 4) ^ 0x63);
	}
}

void expand_key(const u8 key[16], u8 schedule[176])
{
	for (int i = 0; i < 16; ++i)
		schedule[i] = key[i];
	u8 rcon = 1;
	for (int i = 16; i < 176; i += 4) {
		u8 t[4] = {schedule[i - 4], schedule[i - 3], schedule[i - 2], schedule[i - 1]};
		if (i % 16 == 0) {
			const u8 first = t[0];
			t[0] = static_cast<u8>(sbox[t[1]] ^ rcon);
			t[1] = sbox[t[2]];
			t[2] = sbox[t[3]];
			t[3] = sbox[first];
			rcon = gf_mul(rcon, 2);
		}
		for (int j = 0; j < 4; ++j)
			schedule[i + j] = static_cast<u8>(schedule[i - 16 + j] ^ t[j]);
	}
}

/* ---- SHA-1 and SHA-256 messages ---- */

constexpr u32 rotl32(u32 value, int n) { return (value << n) | (value >> (32 - n)); }
constexpr u32 rotr32(u32 value, int n) { return (value >> n) | (value << (32 - n)); }

const u32 sha256_k[64] = {
	0x428a2f98, 0x71374491, 0xb5c0fbcf, 0xe9b5dba5, 0x3956c25b, 0x59f111f1, 0x923f82a4, 0xab1c5ed5,
	0xd807aa98, 0x12835b01, 0x243185be, 0x550c7dc3, 0x72be5d74, 0x80deb1fe, 0x9bdc06a7, 0xc19bf174,
	0xe49b69c1, 0xefbe4786, 0x0fc19dc6, 0x240ca1cc, 0x2de92c6f, 0x4a7484aa, 0x5cb0a9dc, 0x76f988da,
	0x983e5152, 0xa831c66d, 0xb00327c8, 0xbf597fc7, 0xc6e00bf3, 0xd5a79147, 0x06ca6351, 0x14292967,
	0x27b70a85, 0x2e1b2138, 0x4d2c6dfc, 0x53380d13, 0x650a7354, 0x766a0abb, 0x81c2c92e, 0x92722c85,
	0xa2bfe8a1, 0xa81a664b, 0xc24b8b70, 0xc76c51a3, 0xd192e819, 0xd6990624, 0xf40e3585, 0x106aa070,
	0x19a4c116, 0x1e376c08, 0x2748774c, 0x34b0bcb5, 0x391c0cb3, 0x4ed8aa4a, 0x5b9cca4f, 0x682e6ff3,
	0x748f82ee, 0x78a5636f, 0x84c87814, 0x8cc70208, 0x90befffa, 0xa4506ceb, 0xbef9a3f7, 0xc67178f2,
};

/* The block of the message "abc" after padding, as big-endian words. */
void abc_block(u32 w[16])
{
	for (int i = 0; i < 16; ++i)
		w[i] = 0;
	w[0] = 0x61626380;
	w[15] = 24;
}

void sha1_abc(bool thumb, u32 digest[5], bool *schedule_ok)
{
	u32 w[80];
	abc_block(w);
	/* The software schedule, to compare with SHA1SU0/SU1. */
	u32 software[80];
	for (int i = 0; i < 16; ++i)
		software[i] = w[i];
	for (int i = 16; i < 80; ++i)
		software[i] = rotl32(software[i - 3] ^ software[i - 8] ^ software[i - 14] ^ software[i - 16], 1);
	u32 hardware[80];
	if (thumb)
		sha1_schedule_thumb(w, hardware);
	else
		sha1_schedule_arm(w, hardware);
	*schedule_ok = true;
	for (int i = 0; i < 80; ++i)
		if (hardware[i] != software[i])
			*schedule_ok = false;
	static const u32 k[4] = {0x5a827999, 0x6ed9eba1, 0x8f1bbcdc, 0xca62c1d6};
	u32 wk[80];
	for (int i = 0; i < 80; ++i)
		wk[i] = software[i] + k[i / 20];
	u32 state[5] = {0x67452301, 0xefcdab89, 0x98badcfe, 0x10325476, 0xc3d2e1f0};
	static const u32 kinds[4] = {0, 1, 2, 1};
	for (int part = 0; part < 4; ++part) {
		if (thumb)
			sha1_rounds_thumb(state, wk + part * 20, 5, kinds[part]);
		else
			sha1_rounds_arm(state, wk + part * 20, 5, kinds[part]);
	}
	static const u32 initial[5] = {0x67452301, 0xefcdab89, 0x98badcfe, 0x10325476, 0xc3d2e1f0};
	for (int i = 0; i < 5; ++i)
		digest[i] = state[i] + initial[i];
}

void sha256_abc(bool thumb, u32 digest[8], bool *schedule_ok)
{
	u32 software[64];
	abc_block(software);
	for (int i = 16; i < 64; ++i) {
		const u32 s0 = rotr32(software[i - 15], 7) ^ rotr32(software[i - 15], 18) ^ (software[i - 15] >> 3);
		const u32 s1 = rotr32(software[i - 2], 17) ^ rotr32(software[i - 2], 19) ^ (software[i - 2] >> 10);
		software[i] = software[i - 16] + s0 + software[i - 7] + s1;
	}
	u32 w[16];
	abc_block(w);
	u32 hardware[64];
	if (thumb)
		sha256_schedule_thumb(w, hardware);
	else
		sha256_schedule_arm(w, hardware);
	*schedule_ok = true;
	for (int i = 0; i < 64; ++i)
		if (hardware[i] != software[i])
			*schedule_ok = false;
	u32 wk[64];
	for (int i = 0; i < 64; ++i)
		wk[i] = software[i] + sha256_k[i];
	static const u32 initial[8] = {0x6a09e667, 0xbb67ae85, 0x3c6ef372, 0xa54ff53a,
				       0x510e527f, 0x9b05688c, 0x1f83d9ab, 0x5be0cd19};
	u32 state[8];
	for (int i = 0; i < 8; ++i)
		state[i] = initial[i];
	if (thumb)
		sha256_rounds_thumb(state, wk, 16);
	else
		sha256_rounds_arm(state, wk, 16);
	for (int i = 0; i < 8; ++i)
		digest[i] = state[i] + initial[i];
}

} // namespace

void group_complete()
{
	char name[160];
	static const char *const isa[2] = {"A32: ", "T32: "};

	/* CRC32 and CRC32C of "123456789". */
	if (hwcap2 & hwcap2_crc32) {
		static const u8 text[9] = {'1', '2', '3', '4', '5', '6', '7', '8', '9'};
		struct Crc {
			u32 (*function[2])(u32, const u8 *, u32);
			u32 want;
			const char *name;
		};
		static const Crc crcs[] = {
			{{crc32_b_arm, crc32_b_thumb}, 0xcbf43926, "CRC32B over the bytes of 123456789"},
			{{crc32_h_arm, crc32_h_thumb}, 0xcbf43926, "CRC32H over halfwords and a CRC32B tail"},
			{{crc32_w_arm, crc32_w_thumb}, 0xcbf43926, "CRC32W over words and a CRC32B tail"},
			{{crc32c_b_arm, crc32c_b_thumb}, 0xe3069283, "CRC32CB over the bytes of 123456789"},
			{{crc32c_h_arm, crc32c_h_thumb}, 0xe3069283, "CRC32CH over halfwords and a CRC32CB tail"},
			{{crc32c_w_arm, crc32c_w_thumb}, 0xe3069283, "CRC32CW over words and a CRC32CB tail"},
		};
		for (const Crc &crc : crcs)
			for (int t = 0; t < 2; ++t)
				check_eq(~crc.function[t](0xffffffff, text, 9), crc.want, labelled(name, isa[t], crc.name));
	}

	/* AES-128: the FIPS-197 example. */
	if (hwcap2 & hwcap2_aes) {
		init_sbox();
		static const u8 key[16] = {0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15};
		static const u8 plain[16] = {0x00, 0x11, 0x22, 0x33, 0x44, 0x55, 0x66, 0x77,
					     0x88, 0x99, 0xaa, 0xbb, 0xcc, 0xdd, 0xee, 0xff};
		static const u8 cipher[16] = {0x69, 0xc4, 0xe0, 0xd8, 0x6a, 0x7b, 0x04, 0x30,
					      0xd8, 0xcd, 0xb7, 0x80, 0x70, 0xb4, 0xc5, 0x5a};
		u8 schedule[176];
		expand_key(key, schedule);
		check_mem(schedule + 160, "\x13\x11\x1d\x7f\xe3\x94\x4a\x17\xf3\x07\xa7\x8b\x4d\x2b\x30\xc5", 16,
			  "the software AES key schedule ends with FIPS-197's last round key");
		for (int t = 0; t < 2; ++t) {
			u8 out[16] = {};
			(t ? aes_encrypt_thumb : aes_encrypt_arm)(schedule, plain, out);
			check_mem(out, cipher, 16, labelled(name, isa[t], "AESE/AESMC encrypt the FIPS-197 block"));
			u8 decryption[176];
			for (int round = 0; round < 11; ++round) {
				if (round == 0 || round == 10)
					for (int i = 0; i < 16; ++i)
						decryption[round * 16 + i] = schedule[round * 16 + i];
				else
					(t ? aes_imc_thumb : aes_imc_arm)(schedule + round * 16, decryption + round * 16);
			}
			u8 back[16] = {};
			(t ? aes_decrypt_thumb : aes_decrypt_arm)(decryption, cipher, back);
			check_mem(back, plain, 16, labelled(name, isa[t], "AESD/AESIMC decrypt it back"));
		}
	}

	/* VMULL.P64 */
	if (hwcap2 & hwcap2_pmull) {
		struct Vector {
			u64 a, b, hi, lo;
		};
		static const Vector vectors[] = {
			{3, 3, 0, 5},
			{~0ULL, ~0ULL, 0x5555555555555555ULL, 0x5555555555555555ULL},
			{0x8000000000000001ULL, 0x8000000000000001ULL, 0x4000000000000000ULL, 1},
			{0x123456789abcdef0ULL, 0xfedcba9876543210ULL, 0x0e038d8688850b04ULL, 0x0a0789828c810f00ULL},
			{0xdeadbeefcafebabeULL, 0x0123456789abcdefULL, 0x00c42fde8b6b5592ULL, 0xf099825fe2af618aULL},
		};
		for (int t = 0; t < 2; ++t) {
			u32 bad = 0;
			for (const Vector &vector : vectors) {
				alignas(8) u64 in[2] = {vector.a, vector.b};
				alignas(8) u64 out[2] = {};
				(t ? pmull64_thumb : pmull64_arm)(in, out);
				if (out[0] != vector.lo || out[1] != vector.hi)
					++bad;
			}
			check_eq(bad, 0, labelled(name, isa[t], "VMULL.P64 carry-less products"));
		}
	}

	/* SHA-1 of "abc" */
	if (hwcap2 & hwcap2_sha1) {
		static const u32 want[5] = {0xa9993e36, 0x4706816a, 0xba3e2571, 0x7850c26c, 0x9cd0d89d};
		for (int t = 0; t < 2; ++t) {
			u32 digest[5];
			bool schedule_ok = false;
			sha1_abc(t != 0, digest, &schedule_ok);
			check(schedule_ok, labelled(name, isa[t], "SHA1SU0/SHA1SU1 compute the message schedule"));
			check_mem(digest, want, sizeof(want), labelled(name, isa[t], "SHA1C/P/M/H compute SHA-1(abc)"));
		}
	}

	/* SHA-256 of "abc" */
	if (hwcap2 & hwcap2_sha2) {
		static const u32 want[8] = {0xba7816bf, 0x8f01cfea, 0x414140de, 0x5dae2223,
					    0xb00361a3, 0x96177a9c, 0xb410ff61, 0xf20015ad};
		for (int t = 0; t < 2; ++t) {
			u32 digest[8];
			bool schedule_ok = false;
			sha256_abc(t != 0, digest, &schedule_ok);
			check(schedule_ok, labelled(name, isa[t], "SHA256SU0/SHA256SU1 compute the message schedule"));
			check_mem(digest, want, sizeof(want), labelled(name, isa[t], "SHA256H/H2 compute SHA-256(abc)"));
		}
	}

	/* Dot product */
	if (hwcap & hwcap_asimddp) {
		static const u8 a[16] = {0xff, 0xfe, 3, 4, 0xff, 0xfe, 3, 4, 0xff, 0xfe, 3, 4, 0xff, 0xfe, 3, 4};
		static const u8 b[16] = {5, 6, 0xf9, 0xf8, 5, 6, 0xf9, 0xf8, 5, 6, 0xf9, 0xf8, 5, 6, 0xf9, 0xf8};
		for (int t = 0; t < 2; ++t) {
			u32 acc[4] = {100, 200, 300, 400};
			(t ? dot_s8_thumb : dot_s8_arm)(acc, a, b);
			check(acc[0] == 30 && acc[1] == 130 && acc[2] == 230 && acc[3] == 330,
			      labelled(name, isa[t], "VSDOT.S8 accumulates signed byte products"));
			u32 uacc[4] = {100, 200, 300, 400};
			(t ? dot_u8_thumb : dot_u8_arm)(uacc, a, b);
			check(uacc[0] == 4638 && uacc[1] == 4738 && uacc[2] == 4838 && uacc[3] == 4938,
			      labelled(name, isa[t], "VUDOT.U8 accumulates unsigned byte products"));
			u8 la[16], lb[16];
			for (int i = 0; i < 16; ++i) {
				la[i] = static_cast<u8>(i / 4 + 1 + i % 4);
				lb[i] = static_cast<u8>(i < 4 ? 1 : i < 8 ? 2 + (i - 4) : 9);
			}
			u32 lane[4] = {0, 0, 0, 0};
			(t ? dot_s8_lane_thumb : dot_s8_lane_arm)(lane, la, lb);
			check(lane[0] == 40 && lane[1] == 54 && lane[2] == 68 && lane[3] == 82,
			      labelled(name, isa[t], "VSDOT.S8 by element uses the selected group of four"));
		}
	}

	/* Half-precision arithmetic */
	if (hwcap & hwcap_fphp) {
		for (int t = 0; t < 2; ++t) {
			check_eq((t ? fp16_add_thumb : fp16_add_arm)(0x3c00, 0x4000), 0x4200,
				 labelled(name, isa[t], "VADD.F16 1.0 + 2.0 is 3.0"));
			check_eq((t ? fp16_mul_thumb : fp16_mul_arm)(0x4000, 0x4200), 0x4600,
				 labelled(name, isa[t], "VMUL.F16 2.0 * 3.0 is 6.0"));
		}
	}
	if (hwcap & hwcap_asimdhp) {
		static const u16 a[4] = {0x3c00, 0x4000, 0x4200, 0x4400};
		static const u16 b[4] = {0x3800, 0x3800, 0x3800, 0x3800};
		static const u16 want[4] = {0x3e00, 0x4100, 0x4300, 0x4480};
		for (int t = 0; t < 2; ++t) {
			alignas(8) u16 out[4] = {};
			(t ? fp16_add_vec_thumb : fp16_add_vec_arm)(out, a, b);
			check_mem(out, want, sizeof(want), labelled(name, isa[t], "VADD.F16 on four lanes"));
		}
	}
	if (hwcap & hwcap_asimdfhm) {
		for (int t = 0; t < 2; ++t) {
			alignas(8) u32 acc[2] = {0x3f800000, 0x40000000};
			(t ? fhm_fmlal_thumb : fhm_fmlal_arm)(acc, 0x42004000, 0x45004400);
			check(acc[0] == 0x41100000 && acc[1] == 0x41880000,
			      labelled(name, isa[t], "VFMAL.F16 widens the products into single precision"));
		}
	}
}

} // namespace probe
