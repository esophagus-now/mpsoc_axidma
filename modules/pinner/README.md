Memory-pinning driver for Linux.

Goal is to allow a userspace program to ask the kernel to pin some of their 
pages in RAM, and that the kernel will pass back the physical addresses. This 
will form part of a larger strategy to make it easier to use the AXI DMA.

# Limitations

The maximum size of an individual pinned buffer cannot exceed PINNER_MAX_PAGES 
pages in size. You can pin more than buffer, though.

I tried every cache flushing API I could find, and none of them seemed to work. 
The only reliable solution I've found is to let your user process sleep for a 
few seconds. I'm still trying to fix this!

# Userspace API

## `pinner_cmd` struct

The driver works by writing pinner_cmd structs using the `write()` system call. A 
`pinner_cmd` struct looks like this:

```C
    struct pinner_cmd {
        unsigned cmd;
        void *usr_buf;
        unsigned usr_buf_sz;
        struct pinner_handle *handle; //Not sure if this is how I want to do it
        struct pinner_physlist *physlist;
    };
```

`cmd`:
    Can be either `PINNER_PIN`, `PINNER_FLUSH`, or `PINNER_UNPIN`.
    With `PINNER_PIN`, fill in `usr_buf`, `usr_buf_sz`, `handle`, and `physlist`
    With `PINNER_FLUSH`, fill in `usr_buf` and `usr_buf_sz`
    With `PINNER_UNPIN`, you only need to fill in `handle`

`usr_buf`:
    Pointer to the beginning of the buffer you wish to pin

`usr_buf_sz`:
    Length of buffer you wish to pin

`handle`:
    Address of a `pinner_handle` struct (explained in more detail below)

`physlist`:
    Address of a `pinner_physlist` struct (explained in more detail below)


## `pinner_handle` struct

When you perform a `PINNER_PIN` command, the driver will fill the `pinner_handle` 
struct whose address was given in `pinner_cmd.handle`.

When you peform a `PINNER_UNPIN` command, you can use a handle returned from one 
of your pin commands to select which buffer to unpin.

    In case you're curious, the handle is just an integer used internally by 
    the driver to locate some internal data it needs to do the unpinning. DON'T 
    TOUCH IT, or you will not be able to unpin the memory without closing the 
    file descriptor.


## `pinner_physlist` struct

The driver will fill this structure with the physical address information of 
your pinned buffer. Although the pinned buffer may appear contiguous in 
userspace, it may not be physically contiguous. This information is returned as 
follows:

```C
    struct pinner_physlist {
        unsigned num_entries;
        struct pinner_physlist_entry entries[PINNER_MAX_PAGES];
    };
```

`num_entries`:
    The number of discrete chunks in physical memory that make up the entire 
    pinned buffer.

`entries`:
    An array of `pinner_physlist_entry` structs. Each entry represents one 
    discrete chunk of the pinned buffer. Within this array, the entries are in 
    "the right order". That is, if visited each of the entries in turn, you 
    would visit increasing addresses in userspace.
    

## `pinner_physlist_entry` struct


This is simply a physical address and a length:

```C
    struct pinner_physlist_entry {
        unsigned long addr;
        unsigned len;
    };
```

# EXAMPLE

(moved to userspace_example.c in this folder)
