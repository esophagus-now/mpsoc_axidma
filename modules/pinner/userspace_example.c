#include <stdio.h>
#include <stdlib.h>
#include <fcntl.h>
#include <unistd.h>
#include <string.h>
#include "pinner.h"

#define BUF_SIZE 5215

int main() {
    char *mybuf = NULL;
    int fd = -1;
    
    int i;
    int n;
    
    //Open the device file
    fd = open("/dev/pinner", O_RDWR);
    if (fd == -1) {
        perror("Could not open /dev/pinner");
        goto cleanup;
    }
    
    //Allocate a buffer in userspace (contiguous in the virtual address space)
    mybuf = malloc(BUF_SIZE);
    
    //Driver will fill these with pinning information
    struct pinner_handle handle;
    struct pinner_physlist plist;
    
    //Pin the buffer
    struct pinner_cmd pin_cmd = {
        .cmd = PINNER_PIN,
        .usr_buf = mybuf,
        .usr_buf_sz = BUF_SIZE,
        .handle = &handle,
        .physlist = &plist
    };
    n = write(fd, &pin_cmd, sizeof(struct pinner_cmd));
    if (n < 0) {
        perror("Could not write pin command to pinner");
        goto cleanup;
    }
    
    //Print out physical address info
    printf("plist.num_entries = %u\n", plist.num_entries);
    for (int i = 0; i < plist.num_entries; i++) {
        printf("SG entry: address 0x%lX with length %u\n", plist.entries[i].addr, plist.entries[i].len);
    }
    
    //Example of flushing the cache for the buffer. You only need to do this if
    //you modify after pinning it, and the external device is accessing the
    //memory without a hardware cache coherence mechanism (i.e. the S_AXI_HPC0_FPD
    //slave AXI on the PS has a hardware coherence manager).
    struct pinner_cmd flush_cmd = {
        .cmd = PINNER_FLUSH,
        .usr_buf = mybuf,
        .usr_buf_sz = BUF_SIZE
    };
    n = write(fd, &flush_cmd, sizeof(struct pinner_cmd));
    if (n < 0) {
        perror("Could not flush buffer");
        goto cleanup;
    }
    
    //At this point, replace the following code with whatever triggers your 
    //external memory accessor. The information about physical addresses is in
    //the pinner_physlist struct.
    
    puts("Will now sleep for 30 seconds... do what you want with that memory until then!");
    fflush(stdout);
    sleep(30);
    
    //For the hell of it, print out the first couple of bytes
    for (int i = 0; i < 16; i++) {
        printf("%02x ", (+mybuf[i]) & 0xFF);
    }
    
    //Unpin the buffer
    struct pinner_cmd unpin_cmd = {
        .cmd = PINNER_UNPIN,
        .handle = &handle
    };
    n = write(fd, &unpin_cmd, sizeof(struct pinner_cmd));
    if (n < 0) {
        perror("Could not write unpin command to pinner");
        goto cleanup;
    }
    
    puts("");
    
    cleanup:
    if (mybuf) free(mybuf);
    if (fd != -1) close(fd);
}
