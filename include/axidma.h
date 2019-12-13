//At some point, this will be a shared library. Right now I just need to get
//something done, but I figured if I had to write code, I would try to make it
//easier to make into a .so later down the line


#ifndef AXIDMA_H
#define AXIDMA_H 1

//Started adding these version tags, cause I'm starting to lose track of what's
//going on. This code needs to be maintained in several places
#define AXIDMA_USERLIB_VERSION_MAJOR 1
#define AXIDMA_USERLIB_VERSION_MINOR 7

#include "pinner.h"


#define AXIDMA_NOT_FOUND 0xFFFFFFFF

//This cleans up the code slightly. I didn't use a typedef because I was worried
//about conflicts once this becomes a shared library.
#define handle  struct pinner_handle
#define physlist struct pinner_physlist

//Format of a scatter-gather descriptor
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
        unsigned complete   :1;
    } status;
    
    unsigned app0           :32;
    unsigned app1           :32;
    unsigned app2           :32;
    unsigned app3           :32;
    unsigned app4           :32;
} sg_descriptor;

/*
 * Node in a linked list of scatter-gather list entries
*/
typedef struct _sg_entry {
    //Linked list of sg_entry structs
    struct _sg_entry *prev;
    struct _sg_entry *next;
    
    //Offset into virtual memory. 
    unsigned sg_offset; //Used when writing the SG list to memory.
    unsigned data_offset; //Used when returning data to user
    
    //Fields in the ADI DMA SG entry
    //unsigned long nextdesc_phys; //Can (and should) compute this on the fly
    uint64_t buf_phys;
    unsigned len;    
    int is_SOF;
    int is_EOF;
} sg_entry;

/*
 * Struct which manages a scatter-gather list. It allows you to "allocate" 
 * buffers from the data buffer while updating the scatter-gather list entries
*/
typedef struct {
    sg_entry sentinel; //Head of linked list of sg_entries
    sg_entry *to_vist; //Used when returning buffer statuses
    
    void *sg_buf; //User virtual address to start of SG entry memory
    unsigned sg_offset; //Offset into sg_buf where next SG entry will go
    physlist const *sg_plist; //Phyiscal address information for SG list
    
    void *data_buf; //User virtual address to start of data memory
    unsigned data_offset; //Offset into data_buf where next buffer will be allocated
    physlist const *data_plist; //Physical address information for data buffer
} sg_list;


/*
 * Holds whatever state is needed per process
*/
typedef struct {
    int fd;
    void *reg_base;
    
    //Keeps track of which sg_list was written to physical memory
    sg_list *lst;
} axidma_ctx;


typedef enum {
    TRANSFER_SUCCESS,
    TRANSFER_FAILED,
    END_OF_LIST
} buf_code;

/*
 * Struct with info about a buffer returned by the AXI DMA 
*/
typedef struct {
    void *base;
    unsigned len;
    buf_code code;
} s2mm_buf;

//Functions to open and close an AXI DMA context.
axidma_ctx* axidma_open(char const* path);
void axidma_close(axidma_ctx *ctx);

//Functions to create and delete an sg_list objext
sg_list *axidma_list_new(void *sg_buf, physlist const *sg_plist,
                         void *data_buf, physlist const *data_plist);
void axidma_list_del(sg_list *lst);

//Functions for modifying an sg_list


typedef enum {
    ADD_ENTRY_SUCCESS,
    ADD_ENTRY_SG_OOM,
    ADD_ENTRY_BUF_OOM,
    ADD_ENTRY_ERROR
} add_entry_code;

/*
 * Apportions a new buffer from the the user's data buffer, and appends the
 * necessary entries to the SG list
 * 
 * Returns 0 on success, SG_OUT_OF_MEM if there is no space for the next SG 
 * entry, or BUF_OUT_OF_MEM if there is no space for the desired buffer
*/
add_entry_code axidma_add_entry(sg_list *lst, unsigned sz);

/*
 * Clears all the entries in an sg_list 
*/
void axidma_clear_list(sg_list *lst);

/*
 * Writes SG list to physical memory. Call this _before_ starting your trasnfer!
*/

void axidma_write_sg_list(axidma_ctx *ctx, sg_list *lst, int pinner_fd, handle *h);

/*
 * Writes the scatter-gather list entries to memory, then starts the transfer.
 * Set wait_irq to 0 if you don't want to wait for the interrupt
 * CALL axidma_write_sg_list FIRST!
 * TODO: find nice way to decouple from pinner_fns
*/
void axidma_s2mm_transfer(axidma_ctx *ctx, int wait_irq, int enable_timeout);

/*
 * Used for traversing buffers returned from an S2MM trasnfer
*/
s2mm_buf axidma_dequeue_s2mm_buf(sg_list *lst);

/*
 * Call this function if you want to re-traverse the returned buffers
*/
void axidma_reset_lst_traversal(sg_list *lst);

#undef physlist
#undef handle

#endif
