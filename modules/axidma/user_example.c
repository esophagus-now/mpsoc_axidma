/* 

Please note: This example also uses the pinner driver.
 
*/

#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <fcntl.h>
#include <sys/mman.h>
#include <unistd.h>
#include <string.h>
#include "pinner.h"

#define SG_LIST_SIZE 4096
#define BUF_SIZE 5000

#define AXI_DMA_SPAN 0x1000

//Format of a scatter-gather entry
typedef struct {
    unsigned next_desc_lsb  :32; //Big or little endian?
    unsigned next_desc_msb  :32;
    unsigned buffer_lsb     :32;
    unsigned buffer_msb     :32;
    unsigned long           :64; //unused
    
    struct {
        unsigned len        :26;
        unsigned eof        :1;
        unsigned sof        :1;
        unsigned            :4;
    } control;
    
    struct {
        unsigned len        :26;
        unsigned eof        :1;
        unsigned sof        :1;
        unsigned int_err    :1;
        unsigned slave_err  :1;
        unsigned decode_err :1;
        unsigned success    :1;
    } status;
    
    unsigned app0           :32;
    unsigned app1           :32;
    unsigned app2           :32;
    unsigned app3           :32;
    unsigned app4           :32;
} sg_entry;

//Format of the AXI DMA's registers
typedef struct {
    uint32_t    MM2S_DMACR;
    uint32_t    MM2S_DMASR;
    uint32_t    MM2S_curdesc_lsb;
    uint32_t    MM2S_curdesc_msb;
    uint32_t    MM2S_taildesc_lsb;
    uint32_t    MM2S_taildesc_msb;
    
    uint32_t    unused[6];
    
    uint32_t    S2MM_DMACR;
    uint32_t    S2MM_DMASR;
    uint32_t    S2MM_curdesc_lsb;
    uint32_t    S2MM_curdesc_msb;
    uint32_t    S2MM_taildesc_lsb;
    uint32_t    S2MM_taildesc_msb;
} axidma_regs;

//Just to clean up the code slightly
typedef struct pinner_physlist physlist;
typedef struct pinner_handle handle;

//Helper function to pin a buffer in RAM and get the returned handle and
//physlist object    
int pin_buf(int fd, void *buf, unsigned buf_sz, handle *h, physlist *p) {
    struct pinner_cmd pin_cmd = {
        .cmd = PINNER_PIN,
        .usr_buf = buf,
        .usr_buf_sz = buf_sz,
        .handle = h,
        .physlist = p
    };
    
    int n = write(fd, &pin_cmd, sizeof(struct pinner_cmd));
    if (n < 0) {
        perror("Could not write pin command to pinner");
        return -1;
    }
    
    printf("num_entries = %u\n", p->num_entries);
    for (int i = 0; i < p->num_entries; i++) {
        printf("SG entry: address 0x%lX with length %u\n", p->entries[i].addr, p->entries[i].len);
    }
    
    return 0;
}

//Helper function to flush the cache on a pinned buffer
int flush_buf_cache(int fd, handle *h) {
    struct pinner_cmd flush_cmd = {
        .cmd = PINNER_FLUSH,
        .handle = h
    };
    
    int n = write(fd, &flush_cmd, sizeof(struct pinner_cmd));
    if (n < 0) {
        perror("Could not write pin command to pinner");
        return -1;
    }    
    sleep(3); //I don't know why, but sleeping seems to make it more consistent?
    return 0;
}

//Helper function to unpin a buffer
int unpin_buf(int fd, struct pinner_handle *h) {
    struct pinner_cmd unpin_cmd = {
        .cmd = PINNER_UNPIN,
        .handle = h
    };
    
    int n = write(fd, &unpin_cmd, sizeof(struct pinner_cmd));
    if (n < 0) {
        perror("Could not write unpin command to pinner");
        return -1;
    } 
}

//Doesn't have to be efficient. Just does a dumb linear search
uint64_t virt_to_phys(void *p, void *base, physlist *plist) {
    unsigned offset = p - base;
    for (int i = 0; i < plist->num_entries; i++) {
        if (offset >= plist->entries[i].len) {
            offset -= plist->entries[i].len;
            continue;
        }
        return plist->entries[i].addr + offset;
    }
    //Error: not found in physlist
    return (uint64_t) NULL;
}

//This walks through a physlist and writes scatter-gather entries to RAM.
//ASSUMPTION: this assumes that the buffer for the scatter-gather list is 
//physically contiguous, since it made this function a lot easier to write

//By the way, this aligns the descriptors on 128 byte boundaries. It's not
//necessary if you enable Allow Unaligned Transfers.
int write_physlist_sg(volatile void *sg_buf, physlist *sg_plist, physlist *plist) {
    volatile sg_entry *base = (volatile sg_entry*) sg_buf;
    for (int i = 0; i < plist->num_entries; i++) {
        volatile sg_entry *curdesc = (volatile sg_entry*) (((void*)base)+(128*i));
        uint64_t next_phys = virt_to_phys(((void*)base) + 128*(i+1), (void*)base, sg_plist);
        curdesc->next_desc_lsb = (uint32_t)(next_phys & 0xFFFFFFFF);
        curdesc->next_desc_msb = (uint32_t)((next_phys >> 32) & 0xFFFFFFFF);
        curdesc->buffer_lsb = (uint32_t)(plist->entries[i].addr & 0xFFFFFFFF);
        curdesc->buffer_msb = (uint32_t)((plist->entries[i].addr >> 32) & 0xFFFFFFFF);
        curdesc->control.sof = (i == 0);
        curdesc->control.eof = (i == (plist->num_entries - 1));
        curdesc->control.len = plist->entries[i].len;
    }
} 

int main(int argc, char **argv) {
    //Initialize all the variables we'll be using
    sg_entry *sg_list = NULL;
    char *buf = NULL;
    int fd = -1;
    int axidma_fd = -1;
    void *axidma_base = MAP_FAILED;
    
    //Check if arguments make sense
    if (argc != 2) {
        puts("Usage: axidma /dev/uioX");
        return 1;
    }
    
    //Open the pinner driver
    fd = open("/dev/pinner", O_RDWR);
    if (fd == -1) {
        perror("Could not open /dev/pinner");
        goto cleanup;
    }
    
    //Open the AXI DMA driver. This uses the command line argument as the file
    //path
    axidma_fd = open(argv[1], O_RDWR);
    if (axidma_fd == -1) {
        perror("Could not open AXI DMA UIO driver");
        goto cleanup;
    }
    
    //mmap the AXI DMA's registers to a user virtual address (in axidma_base)
    axidma_base = mmap(0, AXI_DMA_SPAN, PROT_READ | PROT_WRITE, MAP_SHARED, axidma_fd, 0);
    if (axidma_base == MAP_FAILED) {
        perror("Could not mmap AXI DMA registers");
        goto cleanup;
    }
    
    //This is how I make sure that the scatter-gather buffer is physically 
    //contiguous: by allocating an entire page that is page-aligned. Please note
    //that if I had allocated a larger buffer, there is no guarantee it would 
    //have been contiguous
    if(posix_memalign((void**)&sg_list, 4096, SG_LIST_SIZE) != 0) {
        perror("Could not allocate memory");
        goto cleanup;
    }
    
    //Allocate space for data returned from the FPGA
    buf = malloc(BUF_SIZE);
    if (!latencies_buf) {
        perror("Could not allocate buffer for data");
        goto cleanup;
    }
    
    //Now we'll pin the scatter-gather list buffer. We declare the structs that
    //the driver will fill in:
    handle sg_handle;
    physlist sg_plist;
    
    //And now we actually perform the pinning
    puts("Pinning SG entry buffer...");
    if (pin_buf(fd, sg_list, SG_LIST_SIZE, &sg_handle, &sg_plist) < 0) {
        goto cleanup;
    }
    
    //Same thing, but for the data buffer
    handle buf_handle;
    physlist buf_plist;
    
    puts("Pïnning data buffer...");
    if (pin_buf(fd, buf, BUF_SIZE, &buf_handle, &buf_plist) < 0) {
        goto cleanup;
    }
    
    //Fill in the scatter-gather entries
    write_physlist_sg(sg_list, &sg_plist, &buf_plist);
    
    //Flush cache
    flush_buf_cache(fd, &sg_handle);
    
    //Now we actually send the commands to the AXI DMA's registers
    //This follows the programming sequence in the product guide. First, we 
    //write the pointer to the first descriptor
    volatile axidma_regs *regs = (volatile axidma_regs *) axidma_base;
    unsigned long curdesc_phys = virt_to_phys(sg_list, sg_list, &sg_plist);
    regs->S2MM_curdesc_lsb = (uint32_t) (curdesc_phys & 0xFFFFFFFF);
    regs->S2MM_curdesc_msb = (uint32_t) ((curdesc_phys>>32) & 0xFFFFFFFF);
    //Enable IOC interrupts, and set run/stop to 1
    regs->S2MM_DMACR = 0b1000000000001; 
    //Now write the pointer to the last descriptor. This starts the transfer
    unsigned long taildesc_phys = virt_to_phys(((void*)sg_list) + 128*(latencies_plist.num_entries-1), sg_list, &sg_plist);
    regs->S2MM_taildesc_lsb = (uint32_t) (taildesc_phys & 0xFFFFFFFF);
    regs->S2MM_taildesc_msb = (uint32_t) ((taildesc_phys>>32) & 0xFFFFFFFF);
    
    
    //At this point, transfer has started. Wait for the interrupt!
    puts("Waiting for DMA to finish!");
    fflush(stdout);
    
    unsigned pending;
    read(axidma_fd, &pending, sizeof(pending));
    
    //For extra safety, flush the cache (I don't think this is necessary)
    flush_buf_cache(fd, &sg_handle);
    
    //Now we're done, so unpin the buffers, unmap buffers, free them, etc.
    if(unpin_buf(fd, &sg_handle) < 0) {
        goto cleanup;
    }
    printf("Completion status: %08x\n", *((uint32_t*)sg_list + 7));
    
    if(unpin_buf(fd, &latencies_handle) < 0) {
        goto cleanup;
    }
    
    for (int i = 0; i < LATENCIES_SIZE; i++) {
        printf("%02x ", +latencies_buf[i] & 0xFF);
    }
    
    puts("");
    
    cleanup:
    
    if (latencies_buf) free(latencies_buf);
    if (sg_list) free(sg_list);
    if (axidma_base != MAP_FAILED) munmap(axidma_base, AXI_DMA_SPAN);
    if (axidma_fd != -1) close(axidma_fd);
    if (fd != -1) close(fd);
}
