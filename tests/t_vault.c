/* t_vault.c - vault.c: stored passwords (scrypt + AES-256-GCM) */
#include "check.h"
#include "../src/vault.h"

static void t_roundtrip(void)
{
	char *v = vault_encrypt("master pw", "imap me@host", "s3cret \xC3\xA9");
	CHECK(v != NULL);
	if (!v) return;
	CHECK(!strncmp(v, "enc1$", 5));
	CHECK_NOT_HAS(v, "s3cret");
	char *p = vault_decrypt("master pw", "imap me@host", v);
	CHECK_STR(p, "s3cret \xC3\xA9");
	vault_free(p);

	/* wrong master password, other server (AAD): nothing */
	CHECK_STR(vault_decrypt("Master pw", "imap me@host", v), NULL);
	CHECK_STR(vault_decrypt("master pw", "imap me@evil", v), NULL);

	/* any change to the value: nothing. (The first character of the
	 * ciphertext: the last ones may hold only base64 padding bits.) */
	char *bad = strdup(v);
	size_t n = strlen(bad);
	char *ctext = strrchr(bad, '$') + 1;
	*ctext = *ctext == 'A' ? 'B' : 'A';
	CHECK_STR(vault_decrypt("master pw", "imap me@host", bad), NULL);
	*ctext = v[ctext - bad];
	char *salt = strchr(strchr(strchr(strchr(bad, '$') + 1, '$') + 1, '$') + 1, '$') + 1;
	*salt = *salt == 'A' ? 'B' : 'A';                     /* another salt: another key */
	CHECK_STR(vault_decrypt("master pw", "imap me@host", bad), NULL);
	bad[n - 3] = 0;
	CHECK_STR(vault_decrypt("master pw", "imap me@host", bad), NULL);
	free(bad);

	/* random salt and nonce: the same password twice gives other values */
	char *v2 = vault_encrypt("master pw", "imap me@host", "s3cret \xC3\xA9");
	CHECK(v2 && strcmp(v, v2));
	free(v2);
	free(v);

	/* nonsense values */
	CHECK_STR(vault_decrypt("m", "a", ""), NULL);
	CHECK_STR(vault_decrypt("m", "a", "enc1$"), NULL);
	CHECK_STR(vault_decrypt("m", "a", "enc1$99$8$1$AA$AA$AA"), NULL);
	CHECK_STR(vault_decrypt("m", "a", "plain text"), NULL);
	vault_free(NULL);

	/* an empty password */
	v = vault_encrypt("m", "a", "");
	CHECK_STR_FREE(vault_decrypt("m", "a", v), "");
	free(v);
}

void run_vault(void)
{
	RUN(t_roundtrip);
}
