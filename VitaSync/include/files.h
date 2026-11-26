#ifndef FILES_H
#define FILES_H

#include <psp2kern/udcd.h>

/*
 * File transfer over USB (build 119): the PC lists, reads and writes files
 * on the Vita through the plugin's vendor interface (bulk EP5 IN, EP6 OUT).
 * The Vita's own filesystem driver does all the IO, in one kernel thread.
 * linux/vitactl.py speaks the same protocol (keep both in step).
 *
 * One command at a time, PC-driven, little-endian:
 *
 *   PC -> Vita   request, always FILES_REQ_SIZE bytes: struct files_req,
 *                then the path (UTF-8, NUL-terminated; RENAME: "old\0new\0")
 *   Vita -> PC   response, always FILES_RESP_SIZE bytes: struct files_resp
 *
 *   STAT    response: status, mode, size, mtime
 *   LIST    response: status, size = bytes of entries that follow, then the
 *           entries (struct files_dirent + name, no NUL) in chunks
 *   GET     arg = offset; response: status, size = bytes that follow, then
 *           the data in chunks of up to FILES_CHUNK
 *   PUT     arg = file size, flags may hold FILES_OVERWRITE. A first
 *           response says go (0) or why not; if go, the PC sends the data
 *           in chunks of up to FILES_CHUNK, then a second response gives
 *           the result. The data goes to "<path>.vstmp", renamed over the
 *           path at the end, so a half-sent file never replaces a good one.
 *   MKDIR, REMOVE (one file), RMDIR (an empty folder), RENAME (flags may
 *           hold FILES_OVERWRITE): response with the status.
 *   PING    response: status 0, size = FILES_PROTOCOL.
 *
 * Without FILES_OVERWRITE, PUT and RENAME refuse an existing target
 * (FILES_E_EXIST) before writing anything. There is no recursive delete.
 * Only paths on FILES_ALLOWED devices are served (FILES_E_REFUSED).
 * A new request arriving while the Vita sends GET or LIST data stops that
 * transfer, so a PC that gave up mid-transfer can always start over. A PC
 * that gave up mid-PUT: the plugin drops the PUT after 5 s without data;
 * a request sent meanwhile is lost and the PC sends it again.
 */

#define FILES_PROTOCOL		1
#define FILES_REQ_MAGIC		0x31465356	/* "VSF1" */
#define FILES_RESP_MAGIC	0x31525356	/* "VSR1" */
#define FILES_REQ_SIZE		1024
#define FILES_RESP_SIZE		64
#define FILES_CHUNK		(256 * 1024)
#define FILES_PATH_MAX		(FILES_REQ_SIZE - 32 - 8)	/* room for ".vstmp" */

enum files_op {
	FILES_OP_PING	= 0,
	FILES_OP_STAT	= 1,
	FILES_OP_LIST	= 2,
	FILES_OP_GET	= 3,
	FILES_OP_PUT	= 4,
	FILES_OP_MKDIR	= 5,
	FILES_OP_REMOVE	= 6,
	FILES_OP_RMDIR	= 7,
	FILES_OP_RENAME	= 8,
};

#define FILES_OVERWRITE		0x1

/* Statuses that aren't the system's own error codes. */
#define FILES_E_BADREQ		0x80010016	/* EINVAL: malformed request */
#define FILES_E_REFUSED		0x8001000D	/* EACCES: device not allowed */
#define FILES_E_EXIST		0x80010011	/* EEXIST */
#define FILES_E_ISDIR		0x80010015	/* EISDIR */
#define FILES_E_TIMEOUT		0x8001003C	/* ETIMEDOUT: the PC stopped */
#define FILES_E_SHORT		0x80010005	/* EIO: fewer bytes than announced */

struct files_req {
	unsigned int magic;
	unsigned int seq;
	unsigned int op;
	unsigned int flags;
	unsigned long long arg;
	unsigned int path_len;		/* bytes of path, NULs included */
	unsigned int reserved;
} __attribute__((packed));

/* Date as on the Vita: year, month, day, hour, minute, second. */
struct files_time {
	unsigned short year;
	unsigned char month, day, hour, minute, second, pad;
} __attribute__((packed));

struct files_resp {
	unsigned int magic;
	unsigned int seq;
	int status;
	unsigned int mode;		/* SCE_S_IFDIR / SCE_S_IFREG bits */
	unsigned long long size;
	struct files_time mtime;
	unsigned char reserved[32];
} __attribute__((packed));

struct files_dirent {
	unsigned short name_len;
	unsigned char is_dir;
	unsigned char pad;
	unsigned int reserved;
	unsigned long long size;
	struct files_time mtime;
} __attribute__((packed));

_Static_assert(sizeof(struct files_req) == 32, "files_req size");
_Static_assert(sizeof(struct files_resp) == FILES_RESP_SIZE, "files_resp size");
_Static_assert(sizeof(struct files_dirent) == 24, "files_dirent size");

/* module_start: memory, event flag, thread. Returns < 0 on failure; the rest
 * of the plugin works without. */
int files_init(SceUdcdEndpoint *ep_in, SceUdcdEndpoint *ep_out);

/* module_stop. */
void files_fini(void);

/* The PC is attached and USB is ours (1), or not (0: detach, or USB about
 * to be taken by VitaShell). Going offline cancels our requests. Safe in
 * USB callbacks and hooks. */
void files_set_online(int online);

#endif
