#include <stdio.h>
#include <fcntl.h>
#include <unistd.h>
#include <string.h>
#include <errno.h>
#include "pinner.h"
#include "pinner_fns.h"


int pinner_open() {
    int fd = open("/dev/pinner", O_RDWR);
    if (fd == -1) {
        perror("Could not open /dev/pinner");
    }
    return fd;
}

void pinner_close(int fd) {
    if (fd != -1) close(fd);
}

//Helper function to pin a buffer in RAM and get the returned handle and
//physlist object. Returns -1 on error 
int pin_buf(int fd, void *buf, unsigned buf_sz, struct pinner_handle *h, struct pinner_physlist *p) {
    struct pinner_cmd pin_cmd = {
        .cmd = PINNER_PIN,
        .usr_buf = buf,
        .usr_buf_sz = buf_sz,
        .handle = h,
        .physlist = p
    };
    
    if (fd == -1) {
        fprintf(stderr, "Error: invalid file descriptor. Did open_pinner() fail?");
        errno = EINVAL;
        return -1;
    }
    
    int n = write(fd, &pin_cmd, sizeof(struct pinner_cmd));
    if (n < 0) {
        perror("Could not write pin command to pinner");
        return -1;
    }
    /*
    printf("num_entries = %u\n", p->num_entries);
    for (int i = 0; i < p->num_entries; i++) {
        printf("SG entry: address 0x%lX with length %u\n", p->entries[i].addr, p->entries[i].len);
    }*/
    
    return 0;
}

//Helper function to flush the cache on a pinned buffer. Returns -1 on error
int flush_buf_cache(int fd, struct pinner_handle *h) {
    struct pinner_cmd flush_cmd = {
        .cmd = PINNER_FLUSH,
        .usr_buf_sz = 0b00,
        .handle = h
    };
    
    if (fd == -1) {
        fprintf(stderr, "Error: invalid file descriptor. Did open_pinner() fail?");
        errno = EINVAL;
        return -1;
    }
    
    int n = write(fd, &flush_cmd, sizeof(struct pinner_cmd));
    if (n < 0) {
        perror("Could not write pin command to pinner");
        return -1;
    }    
    sleep(3); //So aboslutely none of those kernel functions flush the cache...
    return 0;
}

//Helper function to unpin a buffer. Returns -1 on error
int unpin_buf(int fd, struct pinner_handle *h) {
    struct pinner_cmd unpin_cmd = {
        .cmd = PINNER_UNPIN,
        .handle = h
    };
    
    if (fd == -1) {
        fprintf(stderr, "Error: invalid file descriptor. Did open_pinner() fail?");
        errno = EINVAL;
        return -1;
    }
    
    int n = write(fd, &unpin_cmd, sizeof(struct pinner_cmd));
    if (n < 0) {
        perror("Could not write unpin command to pinner");
        return -1;
    } 
    
    return 0;
}
