#include "log.h"
#include <psp2kern/io/fcntl.h>
#include <psp2kern/io/stat.h>
#include <psp2kern/kernel/threadmgr.h>

static unsigned int log_buf_ptr = 0;
static char log_buf[16 * 1024];
static int log_file_started;
/* uvc_thread and the breadcrumb logger thread both write. */
static SceUID log_mutex = -1;

static void log_lock(void)
{
	if (log_mutex >= 0)
		ksceKernelLockMutex(log_mutex, 1, NULL);
}

static void log_unlock(void)
{
	if (log_mutex >= 0)
		ksceKernelUnlockMutex(log_mutex, 1);
}

/* Forget buffered lines; the next flush starts a new file. */
void log_reset()
{
	if (log_mutex < 0)
		log_mutex = ksceKernelCreateMutex("vitasync_log_mutex", 0, 0, NULL);
	log_lock();
	log_buf_ptr = 0;
	log_file_started = 0;
	log_unlock();
}

void log_write(const char *buffer, size_t length)
{
	log_lock();
	if ((log_buf_ptr + length) < sizeof(log_buf)) {
		memcpy(log_buf + log_buf_ptr, buffer, length);
		log_buf_ptr = log_buf_ptr + length;
	}
	log_unlock();
}

/* Write the lines buffered since the last flush. The first successful flush
 * truncates the file, later ones append. If the file can't be opened, the
 * lines stay buffered for the next try. */
void log_flush()
{
	SceUID fd;

	log_lock();
	if (log_buf_ptr == 0) {
		log_unlock();
		return;
	}

	ksceIoMkdir(LOG_PATH, 6);

	fd = ksceIoOpen(LOG_FILE, SCE_O_WRONLY | SCE_O_CREAT |
		(log_file_started ? SCE_O_APPEND : SCE_O_TRUNC), 6);
	if (fd >= 0) {
		int st;

		ksceIoWrite(fd, log_buf, log_buf_ptr);
		/* Force the lines to storage: without it, a freeze followed by
		 * a forced power-off lost everything since the system's last
		 * save (build 94's first test). */
		ksceIoSyncByFd(fd, &st);
		ksceIoClose(fd);
		log_file_started = 1;
		log_buf_ptr = 0;
	}
	log_unlock();
}
