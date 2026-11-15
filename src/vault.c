/* vault.c - passwords stored encrypted under a master password (see vault.h) */
#include "vault.h"
#include "util.h"

#include <openssl/crypto.h>
#include <openssl/evp.h>
#include <openssl/rand.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define LOG_N    16          /* scrypt N = 65536, r = 8: 64 MiB, about 0.2 s */
#define R        8
#define P        1
#define SALT_LEN 16
#define IV_LEN   12
#define TAG_LEN  16
#define KEY_LEN  32
#define MAX_PW   1024

void vault_free(char *s)
{
	if (!s) return;
	OPENSSL_cleanse(s, strlen(s));
	free(s);
}

#if defined(LIBRESSL_VERSION_NUMBER) || defined(EMSG_OWN_SCRYPT)
/* LibreSSL (OpenBSD) has no EVP_PBE_scrypt: scrypt as in RFC 7914, on top of
 * PBKDF2-HMAC-SHA256. Gives the same keys, so stored passwords move between
 * systems. */
#include <stdint.h>

#define ROTL(a, b) (((a) << (b)) | ((a) >> (32 - (b))))

static void salsa20_8(uint32_t B[16])
{
	uint32_t x[16];
	memcpy(x, B, sizeof x);
	for (int i = 0; i < 8; i += 2) {
		x[4] ^= ROTL(x[0] + x[12], 7);   x[8] ^= ROTL(x[4] + x[0], 9);
		x[12] ^= ROTL(x[8] + x[4], 13);  x[0] ^= ROTL(x[12] + x[8], 18);
		x[9] ^= ROTL(x[5] + x[1], 7);    x[13] ^= ROTL(x[9] + x[5], 9);
		x[1] ^= ROTL(x[13] + x[9], 13);  x[5] ^= ROTL(x[1] + x[13], 18);
		x[14] ^= ROTL(x[10] + x[6], 7);  x[2] ^= ROTL(x[14] + x[10], 9);
		x[6] ^= ROTL(x[2] + x[14], 13);  x[10] ^= ROTL(x[6] + x[2], 18);
		x[3] ^= ROTL(x[15] + x[11], 7);  x[7] ^= ROTL(x[3] + x[15], 9);
		x[11] ^= ROTL(x[7] + x[3], 13);  x[15] ^= ROTL(x[11] + x[7], 18);
		x[1] ^= ROTL(x[0] + x[3], 7);    x[2] ^= ROTL(x[1] + x[0], 9);
		x[3] ^= ROTL(x[2] + x[1], 13);   x[0] ^= ROTL(x[3] + x[2], 18);
		x[6] ^= ROTL(x[5] + x[4], 7);    x[7] ^= ROTL(x[6] + x[5], 9);
		x[4] ^= ROTL(x[7] + x[6], 13);   x[5] ^= ROTL(x[4] + x[7], 18);
		x[11] ^= ROTL(x[10] + x[9], 7);  x[8] ^= ROTL(x[11] + x[10], 9);
		x[9] ^= ROTL(x[8] + x[11], 13);  x[10] ^= ROTL(x[9] + x[8], 18);
		x[12] ^= ROTL(x[15] + x[14], 7); x[13] ^= ROTL(x[12] + x[15], 9);
		x[14] ^= ROTL(x[13] + x[12], 13); x[15] ^= ROTL(x[14] + x[13], 18);
	}
	for (int i = 0; i < 16; i++) B[i] += x[i];
}

/* BlockMix with Salsa20/8: B (2r blocks of 16 words) -> Y */
static void blockmix(const uint32_t *B, uint32_t *Y, int r)
{
	uint32_t X[16];
	memcpy(X, B + (2 * r - 1) * 16, sizeof X);
	for (int i = 0; i < 2 * r; i++) {
		for (int k = 0; k < 16; k++) X[k] ^= B[i * 16 + k];
		salsa20_8(X);
		memcpy(Y + ((i & 1) * r + i / 2) * 16, X, sizeof X);
	}
}

static int own_scrypt(const char *pass, size_t plen, const unsigned char *salt, size_t slen,
                      uint64_t N, int r, int p, unsigned char *key, size_t klen)
{
	size_t words = (size_t)32 * (size_t)r;            /* 128 r bytes */
	size_t blen = words * 4 * (size_t)p;
	unsigned char *B = malloc(blen);
	uint32_t *X = malloc(words * 4), *Y = malloc(words * 4);
	uint32_t *V = malloc(words * 4 * (size_t)N);
	int ok = B && X && Y && V &&
	         PKCS5_PBKDF2_HMAC(pass, (int)plen, salt, (int)slen, 1, EVP_sha256(), (int)blen, B) == 1;
	for (int i = 0; ok && i < p; i++) {
		unsigned char *b = B + (size_t)i * words * 4;
		for (size_t k = 0; k < words; k++)
			X[k] = (uint32_t)b[4 * k] | (uint32_t)b[4 * k + 1] << 8 |
			       (uint32_t)b[4 * k + 2] << 16 | (uint32_t)b[4 * k + 3] << 24;
		for (uint64_t n = 0; n < N; n++) {
			memcpy(V + n * words, X, words * 4);
			blockmix(X, Y, r);
			memcpy(X, Y, words * 4);
		}
		for (uint64_t n = 0; n < N; n++) {
			uint64_t j = X[(2 * r - 1) * 16] & (N - 1);
			for (size_t k = 0; k < words; k++) X[k] ^= V[j * words + k];
			blockmix(X, Y, r);
			memcpy(X, Y, words * 4);
		}
		for (size_t k = 0; k < words; k++) {
			b[4 * k] = (unsigned char)X[k];
			b[4 * k + 1] = (unsigned char)(X[k] >> 8);
			b[4 * k + 2] = (unsigned char)(X[k] >> 16);
			b[4 * k + 3] = (unsigned char)(X[k] >> 24);
		}
	}
	if (ok)
		ok = PKCS5_PBKDF2_HMAC(pass, (int)plen, B, (int)blen, 1, EVP_sha256(), (int)klen, key) == 1;
	if (V) { OPENSSL_cleanse(V, words * 4 * (size_t)N); free(V); }
	if (B) { OPENSSL_cleanse(B, blen); free(B); }
	if (X) { OPENSSL_cleanse(X, words * 4); free(X); }
	if (Y) { OPENSSL_cleanse(Y, words * 4); free(Y); }
	return ok ? 0 : -1;
}
#endif

static int derive(const char *master, const unsigned char *salt, int logn, int r, int p,
                  unsigned char key[KEY_LEN])
{
	if (logn < 10 || logn > 20 || r < 1 || r > 32 || p < 1 || p > 4) return -1;
	uint64_t n = (uint64_t)1 << logn;
	uint64_t maxmem = 128 * n * (uint64_t)r * (uint64_t)p + (1u << 20) * 8;
#if defined(LIBRESSL_VERSION_NUMBER) || defined(EMSG_OWN_SCRYPT)
	(void)maxmem;
	return own_scrypt(master, strlen(master), salt, SALT_LEN, n, r, p, key, KEY_LEN);
#else
	return EVP_PBE_scrypt(master, strlen(master), salt, SALT_LEN, n, (uint64_t)r, (uint64_t)p,
	                      maxmem, key, KEY_LEN) == 1 ? 0 : -1;
#endif
}

static char *b64(const unsigned char *d, size_t n)
{
	char *o = xmalloc(4 * ((n + 2) / 3) + 1);
	EVP_EncodeBlock((unsigned char *)o, d, (int)n);
	return o;
}

/* Decode base64 s[0..sl) into at most max bytes; *got = the length. */
static int unb64(const char *s, size_t sl, unsigned char *out, size_t max, size_t *got)
{
	if (!sl || sl % 4 || sl / 4 * 3 > max + 2) return -1;
	unsigned char tmp[MAX_PW + TAG_LEN + 8];
	if (sl / 4 * 3 > sizeof tmp) return -1;
	int n = EVP_DecodeBlock(tmp, (const unsigned char *)s, (int)sl);
	if (n < 0) return -1;
	size_t pad = (s[sl - 1] == '=') + (s[sl - 2] == '=');
	size_t len = (size_t)n - pad;
	if (len > max) return -1;
	memcpy(out, tmp, len);
	OPENSSL_cleanse(tmp, sizeof tmp);
	*got = len;
	return 0;
}

char *vault_encrypt(const char *master, const char *aad, const char *pw)
{
	size_t pl = strlen(pw);
	if (pl > MAX_PW) return NULL;
	unsigned char salt[SALT_LEN], iv[IV_LEN], key[KEY_LEN], tag[TAG_LEN];
	unsigned char *ct = xmalloc(pl + TAG_LEN);
	char *res = NULL;
	EVP_CIPHER_CTX *c = NULL;
	int len, ok = 0;

	if (RAND_bytes(salt, SALT_LEN) != 1 || RAND_bytes(iv, IV_LEN) != 1) goto out;
	if (derive(master, salt, LOG_N, R, P, key) != 0) goto out;
	if (!(c = EVP_CIPHER_CTX_new())) goto out;
	if (EVP_EncryptInit_ex(c, EVP_aes_256_gcm(), NULL, NULL, NULL) != 1 ||
	    EVP_CIPHER_CTX_ctrl(c, EVP_CTRL_GCM_SET_IVLEN, IV_LEN, NULL) != 1 ||
	    EVP_EncryptInit_ex(c, NULL, NULL, key, iv) != 1 ||
	    EVP_EncryptUpdate(c, NULL, &len, (const unsigned char *)aad, (int)strlen(aad)) != 1 ||
	    EVP_EncryptUpdate(c, ct, &len, (const unsigned char *)pw, (int)pl) != 1 ||
	    EVP_EncryptFinal_ex(c, ct + len, &len) != 1 ||
	    EVP_CIPHER_CTX_ctrl(c, EVP_CTRL_GCM_GET_TAG, TAG_LEN, tag) != 1)
		goto out;
	memcpy(ct + pl, tag, TAG_LEN);
	ok = 1;
out:
	EVP_CIPHER_CTX_free(c);
	OPENSSL_cleanse(key, sizeof key);
	if (ok) {
		char *s = b64(salt, SALT_LEN), *i = b64(iv, IV_LEN), *t = b64(ct, pl + TAG_LEN);
		buf_t b = {0};
		buf_printf(&b, "enc1$%d$%d$%d$%s$%s$%s", LOG_N, R, P, s, i, t);
		res = buf_steal(&b);
		free(s);
		free(i);
		free(t);
	}
	OPENSSL_cleanse(ct, pl + TAG_LEN);
	free(ct);
	return res;
}

char *vault_decrypt(const char *master, const char *aad, const char *v)
{
	if (strncmp(v, "enc1$", 5)) return NULL;
	const char *part[6];
	size_t plen[6];
	const char *p = v + 5;
	for (int i = 0; i < 6; i++) {
		part[i] = p;
		plen[i] = strcspn(p, "$");
		p += plen[i];
		if (i < 5) {
			if (*p != '$') return NULL;
			p++;
		}
	}
	if (*p) return NULL;
	int logn = atoi(part[0]), r = atoi(part[1]), pp = atoi(part[2]);

	unsigned char salt[SALT_LEN], iv[IV_LEN], key[KEY_LEN];
	unsigned char ct[MAX_PW + TAG_LEN];
	size_t n;
	if (unb64(part[3], plen[3], salt, SALT_LEN, &n) || n != SALT_LEN) return NULL;
	if (unb64(part[4], plen[4], iv, IV_LEN, &n) || n != IV_LEN) return NULL;
	size_t cl;
	if (unb64(part[5], plen[5], ct, sizeof ct, &cl) || cl < TAG_LEN) return NULL;
	if (derive(master, salt, logn, r, pp, key) != 0) return NULL;

	size_t pl = cl - TAG_LEN;
	char *pw = xmalloc(pl + 1);
	EVP_CIPHER_CTX *c = EVP_CIPHER_CTX_new();
	int len, ok = c &&
		EVP_DecryptInit_ex(c, EVP_aes_256_gcm(), NULL, NULL, NULL) == 1 &&
		EVP_CIPHER_CTX_ctrl(c, EVP_CTRL_GCM_SET_IVLEN, IV_LEN, NULL) == 1 &&
		EVP_DecryptInit_ex(c, NULL, NULL, key, iv) == 1 &&
		EVP_DecryptUpdate(c, NULL, &len, (const unsigned char *)aad, (int)strlen(aad)) == 1 &&
		EVP_DecryptUpdate(c, (unsigned char *)pw, &len, ct, (int)pl) == 1 &&
		EVP_CIPHER_CTX_ctrl(c, EVP_CTRL_GCM_SET_TAG, TAG_LEN, ct + pl) == 1 &&
		EVP_DecryptFinal_ex(c, (unsigned char *)pw + len, &len) == 1;
	EVP_CIPHER_CTX_free(c);
	OPENSSL_cleanse(key, sizeof key);
	OPENSSL_cleanse(ct, sizeof ct);
	if (!ok || memchr(pw, '\0', pl)) {
		OPENSSL_cleanse(pw, pl + 1);
		free(pw);
		return NULL;
	}
	pw[pl] = '\0';
	return pw;
}
