#ifndef PINNER_H
#define PINNER_H 1

//Max number of pages in a single pinning
#define PINNER_MAX_PAGES 1024

#define PINNER_PIN 1
#define PINNER_UNPIN 2
#define PINNER_FLUSH 3

//Normally I would want this to be an opaque struct, but there's no easy way to
//do that when kernel and userspace share a header
//To the user: don't touch this!!
struct pinner_handle {
    unsigned user_magic; //Must match the magic in the proc_info struct.
    unsigned pin_magic;  //Must match the magic in the pinning struct
};

struct pinner_physlist_entry {
    unsigned long addr;
    unsigned len;
};
struct pinner_physlist {
    unsigned num_entries;
    struct pinner_physlist_entry entries[PINNER_MAX_PAGES];
};

struct pinner_cmd {
    unsigned cmd;
    void *usr_buf;
    unsigned usr_buf_sz;
    struct pinner_handle *handle; //Not sure if this is how I want to do it
    struct pinner_physlist *physlist;
};


#endif
