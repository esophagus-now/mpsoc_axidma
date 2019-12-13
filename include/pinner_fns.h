#ifndef PINNER_FNS_H
#define PINNER_FNS_H 1
#include "pinner.h"

//Just so that the user doesn't see open() and close() system calls directly.
//Returns -1 on error.
int pinner_open();
void pinner_close(int fd);

//Helper function to pin a buffer in RAM and get the returned handle and
//physlist object. Returns -1 on error
int pin_buf(int fd, void *buf, unsigned buf_sz, struct pinner_handle *h, struct pinner_physlist *p);

//Helper function to flush the cache on a pinned buffer. Returns -1 on error
int flush_buf_cache(int fd, struct pinner_handle *h);

//Helper function to unpin a buffer. Returns -1 on error
int unpin_buf(int fd, struct pinner_handle *h);


#endif
