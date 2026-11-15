/* bench.c - can an 8088 do TLS? Checks and times the BearSSL pieces a TLS 1.2
 * handshake and record layer need. Builds for 16-bit DOS (8086 and up), for
 * 32-bit DOS (386 and up, with a DOS extender) and natively (gcc), so the
 * outputs can be compared. "make dos-bench" in the top directory builds the
 * DOS versions; see README.md, "MS-DOS".
 *
 * BENCH FAST skips the slower alternative implementations.
 * Every result line is "name  OK|FAIL  time". Results also go to RESULT.TXT,
 * which is closed after every line (DOS keeps unclosed files in its buffers).
 */
#include <stdio.h>
#include <string.h>
#include "bearssl.h"
#include "vectors.h"

#if defined(__WATCOMC__) && defined(__386__)
/* 32-bit DOS extender: the first megabyte is mapped 1:1.
 * BIOS timer ticks since midnight (18.2065 per second) */
static unsigned long ticks(void)
{
	return *(volatile unsigned long *)0x46C;
}
#define TPS 18.2065
#elif defined(__WATCOMC__)
#include <i86.h>
/* BIOS timer ticks since midnight (18.2065 per second) */
static unsigned long ticks(void)
{
	return *(volatile unsigned long __far *)MK_FP(0x40, 0x6C);
}
#define TPS 18.2065
#else
#include <time.h>
static unsigned long ticks(void)
{
	struct timespec ts;
	clock_gettime(CLOCK_MONOTONIC, &ts);
	return (unsigned long)(ts.tv_sec * 1000 + ts.tv_nsec / 1000000);
}
#define TPS 1000.0
#endif

/* Implementations: 16-bit code uses the 15-bit (i15/m15) arithmetic, a 32-bit
 * build the 31-bit (i31/m31) one with 32x32->64 multiplies. */
#ifdef __386__
#define BITS "32-bit"
#define X25519_FAST  br_ec_c25519_m31
#define X25519_FASTN "X25519 (c25519_m31)"
#define P256_IMPL    br_ec_p256_m31
#define P256_IMPLN   "(p256_m31)"
#define PRIME_IMPL   br_ec_prime_i31
#define PRIME_IMPLN  "(prime_i31)"
#define ECDSA_VRFY   br_ecdsa_i31_vrfy_asn1
#define RSA_VRFY     br_rsa_i31_pkcs1_vrfy
#define RSA_N        "(i31)"
#define POLY_ALT     br_poly1305_ctmul_run
#define POLY_ALTN    "ChaCha20-Poly1305 (ctmul)"
#else
#ifdef __WATCOMC__
#define BITS "16-bit"
#else
#define BITS "native"
#endif
#define X25519_FAST  br_ec_c25519_m15
#define X25519_FASTN "X25519 (c25519_m15)"
#define P256_IMPL    br_ec_p256_m15
#define P256_IMPLN   "(p256_m15)"
#define PRIME_IMPL   br_ec_prime_i15
#define PRIME_IMPLN  "(prime_i15)"
#define ECDSA_VRFY   br_ecdsa_i15_vrfy_asn1
#define RSA_VRFY     br_rsa_i15_pkcs1_vrfy
#define RSA_N        "(i15)"
#define POLY_ALT     br_poly1305_i15_run
#define POLY_ALTN    "ChaCha20-Poly1305 (i15)"
#endif

static int fails;

/* Append a line to RESULT.TXT and close it at once: DOS keeps written data in
 * its buffers until a file is closed, so this is what makes progress visible. */
static void logline(const char *line)
{
	FILE *f = fopen("RESULT.TXT", "a");
	if (f) {
		fputs(line, f);
		fclose(f);
	}
}

static void starting(const char *what)
{
	char line[100];
	sprintf(line, "  starting %s at tick %lu\n", what, ticks());
	fputs(line, stdout);
	logline(line);
}

static void say(const char *fmt, const char *name, int ok, double secs, const char *extra)
{
	char line[160];
	(void)fmt;
	sprintf(line, "%-34s %s  %9.2f s%s\n", name, ok ? "OK  " : "FAIL", secs, extra ? extra : "");
	fputs(line, stdout);
	logline(line);
	if (!ok) fails++;
}

static int hexval(char c)
{
	return c <= '9' ? c - '0' : (c | 32) - 'a' + 10;
}

static size_t unhex(unsigned char *d, const char *s)
{
	size_t n = 0;
	while (s[0] && s[1]) {
		d[n++] = (unsigned char)(hexval(s[0]) << 4 | hexval(s[1]));
		s += 2;
	}
	return n;
}

static unsigned char buf[1024], buf2[1024];

/* Run fn until at least min_ticks have passed; returns seconds per call. */
#define TIMED(per, min_ticks, call) do { \
	unsigned long t0_ = ticks(), t1_; long n_ = 0; \
	do { call; n_++; t1_ = ticks(); } while (t1_ - t0_ < (unsigned long)(min_ticks)); \
	per = (double)(t1_ - t0_) / TPS / (double)n_; \
} while (0)

/* With an argument (FAST): skip the slower alternative implementations. */
int main(int argc, char **argv)
{
	int fast = argc > 1;
	(void)argv;
	unsigned char k[64], iv[16], exp[128], res[128], tag[16];
	double per;
	char extra[64];
	remove("RESULT.TXT");
	fputs("emsg TLS bench - BearSSL 0.6, " BITS " build\n", stdout);
	logline("emsg TLS bench - BearSSL 0.6, " BITS " build\n");
	{
		/* does the BIOS clock tick? (all timings depend on it) */
		unsigned long t0 = ticks(), n = 0;
		while (ticks() == t0 && n < 2000000UL) n++;
		logline(ticks() != t0 ? "timer ticks: yes\n" : "timer ticks: NO - timings invalid\n");
	}

	starting("SHA-256");
	/* SHA-256 */
	{
		br_sha256_context c;
		br_sha256_init(&c);
		br_sha256_update(&c, "abc", 3);
		br_sha256_out(&c, res);
		unhex(exp, "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad");
		int ok = !memcmp(res, exp, 32);
		memset(buf, 0x5a, sizeof buf);
		TIMED(per, 18, (br_sha256_init(&c), br_sha256_update(&c, buf, 1024), br_sha256_out(&c, res)));
		sprintf(extra, "  per KB  (%.1f KB/s)", 1.0 / per);
		say(0, "SHA-256", ok, per, extra);
	}

	starting("AES-GCM");
	/* AES-128-GCM, NIST test case 3, with both table-based AES and two GHASH */
	{
		static const br_ghash gh[2] = {br_ghash_ctmul, br_ghash_ctmul32};
		static const char *ghn[2] = {"AES-128-GCM (aes_big, ctmul)", "AES-128-GCM (aes_big, ctmul32)"};
		for (int g = 0; g < 2; g++) {
			br_aes_big_ctr_keys aes;
			br_gcm_context gc;
			unsigned char p[64], c[64];
			unhex(k, "feffe9928665731c6d6a8f9467308308");
			unhex(iv, "cafebabefacedbaddecaf888");
			size_t pl = unhex(p, "d9313225f88406e5a55909c5aff5269a86a7a9531534f7da2e4c303d8a318a72"
			                     "1c3c0c95956809532fcf0e2449a6b525b16aedf5aa0de657ba637b391aafd255");
			unhex(c, "42831ec2217774244b7221b784d0d49ce3aa212f2c02a4e035c17e2329aca12e"
			         "21d514b25466931c7d8f6a5aac84aa051ba30b396a0aac973d58e091473f5985");
			unhex(exp, "4d5c2af327cd64a62cf35abd2ba6fab4");
			br_aes_big_ctr_init(&aes, k, 16);
			br_gcm_init(&gc, &aes.vtable, gh[g]);
			br_gcm_reset(&gc, iv, 12);
			br_gcm_flip(&gc);
			memcpy(res, p, pl);
			br_gcm_run(&gc, 1, res, pl);
			br_gcm_get_tag(&gc, tag);
			int ok = !memcmp(res, c, pl) && !memcmp(tag, exp, 16);
			TIMED(per, 18, (br_gcm_reset(&gc, iv, 12), br_gcm_flip(&gc), br_gcm_run(&gc, 1, buf, 1024),
			                br_gcm_get_tag(&gc, tag)));
			sprintf(extra, "  per KB  (%.2f KB/s)", 1.0 / per);
			say(0, ghn[g], ok, per, extra);
		}
	}

	starting("ChaCha20-Poly1305");
	/* ChaCha20-Poly1305, RFC 8439 2.8.2 */
	{
		static const char pt[] = "Ladies and Gentlemen of the class of '99: If I could offer you only "
		                         "one tip for the future, sunscreen would be it.";
		static const br_poly1305_run pr[2] = {br_poly1305_ctmul32_run, POLY_ALT};
		static const char *prn[2] = {"ChaCha20-Poly1305 (ctmul32)", POLY_ALTN};
		unsigned char aad[12];
		for (int r = 0; r < 2; r++) {
			unhex(k, "808182838485868788898a8b8c8d8e8f909192939495969798999a9b9c9d9e9f");
			unhex(iv, "070000004041424344454647");
			unhex(aad, "50515253c0c1c2c3c4c5c6c7");
			unhex(exp, "1ae10b594f09e26a7e902ecbd0600691");
			size_t n = strlen(pt);
			memcpy(buf2, pt, n);
			pr[r](k, iv, buf2, n, aad, 12, tag, br_chacha20_ct_run, 1);
			unsigned char c0[4];
			unhex(c0, "d31a8d34");
			int ok = !memcmp(tag, exp, 16) && !memcmp(buf2, c0, 4);
			TIMED(per, 18, pr[r](k, iv, buf, 1024, aad, 12, tag, br_chacha20_ct_run, 1));
			sprintf(extra, "  per KB  (%.2f KB/s)", 1.0 / per);
			say(0, prn[r], ok, per, extra);
		}
	}

	starting("X25519");
	/* X25519, RFC 7748 5.2 */
	{
		static const br_ec_impl *const im[2] = {&br_ec_c25519_i15, &X25519_FAST};
		static const char *imn[2] = {"X25519 (c25519_i15)", X25519_FASTN};
		for (int i = fast; i < 2; i++) {
			unsigned char sc[32], u[32];
			unhex(sc, "a546e36bf0527c9d3b16154b82465edd62144c0ac1fc5a18506a2244ba449ac4");
			unhex(exp, "c3da55379de9c6908e94ea4df28d084f32eccf03491c71f754b4075577a28552");
			unsigned long t0 = ticks();
			unhex(u, "e6db6867583030db3594c1a424b15f7c726624ec26b3353b10a903a6d0ab1c4c");
			int ok = im[i]->mul(u, 32, sc, 32, BR_EC_curve25519) && !memcmp(u, exp, 32);
			say(0, imn[i], ok, (ticks() - t0) / TPS, "  one key exchange");
		}
	}

	starting("ECDSA");
	/* ECDSA signature checks (certificates, ECDHE_ECDSA) */
	{
		unsigned long t0 = ticks();
		br_ec_public_key pk = {BR_EC_secp256r1, (unsigned char *)P256_Q, sizeof P256_Q};
		int ok = ECDSA_VRFY(&P256_IMPL, H256, 32, &pk, P256_SIG, sizeof P256_SIG);
		say(0, "ECDSA P-256 verify " P256_IMPLN, ok, (ticks() - t0) / TPS, 0);
		if (!fast) {
			t0 = ticks();
			ok = br_ecdsa_i15_vrfy_asn1(&br_ec_prime_i15, H256, 32, &pk, P256_SIG, sizeof P256_SIG);
			say(0, "ECDSA P-256 verify (prime_i15)", ok, (ticks() - t0) / TPS, 0);
		}
		t0 = ticks();
		br_ec_public_key pk3 = {BR_EC_secp384r1, (unsigned char *)P384_Q, sizeof P384_Q};
		ok = ECDSA_VRFY(&PRIME_IMPL, H384, 48, &pk3, P384_SIG, sizeof P384_SIG);
		say(0, "ECDSA P-384 verify " PRIME_IMPLN, ok, (ticks() - t0) / TPS, 0);
		/* a wrong hash must fail */
		unsigned char bad[32];
		memcpy(bad, H256, 32);
		bad[0] ^= 1;
		ok = !ECDSA_VRFY(&P256_IMPL, bad, 32, &pk, P256_SIG, sizeof P256_SIG);
		say(0, "ECDSA P-256 rejects a wrong hash", ok, 0, 0);
	}

	starting("ECDH P-256");
	/* ECDHE on P-256 (the other usual key exchange) */
	{
		unsigned char pt[65], sc[32];
		memcpy(pt, P256_Q, 65);
		memset(sc, 0x37, 32);
		unsigned long t0 = ticks();
		int ok = P256_IMPL.mul(pt, 65, sc, 32, BR_EC_secp256r1);
		say(0, "ECDH P-256 " P256_IMPLN, ok, (ticks() - t0) / TPS, "  one key exchange");
	}

	starting("RSA");
	/* RSA signature checks (most certificates) */
	{
		static const unsigned char e[3] = {1, 0, 1};
		br_rsa_public_key pk = {(unsigned char *)RSA2048_N, sizeof RSA2048_N, (unsigned char *)e, 3};
		unsigned char hout[64];
		unsigned long t0 = ticks();
		int ok = RSA_VRFY(RSA2048_SIG, sizeof RSA2048_SIG, BR_HASH_OID_SHA256, 32, &pk, hout)
		         && !memcmp(hout, H256, 32);
		say(0, "RSA-2048 verify " RSA_N, ok, (ticks() - t0) / TPS, 0);
		br_rsa_public_key pk4 = {(unsigned char *)RSA4096_N, sizeof RSA4096_N, (unsigned char *)e, 3};
		t0 = ticks();
		ok = RSA_VRFY(RSA4096_SIG, sizeof RSA4096_SIG, BR_HASH_OID_SHA256, 32, &pk4, hout)
		     && !memcmp(hout, H256, 32);
		say(0, "RSA-4096 verify " RSA_N, ok, (ticks() - t0) / TPS, 0);
	}

	printf("%s\n", fails ? "SOME CHECKS FAILED" : "all checks passed");
	logline(fails ? "RESULT: SOME CHECKS FAILED\n" : "RESULT: all checks passed\n");
	return fails != 0;
}
