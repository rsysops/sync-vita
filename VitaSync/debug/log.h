#ifndef LOG_H
#define LOG_H

#include <stdio.h>
#include <string.h>

/* ur0: (internal storage) because VitaShell's USB transfer never hands it
 * to the PC: the log can be written at any time, even during a transfer. */
#define LOG_PATH "ur0:data/"
#define LOG_FILE LOG_PATH "VitaSync_log.txt"

void log_reset();
void log_write(const char *buffer, size_t length);
void log_flush();

#ifdef DEBUG
#  define LOG_TO_FILE(...) \
	do { \
		char buffer[256]; \
		snprintf(buffer, sizeof(buffer), ##__VA_ARGS__); \
		log_write(buffer, strlen(buffer)); \
	} while (0)
#else
#  define LOG_TO_FILE(...) (void)0
#endif

#define TEST_CALL(f, ...) ({ \
	int ret = f(__VA_ARGS__); \
	LOG_TO_FILE(# f " returned 0x%08X\n", ret); \
	ret; \
})

#define LOG_PADDR(vaddr) LOG_TO_FILE("paddr(" #vaddr "): 0x%08lX\n", get_paddr(vaddr));

#endif
