/* vault.h - passwords stored encrypted under a master password
 *
 * A stored password is one text value:
 *
 *     enc1$<log2 N>$<r>$<p>$<salt>$<nonce>$<ciphertext+tag>     (base64 parts)
 *
 *   key        = scrypt(master password, salt, N, r, p) -> 32 bytes
 *   ciphertext = AES-256-GCM(key, nonce, password), 16-byte tag appended
 *   AAD        = what the password belongs to, e.g. "imap user@host"
 *
 * Salt (16 bytes) and nonce (12 bytes) are random for every value. The AAD
 * is authenticated, so a value only decrypts for the server it was stored
 * for: editing server= in the config file cannot make mcm send the
 * password elsewhere. A wrong master password or any change to the value
 * makes decryption fail (the GCM tag does not match).
 */
#ifndef MCM_VAULT_H
#define MCM_VAULT_H

/* Encrypt; returns a malloc'd "enc1$..." string, or NULL on failure. */
char *vault_encrypt(const char *master, const char *aad, const char *password);

/* Decrypt; returns the malloc'd password, or NULL (wrong master password,
 * other AAD, damaged value). */
char *vault_decrypt(const char *master, const char *aad, const char *value);

/* Overwrite with zeros and free; NULL is fine. */
void vault_free(char *secret);

#endif
