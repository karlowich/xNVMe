#ifndef SYS_UIO_H
#define SYS_UIO_H
#include <inttypes.h>
#include <unistd.h>

struct iovec
{
	void *iov_base;  /* Base address of a memory region for input or output */
	size_t iov_len;   /* The size of the memory pointed to by iov_base */
};

#endif /* SYS_UIO_H */
