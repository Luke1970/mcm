/* dbox.c - reading Dovecot's sdbox files and their flags (see dbox.h)
 *
 * The layouts follow Dovecot 2.x (src/lib-storage/index/dbox-common/
 * dbox-file.h, src/lib-index/mail-index.h, mail-transaction-log.h). Every
 * number in the index and the log is little-endian; a transaction's size is
 * stored in Dovecot's own big-endian "offset" form.
 */
#include "dbox.h"
#include "util.h"

#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

#define MAX_INDEX (256u * 1024 * 1024)   /* larger index or log files are not read */

static uint32_t le32(const unsigned char *p)
{
	return (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24;
}

static uint16_t le16(const unsigned char *p) { return (uint16_t)(p[0] | p[1] << 8); }

/* mail_index_offset_to_uint32() */
static uint32_t log_size(const unsigned char *p)
{
	uint32_t v = (uint32_t)p[0] << 24 | (uint32_t)p[1] << 16 | (uint32_t)p[2] << 8 | p[3];
	if ((v & 0x80808080u) != 0x80808080u) return 0;
	return ((v & 0x7f) | (v & 0x7f00) >> 8 << 7 | (v & 0x7f0000) >> 16 << 14 |
	        (v & 0x7f000000) >> 24 << 21) << 2;
}

static unsigned char *read_file(const char *path, size_t *len)
{
	FILE *f = fopen(path, "rb");
	if (!f) return NULL;
	buf_t b = {0};
	char tmp[65536];
	size_t got;
	while ((got = fread(tmp, 1, sizeof tmp, f)) > 0 && b.len < MAX_INDEX) buf_add(&b, tmp, got);
	fclose(f);
	if (!b.len || b.len >= MAX_INDEX) {
		buf_free(&b);
		return NULL;
	}
	*len = b.len;
	return (unsigned char *)b.d;
}

/* The index files are in dbox-Mails (the default), or next to it. */
static char *index_file(const char *dir, const char *name)
{
	buf_t p = {0};
	struct stat st;
	buf_printf(&p, "%s/" DBOX_MAILS "/%s", dir, name);
	if (stat(p.d, &st) == 0) return buf_steal(&p);
	p.len = 0;
	buf_printf(&p, "%s/%s", dir, name);
	if (stat(p.d, &st) == 0) return buf_steal(&p);
	buf_free(&p);
	return NULL;
}

char *dbox_log_path(const char *dir)
{
	char *p = index_file(dir, "dovecot.index.log");
	if (!p) {
		buf_t b = {0};
		buf_printf(&b, "%s/" DBOX_MAILS "/dovecot.index.log", dir);
		p = buf_steal(&b);
	}
	return p;
}

typedef struct {
	dbox_fl *f;
	int      n, cap;
} fl_list;

static int find(const fl_list *l, uint32_t uid)   /* the first entry >= uid */
{
	int lo = 0, hi = l->n;
	while (lo < hi) {
		int mid = (lo + hi) / 2;
		if (l->f[mid].uid < uid) lo = mid + 1;
		else hi = mid;
	}
	return lo;
}

static void put(fl_list *l, uint32_t uid, uint8_t flags)
{
	int i = find(l, uid);
	if (i < l->n && l->f[i].uid == uid) {
		l->f[i].flags = flags;
		return;
	}
	if (l->n == l->cap) {
		l->cap = l->cap ? l->cap * 2 : 256;
		l->f = xrealloc(l->f, sizeof *l->f * (size_t)l->cap);
	}
	memmove(l->f + i + 1, l->f + i, sizeof *l->f * (size_t)(l->n - i));
	l->f[i] = (dbox_fl){uid, flags};
	l->n++;
}

typedef struct {
	uint32_t indexid, seq, head;   /* the log position the index is synced to */
} idx_pos;

/* dovecot.index: the base header, then record_size bytes per message
 * (uid, flags, extensions). 0, or -1 when it cannot be used. */
static int read_index(const char *dir, fl_list *l, idx_pos *pos)
{
	char *path = index_file(dir, "dovecot.index");
	size_t len = 0;
	unsigned char *d = path ? read_file(path, &len) : NULL;
	free(path);
	if (!d) return -1;
	int rc = -1;
	if (len >= 72 && d[0] == 7 && (d[12] & 1)) {      /* major version 7, little-endian */
		uint32_t hsize = le32(d + 4), rsize = le32(d + 8), count = le32(d + 32);
		if (le16(d + 2) >= 72 && hsize >= 72 && rsize >= 5 && hsize <= len &&
		    count <= (len - hsize) / rsize) {
			for (uint32_t i = 0; i < count; i++) {
				const unsigned char *r = d + hsize + (size_t)i * rsize;
				put(l, le32(r), r[4] & 0x1f);
			}
			pos->indexid = le32(d + 16);
			pos->seq = le32(d + 60);
			pos->head = le32(d + 68);
			rc = 0;
		}
	}
	free(d);
	return rc;
}

/* Apply a transaction log from offset start (0: from its first
 * transaction): appends give a message's flags, flag updates change them.
 * Expunged messages are left: their files are gone. Fills *seq with the
 * file's sequence number; -1 when it is not a log of this index. */
static int read_log(const char *path, uint32_t indexid, uint32_t start, fl_list *l, uint32_t *seq)
{
	size_t len = 0;
	unsigned char *d = read_file(path, &len);
	if (!d) return -1;
	if (len < 24 || d[0] != 1 || (indexid && le32(d + 4) != indexid)) {
		free(d);
		return -1;
	}
	uint32_t hsize = le16(d + 2);
	*seq = le32(d + 8);
	size_t at = start ? start : hsize;
	while (at >= hsize && at + 8 <= len) {
		uint32_t size = log_size(d + at), type = le32(d + at + 4) & 0x0fffffff;
		if (size < 8 || size > len - at) break;           /* unfinished: the end */
		const unsigned char *r = d + at + 8, *end = d + at + size;
		if (type == 0x02) {                               /* APPEND: uid, flags (8 bytes each) */
			for (; r + 8 <= end; r += 8) put(l, le32(r), r[4] & 0x1f);
		} else if (type == 0x04) {                        /* FLAG_UPDATE: uid1 uid2 add remove (12) */
			for (; r + 12 <= end; r += 12) {
				uint32_t u1 = le32(r), u2 = le32(r + 4);
				for (int i = find(l, u1); i < l->n && l->f[i].uid <= u2; i++)
					l->f[i].flags = (uint8_t)((l->f[i].flags & ~r[9]) | (r[8] & 0x1f));
			}
		}
		at += size;
	}
	free(d);
	return 0;
}

int dbox_flags(const char *dir, dbox_fl **out, int *n)
{
	fl_list l = {0};
	idx_pos pos = {0};
	int have_index = read_index(dir, &l, &pos) == 0;
	char *log = index_file(dir, "dovecot.index.log");
	buf_t log2 = {0};
	if (log) buf_printf(&log2, "%s.2", log);
	uint32_t seq = 0, seq2 = 0;
	int ok = have_index;
	if (log) {
		/* which log holds the changes after the index: .log, or .log.2
		 * (rotated) followed by all of .log */
		fl_list probe = {0};
		int r = read_log(log, pos.indexid, 0xffffffffu, &probe, &seq);   /* (only the header) */
		free(probe.f);
		if (r == 0 && have_index && seq == pos.seq) {
			read_log(log, pos.indexid, pos.head, &l, &seq);
		} else if (r == 0 && have_index) {
			fl_list tmp = {0};
			uint32_t s2 = 0;
			if (read_log(log2.d, pos.indexid, 0xffffffffu, &tmp, &s2) == 0 && s2 == pos.seq) {
				read_log(log2.d, pos.indexid, pos.head, &l, &seq2);
				read_log(log, pos.indexid, 0, &l, &seq);
			}
			free(tmp.f);
		} else if (r == 0) {                              /* no index: the whole history */
			read_log(log2.d, 0, 0, &l, &seq2);
			read_log(log, 0, 0, &l, &seq);
			ok = 1;
		}
	}
	free(log);
	buf_free(&log2);
	*out = l.f;
	*n = l.n;
	return ok ? 0 : -1;
}

int dbox_flag_of(const dbox_fl *f, int n, uint32_t uid)
{
	fl_list l = {(dbox_fl *)f, n, n};
	int i = find(&l, uid);
	return i < n && f[i].uid == uid ? f[i].flags : -1;
}

static unsigned long long hex(const char *s, size_t n, int *bad)
{
	unsigned long long v = 0;
	for (size_t i = 0; i < n; i++) {
		char c = s[i];
		int d = c >= '0' && c <= '9' ? c - '0' : c >= 'a' && c <= 'f' ? c - 'a' + 10
		      : c >= 'A' && c <= 'F' ? c - 'A' + 10 : -1;
		if (d < 0) {
			*bad = 1;
			return 0;
		}
		v = v << 4 | (unsigned)d;
	}
	return v;
}

int dbox_locate(FILE *f, long long *off, size_t *len)
{
	char line[256];
	if (fseek(f, 0, SEEK_SET) != 0 || !fgets(line, sizeof line, f)) return -1;
	size_t hlen = strlen(line);
	if (hlen < 3 || line[hlen - 1] != '\n' || (line[0] != '2' && line[0] != '1') || line[1] != ' ')
		return -1;
	/* "2 M1e C<stamp>": M is the size of each message header */
	unsigned long long msize = 0;
	int bad = 0;
	for (char *t = strtok(line + 2, " \n"); t; t = strtok(NULL, " \n"))
		if (*t == 'M') msize = hex(t + 1, strlen(t + 1), &bad);
	if (bad || msize < 30 || msize > 4096) return -1;
	char mh[4096];
	if (fread(mh, 1, (size_t)msize, f) != msize) return -1;
	/* "\001\002" 'N' ' ' <8 old uid> ' ' <16 hex: size> ... '\n' */
	if (mh[0] != '\001' || mh[1] != '\002' || mh[2] != 'N' || mh[msize - 1] != '\n') return -1;
	unsigned long long size = hex(mh + 13, 16, &bad);
	if (bad) return -1;
	*off = (long long)(hlen + msize);
	*len = (size_t)size;
	return 0;
}

const char *dbox_compressed(const char *b, size_t n)
{
	static const struct { const char *magic; size_t n; const char *name; } k[] = {
		{"\x1f\x8b", 2, "gzip"}, {"\x28\xb5\x2f\xfd", 4, "zstd"}, {"BZh", 3, "bzip2"},
		{"\xfd" "7zXZ", 5, "xz"}, {"Dovecot-LZ4", 11, "lz4"},
	};
	for (size_t i = 0; i < sizeof k / sizeof *k; i++)
		if (n >= k[i].n && !memcmp(b, k[i].magic, k[i].n)) return k[i].name;
	return NULL;
}
