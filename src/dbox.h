/* dbox.h - reading Dovecot's sdbox (single-dbox) mailboxes
 *
 * An sdbox mailbox is a directory with a dbox-Mails/ directory that holds
 * one file per message, u.UID. Each file starts with a header line
 * ("2 M1e C<time>"), then a message header ("\001\002N ... <size in hex>\n"),
 * the message itself and a metadata block. The flags are not in the files:
 * they are in Dovecot's index (dovecot.index) and the changes since in its
 * transaction log (dovecot.index.log, .log.2). mcm only reads these files.
 */
#ifndef MCM_DBOX_H
#define MCM_DBOX_H

#include <stddef.h>
#include <stdint.h>
#include <stdio.h>

#define DBOX_MAILS "dbox-Mails"

/* Dovecot's flag bits */
enum { DBOX_ANSWERED = 0x01, DBOX_FLAGGED = 0x02, DBOX_DELETED = 0x04, DBOX_SEEN = 0x08 };

typedef struct {
	uint32_t uid;
	uint8_t  flags;
} dbox_fl;

/* The flags of the messages of the sdbox mailbox in directory dir, sorted
 * by uid (*out malloc'd, may be NULL). Returns 0, or -1 when there is no
 * index that can be read (the flags are then unknown). */
int dbox_flags(const char *dir, dbox_fl **out, int *n);
/* The flags of uid in a list from dbox_flags, -1 when it is not there. */
int dbox_flag_of(const dbox_fl *f, int n, uint32_t uid);
/* The index's transaction log of dir (malloc'd path; it may not exist):
 * it changes whenever flags change. */
char *dbox_log_path(const char *dir);
/* Where the message is in an open u.UID file: its offset and length.
 * 0, or -1 (not a dbox file). */
int dbox_locate(FILE *f, long long *off, size_t *len);
/* NULL, or the kind of compression the message starts with (Dovecot's
 * mail_compress plugin), which mcm cannot read. */
const char *dbox_compressed(const char *b, size_t n);

#endif
