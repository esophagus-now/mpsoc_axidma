#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <unistd.h>
#include "pinner.h"
#include "axidma.h"
#include "pinner_fns.h"

#define DATA_BUF_SIZE 20000
//Ensure DATA_BUF_SIZE >= NUM_BUFFERS*BUFFER_SZ
#define NUM_BUFFERS 10
#define BUFFER_SZ 1600

int main(int argc, char **argv) {
    if (argc != 2) {
        puts("Usage: example /dev/uioN, where /dev/uioN is the AXI DMA driver");
        return -1;
    }
    
    axidma_ctx *ctx = axidma_open(argv[1]);
    if (!ctx) {
        return -1;
    }
    
    int pinner_fd = pinner_open();
    
    char *sg_buf = malloc(5000);
    
    //For the sake of testing, poison the initial SG list to see what's going on
    for (int i = 0; i < 5000; i++) {
        sg_buf[i] = 0x00;
    }
    
    char *data_buf = malloc(DATA_BUF_SIZE);
    
    struct pinner_physlist sg_plist;
    struct pinner_handle sg_handle;
    int rc = pin_buf(pinner_fd, sg_buf, 5000, &sg_handle, &sg_plist);
    if (rc < 0) {
        return -1;
    }
    
    struct pinner_physlist data_plist;
    struct pinner_handle data_handle;
    rc = pin_buf(pinner_fd, data_buf, DATA_BUF_SIZE, &data_handle, &data_plist);
    if (rc < 0) {
        return -1;
    }
    
    sg_list *lst = axidma_list_new(sg_buf, &sg_plist, data_buf, &data_plist);
    
    for (int i = 0; i < NUM_BUFFERS; i++) {
        rc = axidma_add_entry(lst, BUFFER_SZ);
        if (rc == ADD_ENTRY_SG_OOM) {
            puts("Ran out of memory for SG descriptors");
            break;
        } else if (rc == ADD_ENTRY_BUF_OOM) {
            puts("Ran out of memory for data");
            break;
        } else if (rc == ADD_ENTRY_ERROR) {
            puts("Unrecoverable error");
            exit(-1);
        }
    }
    
    axidma_write_sg_list(ctx, lst, pinner_fd, &sg_handle);
    
    axidma_s2mm_transfer(ctx, 1, 0);
    
    s2mm_buf buf = axidma_dequeue_s2mm_buf(lst);
    int cnt = 0;
    while (buf.code != END_OF_LIST) {
        
        if(buf.code == TRANSFER_SUCCESS) cnt++;
        printf("Received %s buffer of length %u:\n", (buf.code == TRANSFER_SUCCESS ? "good" : "bad"), buf.len);
        for (int i = 0; i < (buf.len) / 64; i++) {
            uint32_t *word_ptr = ((uint32_t*) buf.base) + 16*i;
            for (int j = 15; j >= 0; j--) {
                printf("%08x ", word_ptr[j]);
            }
        }
        puts("");
        
        buf = axidma_dequeue_s2mm_buf(lst);
    }
    
    printf("Got %d good packets out of %d\n", cnt, NUM_BUFFERS);
    
    axidma_list_del(lst);
    
    axidma_close(ctx);
    
    unpin_buf(pinner_fd, &data_handle);
    unpin_buf(pinner_fd, &sg_handle);
    
    pinner_close(pinner_fd);
    return 0;
}
