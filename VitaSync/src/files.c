/*
 * File transfer over USB (build 119). Protocol in include/files.h.
 *
 * One kernel thread does everything: it queues a receive for the next
 * request on EP6, checks it, does the file IO and answers on EP5. USB
 * callbacks only record completions and wake it. Every wait has a timeout,
 * so a PC that goes away mid-transfer never blocks the thread: the transfer
 * is dropped and the thread waits for the next request.
 */
#include <psp2kern/kernel/threadmgr.h>
#include <psp2kern/kernel/sysmem.h>
#include <psp2kern/kernel/cpu.h>
#include <psp2kern/io/fcntl.h>
#include <psp2kern/io/stat.h>
#include <psp2kern/io/dirent.h>
#include <psp2kern/udcd.h>
#include <string.h>
#include "files.h"

#define EV_OUT		1	/* EP6 receive completed */
#define EV_IN		2	/* EP5 send completed */
#define EV_WAKE		4	/* online state changed, or exiting */

/* Per chunk or response. A 256 KiB chunk takes ~25 ms; a PC that gave up
 * mid-push holds the next command back this long (build 119: 10 s). */
#define DATA_TIMEOUT_US		(5 * 1000 * 1000)
#define IDLE_TICK_US		(1000 * 1000)
#define CANCEL_WAIT_US		(100 * 1000)

/* One physically contiguous block: the request, the response, the data. */
#define REQ_OFF		0
#define RESP_OFF	1024
#define DATA_OFF	4096
#define MEM_SIZE	(DATA_OFF + FILES_CHUNK)

static SceUdcdEndpoint *ep_in, *ep_out;
static SceUID mem_uid = -1;
static unsigned char *mem;
static SceUID ev_id = -1;
static SceUID thread_id = -1;

static volatile int online;
static volatile int exiting;
/* Bumped at each online change: an operation started under an older value
 * is abandoned. */
static volatile unsigned int online_gen;

static SceUdcdDeviceRequest out_req, in_req;
static volatile int out_owned, in_owned;	/* queued at SceUdcd */
static volatile int out_rc, in_rc;
static volatile unsigned int out_len;

/* The request being served (copied out of the DMA buffer, which the next
 * receive may overwrite) and the paths built from it. */
static unsigned char req_copy[FILES_REQ_SIZE];
static char tmp_path[FILES_REQ_SIZE];
static unsigned int my_gen;

static const char *const allowed[] = {
	"ux0:", "ur0:", "uma0:", "imc0:", "xmc0:", "grw0:",
};

/* Local string helpers: only memcpy/memset come from the kernel's
 * SceSysclibForDriver, nothing that could pull in a user-mode import. */
static unsigned int str_nlen(const char *s, unsigned int max)
{
	unsigned int n = 0;

	while (n < max && s[n])
		n++;
	return n;
}

static int str_starts(const char *s, const char *prefix)
{
	while (*prefix)
		if (*s++ != *prefix++)
			return 0;
	return 1;
}

/* ---- USB ---- */

static void out_done(SceUdcdDeviceRequest *r)
{
	out_rc = r->returnCode;
	out_len = r->transmitted;
	out_owned = 0;
	ksceKernelSetEventFlag(ev_id, EV_OUT);
}

static void in_done(SceUdcdDeviceRequest *r)
{
	in_rc = r->returnCode;
	in_owned = 0;
	ksceKernelSetEventFlag(ev_id, EV_IN);
}

static void clear_bits(unsigned int bits)
{
	unsigned int seen;

	ksceKernelPollEventFlag(ev_id, bits, SCE_EVENT_WAITOR |
				SCE_EVENT_WAITCLEAR_PAT, &seen);
}

/* Bits seen (and cleared) among those asked, 0 on timeout. */
static unsigned int wait_bits(unsigned int bits, unsigned int timeout_us)
{
	unsigned int seen = 0;
	SceUInt to = timeout_us;

	if (ksceKernelWaitEventFlag(ev_id, bits, SCE_EVENT_WAITOR |
				    SCE_EVENT_WAITCLEAR_PAT, &seen, &to) < 0)
		return 0;
	return seen & bits;
}

static int queue_out(unsigned char *buf, unsigned int size)
{
	int ret;

	ksceKernelDcacheCleanInvalidateRange(buf, size);
	clear_bits(EV_OUT);
	out_req = (SceUdcdDeviceRequest){
		.endpoint = ep_out,
		.data = buf,
		.attributes = SCE_UDCD_DEVICE_REQUEST_ATTR_PHYCONT,
		.size = size,
		.isControlRequest = 0,
		.onComplete = out_done,
		.transmitted = 0,
		.returnCode = 0,
		.next = NULL,
		.unused = NULL,
		.physicalAddress = NULL
	};
	out_owned = 1;
	ret = ksceUdcdReqRecv(&out_req);
	if (ret < 0)
		out_owned = 0;
	return ret;
}

static int queue_in(unsigned char *buf, unsigned int size)
{
	int ret;

	ksceKernelDcacheCleanRange(buf, size);
	clear_bits(EV_IN);
	in_req = (SceUdcdDeviceRequest){
		.endpoint = ep_in,
		.data = buf,
		.attributes = SCE_UDCD_DEVICE_REQUEST_ATTR_PHYCONT,
		.size = size,
		.isControlRequest = 0,
		.onComplete = in_done,
		.transmitted = 0,
		.returnCode = 0,
		.next = NULL,
		.unused = NULL,
		.physicalAddress = NULL
	};
	in_owned = 1;
	ret = ksceUdcdReqSend(&in_req);
	if (ret < 0)
		in_owned = 0;
	return ret;
}

/* Cancel what is queued on an endpoint and give the completion a moment to
 * come back (a cancel may not call it), so the request structure is free
 * before it is used again. */
static void cancel_out(void)
{
	int i;

	if (!out_owned)
		return;
	ksceUdcdReqCancelAll(ep_out);
	ksceUdcdClearFIFO(ep_out);
	for (i = 0; i < CANCEL_WAIT_US / 1000 && out_owned; i++)
		ksceKernelDelayThread(1000);
	out_owned = 0;
	clear_bits(EV_OUT);
}

static void cancel_in(void)
{
	int i;

	if (!in_owned)
		return;
	ksceUdcdReqCancelAll(ep_in);
	ksceUdcdClearFIFO(ep_in);
	for (i = 0; i < CANCEL_WAIT_US / 1000 && in_owned; i++)
		ksceKernelDelayThread(1000);
	in_owned = 0;
	clear_bits(EV_IN);
}

static int still_ours(void)
{
	return online && !exiting && online_gen == my_gen;
}

/* Send len bytes from buf and wait for them to go. 0, or < 0. */
static int send_wait(unsigned char *buf, unsigned int len)
{
	unsigned int seen;

	if (!still_ours() || queue_in(buf, len) < 0)
		return FILES_E_TIMEOUT;
	seen = wait_bits(EV_IN | EV_WAKE, DATA_TIMEOUT_US);
	if (!(seen & EV_IN) || in_rc < 0) {
		cancel_in();
		return FILES_E_TIMEOUT;
	}
	return 0;
}

static void time_from(struct files_time *t, const SceDateTime *d)
{
	t->year = d->year;
	t->month = d->month;
	t->day = d->day;
	t->hour = d->hour;
	t->minute = d->minute;
	t->second = d->second;
	t->pad = 0;
}

static int respond(int status, unsigned int mode, unsigned long long size,
		   const SceDateTime *mtime)
{
	const struct files_req *q = (const struct files_req *)req_copy;
	struct files_resp *r = (struct files_resp *)(mem + RESP_OFF);

	memset(r, 0, sizeof(*r));
	r->magic = FILES_RESP_MAGIC;
	r->seq = q->seq;
	r->status = status;
	r->mode = mode;
	r->size = size;
	if (mtime)
		time_from(&r->mtime, mtime);
	return send_wait(mem + RESP_OFF, FILES_RESP_SIZE);
}

/*
 * Send len bytes of data from the data buffer, while a receive stays queued
 * for the next request: if one arrives first, this transfer is dropped and
 * 1 is returned (the new request is in the request buffer). 0 when sent,
 * < 0 when the PC stopped reading.
 */
static int send_data(unsigned int len)
{
	unsigned int seen;

	if (!out_owned && queue_out(mem + REQ_OFF, FILES_REQ_SIZE) < 0)
		return FILES_E_TIMEOUT;
	if (!still_ours() || queue_in(mem + DATA_OFF, len) < 0)
		return FILES_E_TIMEOUT;
	seen = wait_bits(EV_IN | EV_OUT | EV_WAKE, DATA_TIMEOUT_US);
	if (seen & EV_OUT) {
		cancel_in();
		return 1;
	}
	if (!(seen & EV_IN) || in_rc < 0) {
		cancel_in();
		return FILES_E_TIMEOUT;
	}
	return 0;
}

/* ---- Requests ---- */

static int path_allowed(const char *path)
{
	unsigned int i;

	for (i = 0; i < sizeof(allowed) / sizeof(allowed[0]); i++)
		if (str_starts(path, allowed[i]))
			return 1;
	return 0;
}

static int do_stat(const char *path)
{
	SceIoStat st;
	int ret;

	memset(&st, 0, sizeof(st));
	ret = ksceIoGetstat(path, &st);
	if (ret < 0)
		return respond(ret, 0, 0, NULL);
	return respond(0, st.st_mode, (unsigned long long)st.st_size, &st.st_mtime);
}

static int do_list(const char *path)
{
	static SceIoDirent d;
	unsigned int used = 0, truncated = 0;
	SceUID dfd;
	int ret;

	dfd = ksceIoDopen(path);
	if (dfd < 0)
		return respond(dfd, 0, 0, NULL);
	for (;;) {
		struct files_dirent *e;
		unsigned int name_len;

		memset(&d, 0, sizeof(d));
		ret = ksceIoDread(dfd, &d);
		if (ret <= 0)
			break;
		name_len = str_nlen(d.d_name, sizeof(d.d_name));
		if (used + sizeof(*e) + name_len > FILES_CHUNK) {
			truncated = 1;
			break;
		}
		e = (struct files_dirent *)(mem + DATA_OFF + used);
		memset(e, 0, sizeof(*e));
		e->name_len = (unsigned short)name_len;
		e->is_dir = SCE_S_ISDIR(d.d_stat.st_mode) ? 1 : 0;
		e->size = (unsigned long long)d.d_stat.st_size;
		time_from(&e->mtime, &d.d_stat.st_mtime);
		memcpy(mem + DATA_OFF + used + sizeof(*e), d.d_name, name_len);
		used += sizeof(*e) + name_len;
	}
	ksceIoDclose(dfd);
	if (ret < 0)
		return respond(ret, 0, 0, NULL);
	/* mode bit 31: the listing didn't fit and was cut. */
	ret = respond(0, truncated ? 0x80000000u : 0, used, NULL);
	if (ret < 0 || used == 0)
		return ret;
	return send_data(used);
}

static int do_get(const char *path, unsigned long long offset)
{
	SceIoStat st;
	unsigned long long left;
	SceUID fd;
	int ret;

	memset(&st, 0, sizeof(st));
	ret = ksceIoGetstat(path, &st);
	if (ret < 0)
		return respond(ret, 0, 0, NULL);
	if (SCE_S_ISDIR(st.st_mode))
		return respond(FILES_E_ISDIR, st.st_mode, 0, NULL);
	if (offset > (unsigned long long)st.st_size)
		return respond(FILES_E_BADREQ, 0, 0, NULL);
	fd = ksceIoOpen(path, SCE_O_RDONLY, 0);
	if (fd < 0)
		return respond(fd, 0, 0, NULL);
	if (offset && ksceIoLseek(fd, (SceOff)offset, SCE_SEEK_SET) < 0) {
		ksceIoClose(fd);
		return respond(FILES_E_BADREQ, 0, 0, NULL);
	}
	left = (unsigned long long)st.st_size - offset;
	ret = respond(0, st.st_mode, left, &st.st_mtime);
	while (ret == 0 && left > 0) {
		unsigned int n = left > FILES_CHUNK ? FILES_CHUNK : (unsigned int)left;
		int got = ksceIoRead(fd, mem + DATA_OFF, n);

		/* The file shrank under us: stop; the PC times out. */
		if (got != (int)n) {
			ret = FILES_E_SHORT;
			break;
		}
		ret = send_data(n);
		left -= n;
	}
	ksceIoClose(fd);
	return ret;
}

/* Replace target by tmp: rename, and if the target exists (overwrite was
 * allowed), remove it first. */
static int put_in_place(const char *tmp, const char *target)
{
	SceIoStat st;
	int ret;

	memset(&st, 0, sizeof(st));
	if (ksceIoGetstat(target, &st) >= 0) {
		ret = ksceIoRemove(target);
		if (ret < 0)
			return ret;
	}
	return ksceIoRename(tmp, target);
}

static int do_put(const char *path, unsigned long long size, unsigned int flags)
{
	SceIoStat st;
	unsigned long long left = size;
	int status = 0, ret;
	SceUID fd;

	memset(&st, 0, sizeof(st));
	if (ksceIoGetstat(path, &st) >= 0) {
		if (SCE_S_ISDIR(st.st_mode))
			return respond(FILES_E_ISDIR, st.st_mode, 0, NULL);
		if (!(flags & FILES_OVERWRITE))
			return respond(FILES_E_EXIST, st.st_mode,
				       (unsigned long long)st.st_size, &st.st_mtime);
	}
	{
		unsigned int n = str_nlen(path, FILES_PATH_MAX);

		memcpy(tmp_path, path, n);
		memcpy(tmp_path + n, ".vstmp", sizeof(".vstmp"));
	}
	fd = ksceIoOpen(tmp_path, SCE_O_WRONLY | SCE_O_CREAT | SCE_O_TRUNC, 6);
	if (fd < 0)
		return respond(fd, 0, 0, NULL);

	ret = respond(0, 0, size, NULL);	/* go */
	if (ret < 0) {
		ksceIoClose(fd);
		ksceIoRemove(tmp_path);
		return ret;
	}

	while (left > 0) {
		unsigned int n = left > FILES_CHUNK ? FILES_CHUNK : (unsigned int)left;
		unsigned int seen;

		if (!still_ours() || queue_out(mem + DATA_OFF, n) < 0) {
			status = FILES_E_TIMEOUT;
			break;
		}
		seen = wait_bits(EV_OUT | EV_WAKE, DATA_TIMEOUT_US);
		if (!(seen & EV_OUT)) {
			cancel_out();
			status = FILES_E_TIMEOUT;
			break;
		}
		if (out_rc < 0 || out_len != n) {
			status = FILES_E_SHORT;
			break;
		}
		ksceKernelDcacheInvalidateRange(mem + DATA_OFF, n);
		/* After a write error, keep reading what the PC sends (it
		 * doesn't know yet), then report the error. */
		if (status == 0) {
			ret = ksceIoWrite(fd, mem + DATA_OFF, n);
			if (ret != (int)n)
				status = ret < 0 ? ret : FILES_E_SHORT;
		}
		left -= n;
	}
	ksceIoClose(fd);

	if (status == 0)
		status = put_in_place(tmp_path, path);
	if (status < 0)
		ksceIoRemove(tmp_path);
	if (status == FILES_E_TIMEOUT && !still_ours())
		return status;
	return respond(status, 0, size, NULL);
}

static int do_rename(const char *from, const char *to, unsigned int flags)
{
	SceIoStat st;
	int ret;

	memset(&st, 0, sizeof(st));
	if (ksceIoGetstat(to, &st) >= 0) {
		if (SCE_S_ISDIR(st.st_mode))
			return respond(FILES_E_ISDIR, st.st_mode, 0, NULL);
		if (!(flags & FILES_OVERWRITE))
			return respond(FILES_E_EXIST, st.st_mode,
				       (unsigned long long)st.st_size, &st.st_mtime);
		ret = ksceIoRemove(to);
		if (ret < 0)
			return respond(ret, 0, 0, NULL);
	}
	return respond(ksceIoRename(from, to), 0, 0, NULL);
}

static int do_remove(const char *path)
{
	SceIoStat st;

	memset(&st, 0, sizeof(st));
	if (ksceIoGetstat(path, &st) >= 0 && SCE_S_ISDIR(st.st_mode))
		return respond(FILES_E_ISDIR, st.st_mode, 0, NULL);
	return respond(ksceIoRemove(path), 0, 0, NULL);
}

/* Serve the request in req_copy. Returns 1 when a new request arrived
 * meanwhile (it is in the request buffer), else 0. */
static int serve(void)
{
	const struct files_req *q = (const struct files_req *)req_copy;
	const char *path = (const char *)(req_copy + sizeof(*q));
	const char *path2 = NULL;
	unsigned int max = FILES_REQ_SIZE - sizeof(*q);
	unsigned int len;
	int ret;

	if (q->magic != FILES_REQ_MAGIC)
		return 0;	/* not a request: dropped */
	if (q->op == FILES_OP_PING)
		return respond(0, 0, FILES_PROTOCOL, NULL) == 1;

	len = str_nlen(path, max);
	if (q->path_len == 0 || q->path_len > max || len == max ||
	    len + 1 > FILES_PATH_MAX || !path_allowed(path))
		return respond(len == max || q->path_len > max ?
			       FILES_E_BADREQ : FILES_E_REFUSED, 0, 0, NULL) == 1;
	if (q->op == FILES_OP_RENAME) {
		unsigned int len2;

		path2 = path + len + 1;
		if (len + 1 >= max)
			return respond(FILES_E_BADREQ, 0, 0, NULL) == 1;
		len2 = str_nlen(path2, max - len - 1);
		if (len2 == max - len - 1 || len2 + 1 > FILES_PATH_MAX)
			return respond(FILES_E_BADREQ, 0, 0, NULL) == 1;
		if (!path_allowed(path2))
			return respond(FILES_E_REFUSED, 0, 0, NULL) == 1;
	}

	switch (q->op) {
	case FILES_OP_STAT:	ret = do_stat(path); break;
	case FILES_OP_LIST:	ret = do_list(path); break;
	case FILES_OP_GET:	ret = do_get(path, q->arg); break;
	case FILES_OP_PUT:	ret = do_put(path, q->arg, q->flags); break;
	case FILES_OP_MKDIR:	ret = respond(ksceIoMkdir(path, 0777), 0, 0, NULL); break;
	case FILES_OP_REMOVE:	ret = do_remove(path); break;
	case FILES_OP_RMDIR:	ret = respond(ksceIoRmdir(path), 0, 0, NULL); break;
	case FILES_OP_RENAME:	ret = do_rename(path, path2, q->flags); break;
	default:		ret = respond(FILES_E_BADREQ, 0, 0, NULL); break;
	}
	return ret == 1;
}

static int files_thread(SceSize args, void *argp)
{
	int have_req = 0;

	(void)args;
	(void)argp;
	my_gen = online_gen - 1;
	while (!exiting) {
		if (!online) {
			wait_bits(EV_WAKE, IDLE_TICK_US);
			continue;
		}
		if (my_gen != online_gen) {
			/* (Re)connected: nothing of ours is queued any more. */
			my_gen = online_gen;
			out_owned = in_owned = 0;
			have_req = 0;
			clear_bits(EV_OUT | EV_IN);
		}
		if (!have_req) {
			unsigned int seen;

			if (!out_owned &&
			    queue_out(mem + REQ_OFF, FILES_REQ_SIZE) < 0) {
				wait_bits(EV_WAKE, 100 * 1000);
				continue;
			}
			seen = wait_bits(EV_OUT | EV_WAKE, IDLE_TICK_US);
			if (!(seen & EV_OUT))
				continue;
		}
		have_req = 0;
		if (out_rc < 0 || out_len != FILES_REQ_SIZE)
			continue;	/* error or not a request: dropped */
		ksceKernelDcacheInvalidateRange(mem + REQ_OFF, FILES_REQ_SIZE);
		memcpy(req_copy, mem + REQ_OFF, FILES_REQ_SIZE);
		have_req = serve();
	}
	return 0;
}

/* ---- Life cycle ---- */

int files_init(SceUdcdEndpoint *in, SceUdcdEndpoint *out)
{
	SceKernelAllocMemBlockKernelOpt opt;
	unsigned int size = (MEM_SIZE + 0xFFF) & ~0xFFF;
	int ret;

	ep_in = in;
	ep_out = out;

	memset(&opt, 0, sizeof(opt));
	opt.size = sizeof(opt);
	opt.attr = SCE_KERNEL_ALLOC_MEMBLOCK_ATTR_PHYCONT |
		   SCE_KERNEL_ALLOC_MEMBLOCK_ATTR_HAS_ALIGNMENT;
	opt.alignment = 4 * 1024;
	mem_uid = ksceKernelAllocMemBlock("vitasync_files", 0x10208006, size, &opt);
	if (mem_uid < 0)
		return mem_uid;
	ret = ksceKernelGetMemBlockBase(mem_uid, (void **)&mem);
	if (ret < 0)
		goto err_mem;

	ev_id = ksceKernelCreateEventFlag("vitasync_files_ev", 0, 0, NULL);
	if (ev_id < 0) {
		ret = ev_id;
		goto err_mem;
	}

	/* Below uvc_thread (0x3C) and the sound threads: files can wait. */
	thread_id = ksceKernelCreateThread("vitasync_files", files_thread,
					   0x60, 0x4000, 0, 0x10000, 0);
	if (thread_id < 0) {
		ret = thread_id;
		goto err_ev;
	}
	ret = ksceKernelStartThread(thread_id, 0, NULL);
	if (ret < 0)
		goto err_thread;
	return 0;

err_thread:
	ksceKernelDeleteThread(thread_id);
	thread_id = -1;
err_ev:
	ksceKernelDeleteEventFlag(ev_id);
	ev_id = -1;
err_mem:
	ksceKernelFreeMemBlock(mem_uid);
	mem_uid = -1;
	mem = NULL;
	return ret;
}

void files_fini(void)
{
	if (thread_id >= 0) {
		exiting = 1;
		files_set_online(0);
		ksceKernelWaitThreadEnd(thread_id, NULL, NULL);
		ksceKernelDeleteThread(thread_id);
		thread_id = -1;
	}
	if (ev_id >= 0) {
		ksceKernelDeleteEventFlag(ev_id);
		ev_id = -1;
	}
	if (mem_uid >= 0) {
		ksceKernelFreeMemBlock(mem_uid);
		mem_uid = -1;
		mem = NULL;
	}
}

void files_set_online(int on)
{
	if (ev_id < 0)
		return;
	if (!on && online) {
		ksceUdcdReqCancelAll(ep_in);
		ksceUdcdReqCancelAll(ep_out);
	}
	online = on ? 1 : 0;
	online_gen++;
	ksceKernelSetEventFlag(ev_id, EV_WAKE);
}
