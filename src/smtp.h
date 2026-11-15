/* smtp.h - message submission over SMTP (RFC 5321bis, RFC 6409)
 *
 * implicit_tls (usually port 465): TLS from the start (RFC 8314). Otherwise
 * STARTTLS (RFC 3207) is required; nothing is sent unencrypted. Uses the ESMTP
 * extensions SMTPUTF8 (RFC 6531), 8BITMIME (RFC 6152), SIZE (RFC 1870)
 * and AUTH PLAIN / LOGIN (RFC 4954, RFC 4616).
 */
#ifndef MCM_SMTP_H
#define MCM_SMTP_H

#include <stddef.h>

#include "net.h"

typedef struct {
	int smtputf8, eightbitmime, starttls, auth_plain, auth_login;
	unsigned long size_max;    /* 0 = no limit announced */
} smtp_caps;

typedef struct smtp smtp_t;

/* Connect, greet, secure the connection and log in (if user is set).
 * Certificate problems go to trust (asked only when interactive). */
smtp_t *smtp_open(const char *host, const char *port, int implicit_tls, const char *user,
                  const char *pass, net_trust *trust, int interactive, smtp_caps *caps,
                  char *err, size_t errlen);

/* One transaction: MAIL FROM, RCPT TO for each recipient, DATA. The data
 * has CRLF line ends; dot-stuffing is done here. All recipients must be
 * accepted, otherwise nothing is sent. */
int  smtp_send(smtp_t *s, const char *from, char *const *rcpt, int nrcpt, const char *data,
               size_t len, int smtputf8, int eightbit, char *err, size_t errlen);

void smtp_close(smtp_t *s);   /* QUIT; NULL is fine */

#endif
