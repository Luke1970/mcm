/* net.c - TCP connections with TLS (implicit or STARTTLS) and certificate trust */
#include "net.h"

#ifdef _WIN32
#include <winsock2.h>
#include <ws2tcpip.h>
#include <wincrypt.h>
#else
#include <arpa/inet.h>
#include <netdb.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <unistd.h>
#endif
#include <errno.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include <openssl/err.h>
#include <openssl/ssl.h>
#include <openssl/x509v3.h>

#ifdef _WIN32
#define MSG_NOSIGNAL 0
#define close_socket(fd) closesocket((SOCKET)(fd))
#define sock_errno() WSAGetLastError()
#define EINTR_SOCK WSAEINTR

/* Text of a Winsock error code. */
static const char *sock_strerror(int e)
{
	static char s[256];
	if (!FormatMessageA(FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS, NULL, (DWORD)e, 0, s,
	                    sizeof s, NULL))
		snprintf(s, sizeof s, "error %d", e);
	size_t n = strlen(s);
	while (n && (s[n - 1] == '\r' || s[n - 1] == '\n' || s[n - 1] == '.')) s[--n] = '\0';
	return s;
}

/* Read and send timeouts: Windows wants milliseconds in a DWORD. */
static void sock_timeouts(int fd, int seconds)
{
	DWORD ms = (DWORD)seconds * 1000;
	setsockopt((SOCKET)fd, SOL_SOCKET, SO_RCVTIMEO, (const char *)&ms, sizeof ms);
	setsockopt((SOCKET)fd, SOL_SOCKET, SO_SNDTIMEO, (const char *)&ms, sizeof ms);
}
#else
#define close_socket(fd) close(fd)
#define sock_errno() errno
#define EINTR_SOCK EINTR
#define sock_strerror(e) strerror(e)

static void sock_timeouts(int fd, int seconds)
{
	struct timeval tv = {seconds, 0};
	setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof tv);
	setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof tv);
}
#endif

struct net {
	int      fd;
	SSL_CTX *ctx;
	SSL     *ssl;
	int      broken;
	char    *host, *port;
	char     rbuf[16384];
	size_t   rpos, rlen;
	char     certerr[256];   /* verification problems seen in the handshake */
	char     err[512];
};

static void seterr(net_t *n, const char *fmt, ...) __attribute__((format(MCM_PRINTF, 2, 3)));
static void seterr(net_t *n, const char *fmt, ...)
{
	va_list ap;
	va_start(ap, fmt);
	vsnprintf(n->err, sizeof n->err, fmt, ap);
	va_end(ap);
}

const char *net_error(const net_t *n) { return n ? n->err : "not connected"; }
int net_is_tls(const net_t *n) { return n && n->ssl; }

void net_set_timeout(net_t *n, int seconds)
{
	sock_timeouts(n->fd, seconds);
}

void net_local_literal(const net_t *n, char *out, size_t size)
{
	struct sockaddr_storage ss;
	socklen_t sl = sizeof ss;
	char a[INET6_ADDRSTRLEN] = "127.0.0.1";
	if (getsockname(n->fd, (struct sockaddr *)&ss, &sl) == 0) {
		if (ss.ss_family == AF_INET6) {
			struct sockaddr_in6 *s6 = (struct sockaddr_in6 *)&ss;
			if (IN6_IS_ADDR_V4MAPPED(&s6->sin6_addr)) {
				inet_ntop(AF_INET, &s6->sin6_addr.s6_addr[12], a, sizeof a);
			} else {
				inet_ntop(AF_INET6, &s6->sin6_addr, a, sizeof a);
				snprintf(out, size, "[IPv6:%s]", a);
				return;
			}
		} else {
			inet_ntop(AF_INET, &((struct sockaddr_in *)&ss)->sin_addr, a, sizeof a);
		}
	}
	snprintf(out, size, "[%s]", a);
}

net_t *net_open(const char *host, const char *port, int timeout_s, char *err, size_t errlen)
{
	struct addrinfo hints = {0}, *res, *ai;
	hints.ai_socktype = SOCK_STREAM;
	hints.ai_family = AF_UNSPEC;

	if (quit_requested) {
		snprintf(err, errlen, "interrupted (Ctrl-C)");
		return NULL;
	}
	int rc = getaddrinfo(host, port, &hints, &res);
	if (rc) {
		snprintf(err, errlen, "%s: %s", host, gai_strerror(rc));
		return NULL;
	}
	int fd = -1, saved = 0;
	for (ai = res; ai; ai = ai->ai_next) {
		fd = (int)socket(ai->ai_family, ai->ai_socktype, ai->ai_protocol);
		if (fd < 0) { saved = sock_errno(); continue; }
		sock_timeouts(fd, timeout_s);
		if (connect(fd, ai->ai_addr, (socklen_t)ai->ai_addrlen) == 0) break;
		saved = sock_errno();
		if (quit_requested) {
			close_socket(fd);
			freeaddrinfo(res);
			snprintf(err, errlen, "interrupted (Ctrl-C)");
			return NULL;
		}
		close_socket(fd);
		fd = -1;
	}
	freeaddrinfo(res);
	if (fd < 0) {
		snprintf(err, errlen, "cannot connect to %s:%s: %s", host, port, sock_strerror(saved));
		return NULL;
	}
	net_t *n = xcalloc(1, sizeof *n);
	n->fd = fd;
	n->host = xstrdup(host);
	n->port = xstrdup(port);
	return n;
}

void net_close(net_t *n)
{
	if (!n) return;
	if (n->ssl) {
		if (!n->broken) SSL_shutdown(n->ssl);
		SSL_free(n->ssl);
	}
	if (n->ctx) SSL_CTX_free(n->ctx);
	if (n->fd >= 0) close_socket(n->fd);
	free(n->host);
	free(n->port);
	free(n);
}

void net_trust_free(net_trust *t)
{
	free(t->accepted);
	t->accepted = NULL;
}

/* ------------------------------------------------------------------ */
/* Reading and writing                                                 */

static int fill(net_t *n)
{
	if (n->broken) return -1;
	ssize_t r;
	if (n->ssl) {
		int k = SSL_read(n->ssl, n->rbuf, (int)sizeof n->rbuf);
		r = k > 0 ? k : -1;
	} else {
		do r = recv(n->fd, n->rbuf, (int)sizeof n->rbuf, 0);
		while (r < 0 && sock_errno() == EINTR_SOCK && !quit_requested);
		if (r == 0) r = -1;
	}
	if (r <= 0) {
		if (quit_requested) seterr(n, "interrupted (Ctrl-C)");
		else seterr(n, "connection to %s lost", n->host);
		n->broken = 1;
		return -1;
	}
	n->rpos = 0;
	n->rlen = (size_t)r;
	return 0;
}

int net_write(net_t *n, const void *p, size_t len)
{
	const char *s = p;
	if (n->broken) return -1;
	while (len) {
		ssize_t w;
		if (n->ssl) {
			int k = SSL_write(n->ssl, s, (int)(len > 65536 ? 65536 : len));
			w = k > 0 ? k : -1;
		} else {
			int chunk = (int)(len > 65536 ? 65536 : len);
			do w = send(n->fd, s, chunk, MSG_NOSIGNAL);
			while (w < 0 && sock_errno() == EINTR_SOCK && !quit_requested);
		}
		if (w <= 0) {
			seterr(n, "connection to %s lost", n->host);
			n->broken = 1;
			return -1;
		}
		s += w;
		len -= (size_t)w;
	}
	return 0;
}

int net_line(net_t *n, buf_t *b)
{
	for (;;) {
		if (n->rpos >= n->rlen && fill(n) < 0) return -1;
		char *s = n->rbuf + n->rpos;
		size_t avail = n->rlen - n->rpos;
		char *nl = memchr(s, '\n', avail);
		size_t take = nl ? (size_t)(nl - s) + 1 : avail;
		buf_add(b, s, take);
		n->rpos += take;
		if (nl) return 0;
	}
}

int net_bytes(net_t *n, buf_t *b, size_t len)
{
	while (len) {
		if (n->rpos >= n->rlen && fill(n) < 0) return -1;
		size_t avail = n->rlen - n->rpos;
		size_t take = avail < len ? avail : len;
		buf_add(b, n->rbuf + n->rpos, take);
		n->rpos += take;
		len -= take;
	}
	return 0;
}

/* ------------------------------------------------------------------ */
/* TLS and certificates                                                */

#ifdef _WIN32
/* OpenSSL on Windows has no CA file of its own: trust the certificates in
 * the Windows "ROOT" store (Trusted Root Certification Authorities), and
 * cacert.pem next to mcm.exe if there is one. */
static void add_windows_roots(SSL_CTX *ctx)
{
	X509_STORE *st = SSL_CTX_get_cert_store(ctx);
	HCERTSTORE hs = CertOpenSystemStoreW(0, L"ROOT");
	if (hs) {
		PCCERT_CONTEXT c = NULL;
		while ((c = CertEnumCertificatesInStore(hs, c))) {
			const unsigned char *p = c->pbCertEncoded;
			X509 *x = d2i_X509(NULL, &p, (long)c->cbCertEncoded);
			if (x) {
				X509_STORE_add_cert(st, x);
				X509_free(x);
			}
		}
		CertCloseStore(hs, 0);
	}
	char pem[1024];
	snprintf(pem, sizeof pem, "%s/cacert.pem", win_exe_dir());
	FILE *f = fopen(pem, "r");
	if (f) {
		fclose(f);
		SSL_CTX_load_verify_locations(ctx, pem, NULL);
	}
	ERR_clear_error();   /* duplicates are not a problem */
}
#endif

/* SNI + certificate name check. IP literals are matched against IP SANs. */
static int set_peer_name(SSL *ssl, const char *host)
{
	unsigned char addr[16];
	int is_ip = inet_pton(AF_INET, host, addr) == 1 || inet_pton(AF_INET6, host, addr) == 1;
#if OPENSSL_VERSION_MAJOR >= 4
	if (is_ip) return SSL_set1_ipaddr(ssl, host);
	SSL_set_tlsext_host_name(ssl, host);
	return SSL_set1_dnsname(ssl, host);
#else
	if (is_ip) {
		X509_VERIFY_PARAM *vp = SSL_get0_param(ssl);
		return X509_VERIFY_PARAM_set1_ip_asc(vp, host);
	}
	SSL_set_tlsext_host_name(ssl, host);
	return SSL_set1_host(ssl, host);
#endif
}

/* Record every verification problem, but let the handshake finish:
 * check_cert() decides afterwards (possibly by asking the user). Nothing
 * is sent over the connection before that decision. */
static int verify_cb(int ok, X509_STORE_CTX *x)
{
	if (ok) return 1;
	SSL *ssl = X509_STORE_CTX_get_ex_data(x, SSL_get_ex_data_X509_STORE_CTX_idx());
	net_t *n = ssl ? SSL_get_app_data(ssl) : NULL;
	if (n) {
		const char *e = X509_verify_cert_error_string(X509_STORE_CTX_get_error(x));
		size_t k = strlen(n->certerr);
		if (!strstr(n->certerr, e))
			snprintf(n->certerr + k, sizeof n->certerr - k, "%s%s", k ? "; " : "", e);
	}
	return 1;
}

static void name_str(const X509_NAME *nm, char *out, size_t size)
{
	out[0] = '\0';
	BIO *b = BIO_new(BIO_s_mem());
	if (!b) return;
	X509_NAME_print_ex(b, nm, 0, XN_FLAG_RFC2253 & ~ASN1_STRFLGS_ESC_MSB);
	int k = BIO_read(b, out, (int)size - 1);
	out[k > 0 ? k : 0] = '\0';
	BIO_free(b);
}

static void time_str(const ASN1_TIME *t, char *out, size_t size)
{
	struct tm tm;
	if (t && ASN1_TIME_to_tm(t, &tm) == 1) strftime(out, size, "%Y-%m-%d", &tm);
	else snprintf(out, size, "?");
}

static void san_str(X509 *c, char *out, size_t size)
{
	out[0] = '\0';
	GENERAL_NAMES *gens = X509_get_ext_d2i(c, NID_subject_alt_name, NULL, NULL);
	if (!gens) return;
	size_t k = 0;
	for (int i = 0; i < sk_GENERAL_NAME_num(gens) && k < size - 1; i++) {
		const GENERAL_NAME *g = sk_GENERAL_NAME_value(gens, i);
		char v[64] = "";
		if (g->type == GEN_DNS) {
			snprintf(v, sizeof v, "%.*s", ASN1_STRING_length(g->d.dNSName),
			         (const char *)ASN1_STRING_get0_data(g->d.dNSName));
		} else if (g->type == GEN_IPADD) {
			const unsigned char *a = ASN1_STRING_get0_data(g->d.iPAddress);
			int len = ASN1_STRING_length(g->d.iPAddress);
			if (len == 4) inet_ntop(AF_INET, a, v, sizeof v);
			else if (len == 16) inet_ntop(AF_INET6, a, v, sizeof v);
		}
		if (*v) k += (size_t)snprintf(out + k, size - k, "%s%s", k ? ", " : "", v);
	}
	GENERAL_NAMES_free(gens);
	if (k >= size) k = size - 1;
	out[k] = '\0';
}

/* After the handshake: accept a verified certificate, the one accepted
 * earlier, or (when interactive) whatever the callback accepts. */
static int check_cert(net_t *n, net_trust *t, int interactive)
{
	long vr = SSL_get_verify_result(n->ssl);
	if (vr == X509_V_OK && !*n->certerr) return 0;

#if OPENSSL_VERSION_MAJOR >= 3
	X509 *c = SSL_get1_peer_certificate(n->ssl);
#else
	X509 *c = SSL_get_peer_certificate(n->ssl);
#endif
	if (!c) {
		seterr(n, "TLS: %s sent no certificate", n->host);
		return -1;
	}
	if (!*n->certerr)
		snprintf(n->certerr, sizeof n->certerr, "%s", X509_verify_cert_error_string(vr));

	net_certinfo ci = {.host = n->host, .port = n->port, .problem = n->certerr};
	name_str(X509_get_subject_name(c), ci.subject, sizeof ci.subject);
	name_str(X509_get_issuer_name(c), ci.issuer, sizeof ci.issuer);
	san_str(c, ci.names, sizeof ci.names);
	time_str(X509_get0_notBefore(c), ci.not_before, sizeof ci.not_before);
	time_str(X509_get0_notAfter(c), ci.not_after, sizeof ci.not_after);
	unsigned char md[EVP_MAX_MD_SIZE];
	unsigned mdlen = 0;
	X509_digest(c, EVP_sha256(), md, &mdlen);
	X509_free(c);
	for (unsigned i = 0; i < mdlen; i++)
		snprintf(ci.sha256 + 3 * i, sizeof ci.sha256 - 3 * i, "%02X%s", md[i], i + 1 < mdlen ? ":" : "");

	buf_t key = {0};
	buf_printf(&key, "%s %s %s", n->host, n->port, ci.sha256);
	int ok = t && t->accepted && !strcmp(t->accepted, key.d);
	if (!ok && interactive && t && t->fn && t->fn(&ci, t->ctx)) {
		free(t->accepted);
		t->accepted = buf_steal(&key);
		ok = 1;
	}
	buf_free(&key);
	if (!ok) seterr(n, "TLS certificate of %s not trusted: %s", n->host, n->certerr);
	return ok ? 0 : -1;
}

int net_tls(net_t *n, net_trust *t, int interactive)
{
	if (n->broken) return -1;
	if (n->rpos < n->rlen) {
		/* data received before the handshake could have been injected by
		 * an attacker in plaintext; never treat it as protected */
		seterr(n, "%s sent unexpected data before TLS started", n->host);
		n->broken = 1;
		return -1;
	}

	n->ctx = SSL_CTX_new(TLS_client_method());
	if (!n->ctx) { seterr(n, "TLS init failed"); n->broken = 1; return -1; }
	SSL_CTX_set_min_proto_version(n->ctx, TLS1_2_VERSION);
	SSL_CTX_set_default_verify_paths(n->ctx);
#ifdef _WIN32
	add_windows_roots(n->ctx);
#endif
	SSL_CTX_set_verify(n->ctx, SSL_VERIFY_PEER, verify_cb);

	n->ssl = SSL_new(n->ctx);
	if (!n->ssl) { seterr(n, "TLS init failed"); n->broken = 1; return -1; }
	SSL_set_fd(n->ssl, n->fd);
	SSL_set_app_data(n->ssl, n);
	n->certerr[0] = '\0';
	if (set_peer_name(n->ssl, n->host) != 1) {
		seterr(n, "TLS init failed for host %s", n->host);
		n->broken = 1;
		return -1;
	}

	if (SSL_connect(n->ssl) != 1) {
		unsigned long e = ERR_get_error();
		seterr(n, "TLS handshake with %s failed: %s", n->host,
		       e ? ERR_reason_error_string(e) : "connection closed");
		n->broken = 1;
		return -1;
	}
	if (check_cert(n, t, interactive) < 0) {
		n->broken = 1;
		return -1;
	}
	return 0;
}
