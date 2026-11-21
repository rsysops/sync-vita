#include "log.h"
#include <psp2kern/io/fcntl.h>
#include <psp2kern/io/stat.h>

static unsigned int log_buf_ptr = 0;
static char log_buf[16 * 1024];
static int log_file_started;

/* Forget buffered lines; the next flush starts a new file. */
void log_reset()
{
	log_buf_ptr = 0;
	log_file_started = 0;
}

void log_write(const char *buffer, size_t length)
{
	if ((log_buf_ptr + length) >= sizeof(log_buf))
		return;

	memcpy(log_buf + log_buf_ptr, buffer, length);

	log_buf_ptr = log_buf_ptr + length;
}

/* Write the lines buffered since the last flush. The first successful flush
 * truncates the file, later ones append. If the file can't be opened (ux0:
 * not mounted yet), the lines stay buffered for the next try. */
void log_flush()
{
	SceUID fd;

	if (log_buf_ptr == 0)
		return;

	ksceIoMkdir(LOG_PATH, 6);

	fd = ksceIoOpen(LOG_FILE, SCE_O_WRONLY | SCE_O_CREAT |
		(log_file_started ? SCE_O_APPEND : SCE_O_TRUNC), 6);
	if (fd < 0)
		return;

	ksceIoWrite(fd, log_buf, log_buf_ptr);
	ksceIoClose(fd);

	log_file_started = 1;
	log_buf_ptr = 0;
}
