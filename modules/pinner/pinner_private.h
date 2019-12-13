#ifndef PINNER_PRIVATE_H
#define PINNER_PRIVATE_H 1

#include <linux/scatterlist.h> //For scatterlist struct

struct pinning {
    struct list_head list;
    int num_sg_ents;
    struct scatterlist *sglist;
    unsigned magic; //Helps prevent problems where the user accidentally (or
    //on purpose) fiddled around with the handle we gave them. Should be generated
    //with get_random_bytes.
};

struct proc_info {
    struct list_head list;
    struct list_head pinning_list;
    unsigned magic; //Helps prevent problems where the user accidentally (or
    //on purpose) fiddled around with the handle we gave them. Should be generated
    //with get_random_bytes.
};

#endif
