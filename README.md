### TL;DR


A complete AXI DMA solution (for the Zynq MPSoC) that does not require you to 
write a device tree overlay.


# Updates

After reading some stuff in ldd3 (https://lwn.net/Kernel/LDD3/) I might have 
discovered why the caching isn't working properly: the `dma_map_*` functions 
don't play nicely when you only want to map a fraction of a page. When I get 
back to Toronto I can see if changing this fixes the problem. Specifically, all 
you would have to do is, when building the scatterlist in the `pinner` driver, 
is do map the entire page even if only a fraction of it is needed. 

Alternatively, one could use `poix_memalign` in user space and always allocate 
an integer number of pages,

# The new AXIDMA library

This repo is meant to be a general attempt to make the AXI DMA easier to use. 
It includes some custom kernel modules in the `modules/` folder, and a userspace 
library in the `include/` and `src/` folders.

This file tries to explain the API for the user library. Check out the READMEs 
in the `modules/pinner/` and `modules/axidma/` folders for more information about 
the drivers themselves.

The background explains virtual memory and pinned pages. Afterwards, I explain 
the general idea of the user API. Finally, I give a detailed function reference.

## Background

### Virtual Memory

To access memory in RAM, you need to give it an address. The actual pattern of 
voltages on the RAM's wires is known as a "physical address".

All modern systems have something called a Memory Management Unit (MMU). Every 
address that comes out of the processor first passes through the MMU:

```           
                                              +-----+
                                              |     |
                                              |     |
                                              |     |
                                              |     |
                                              |     |
    +---------+                               |     |
    |         |                               |     |
    |         | Virtual   +-----+  Physical   |     |
    |   CPU   |---------->| MMU |------------>| RAM |
    |         |           +-----+             |     |
    |         |                               |     |
    +---------+                               |     |
                                              |     |
                                              |     |
                                              |     |
                                              |     |
                                              |     |
                                              +-----+
```

The MMU translates every single address before it reaches the RAM. The 
translation itself is specified by the kernel, and can be changed at any time. 
We call the addresses seen by the CPU "virtual addresses".

In most computers, the MMU only translates one part of the address:

```
        Virtual                                   Physical
  +--------------------+       +-----+      +--------------------+
  |       A        | B |-------| MMU |----->|     f(A)       | B |
  +--------------------+       +-----+      +--------------------+
```

Again, the OS kernel decides what the translation function f(A) is, and it can 
be changed on the fly. In fact, every process has its own personal translation 
function.

Typically, B is 12 bits long (i.e. the MMU doesn't touch the lower 12 bits of 
the address). This means that there are 2^12 contiguous virtual addresses (i.e. 
2^12 virtual addresses that have the same value for A) that map to 2^12 
contiguous physical addresses. This type of contiguous chunk is called "page".


### Pinned Pages

A user process in Linux has some amount of pages it can use. However, at any 
point, the Linux kernel may relocate one of these pages or even swap it out to 
disk (and still, the user process can continue the use the exact same virtual 
address; the kernel will simple edit f(A)). A "pinned" page is one which is 
guaranteed to be in RAM and to stay at the same physical address.

I think it's pretty clear why we would need this for DMA; if a page was moved 
or swapped out, the DMA would have no way of knowing this.


## Overview 

This library does two main things: 
1. Allow you to pin some of your process's 
pages, and 
2. control the AXI DMA in the FPGA.

There's also 1.5) get the physical addresses for your pages. You get this for 
free when you pin pages.

Let's walk through using the pinning part of the API. Note that I left out the 
error-checking code to keep things simple.


## Pinning API

Suppose you had a buffer:

```C
    char *mybuffer = malloc(10000);
```

To pin it, you would simply call `pin_buf()`. This function returns the physical 
address information for this buffer, as well as a handle that you would use in 
later function calls:

```C    
    int pinner_fd = pinner_open();
    
    struct pinner_physlist my_plist; //Stores physical address information
    struct pinner_handle my_handle; //Stores the handle for flushing/unpinning
    
    pin_buf(pinner_fd, my_buffer, 10000, &my_handle, &my_plist);
```

Once you've pinned a buffer, you can flush its cache* or unpin it:

```C
    flush_buf_cache(pinner_fd, &my_handle);
    unpin_buf(pinner_fd, &my_handle);
```    

\* Currently, the cache flushing just causes your program to sleep for 3 
seconds. I tried some stuff in the kernel driver, but it didn't work. The only 
things that reliably flushes the cache is letting other processes run for a 
while. I hope I can find a way to fix this!

The handle is basically just a couple of integers that the kernel driver uses 
to locate information about your buffer. Don't modify them!

The `physlist` looks like this:
```C
    struct pinner_physlist {
        unsigned num_entries;
        struct pinner_physlist_entry entries[PINNER_MAX_PAGES];
    };
```

It's simply an array of entries, which look like this:
```C
    struct pinner_physlist_entry {
        unsigned long addr; //Physical address
        unsigned len; //length
    };
```
Even though your buffer appears contiguous in virtual memory, the physical 
pages may be scattered all over RAM. The entries in the array are in order of 
increasing virtual address.


## AXI DMA API

This is more complicated. Since our buffers are scattered all over the place, 
we'll need to use the AXI DMA's scatter-gather mode. This means that we'll need 
to pin a _second_ buffer in physical memory, which the DMA will access directly:

```C
    char *sg_buf = malloc(2000);

    struct pinner_physlist sg_plist;
    struct pinner_handle sg_handle;
    
    pin_buf(pinner_fd, sg_buf, 2000, &sg_handle, &sg_plist);
```

Now we'll get to work on building up the data structures. This occurs in three 
steps:

1. Filling up an `sg_list` with information about our desired buffer sizes
2. Writing the `sg_list` to the pinned physical buffer
3. Starting the actual transaction

After this, there are some function for gettin information about the transfer.


### Filling the sg_list

First, we'll initialize an `axidma_ctx` struct. This struct stores a few state 
variables maintained by the AXI DMA library:
```C
    axidma_ctx *ctx = axidma_open("/dev/uioN");
```    
The argument to this function should be the path to the AXI DMA device file 
(for instructions on finding it, see `modules/axidma/README.txt`).

Next, we'll initialize an `sg_list`:
```C
    sg_list *lst = axidma_list_new(sg_buf, &sg_plist, my_buf, &my_plist);
```
The reason we're giving it the virtual and physical address information is so 
we can use the following function:
```C
    axidma_add_entry(lst, BUFFER1_SZ);
    axidma_add_entry(lst, BUFFER2_SZ);
    ...
    axidma_add_entry(lst, BUFFERn_SZ);
```
This function will divide up `my_buf` into a chunk of size `BUFFER1_SZ`, 
immediately followed by a chunk of size `BUFFER2_SZ`, and so on, and maintain an 
internal list of the scatter-gather information for each chunk. _IT DOES NOT 
MODIFY EITHER_ `my_buf` _OR_ `sg_buf`.


### Writing the scatter-gather descriptors to pinned memory

After calling axidma_add_entry as many times as you want, you can write the 
scatter-gather descriptors to the pinned sg_buf:
```C
    axidma_write_sg_list(ctx, lst, pinner_fd, &sg_handle);
```
This function will traverse the scatter-gather information list and actually 
write the descriptors with the right formatting to the pinned buffer, where the 
AXI DMA will read them. It also flushes the cache for `sg_buf`, which is why we 
need to give it the pinner file descriptor and `sg_handle`.


### Starting the transfer(s)

Now you simply call
```C
    axidma_s2mm_transfer(ctx, ENABLE_INTERRUPT, ENABLE_TIMEOUT);
```
to perform the transfer. Note that once your `sg_list` has been written to the 
pinned buffer, you can call `axidma_s2mm_transfer` without rewriting it, and the 
AXI DMA will overwrite `my_buf` with new data.


### Using the returned results

PLEASE NOTE: this is the part of the library I most recently put in. It's not 
an extremely convenient API, so I might change it in the future.

After calling `axidma_s2mm_transfer`, simply do
```C
    s2mm_buf my_returned_buf = axidma_dequeue_s2mm_buf(lst);
```
This returns a struct which looks like:
```C
    typedef struct {
        void *base;
        unsigned len;
        buf_code code;
    } s2mm_buf;
```
`base` is the pointer to the beginning of the buffer, and `len` is its length. `code` 
is an enum and can be:

    TRANSFER_SUCCESS: This is a valid buffer
    TRANSFER_FAILED: This is an invalid buffer, but here is the data anyway
    END_OF_LIST: This buffer does not contain data. We have reached the end of the list

You can only traverse the list once; it does not "loop back around".

## Future Work


* Write in the API function reference (ugh)

* I really want to fix that cache flushing problem.

* It shouldn't be too hard to add MM2S support. 

* The API for the returned results isn't very good. I should improve it.
