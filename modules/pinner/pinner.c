#include <linux/fs.h> //struct file, struct file_operations
#include <linux/init.h> //for __init, see code
#include <linux/module.h> //for module init and exit macros
#include <linux/miscdevice.h> //for misc_device_register and struct micdev
#include <linux/uaccess.h> //For copy_to_user and copy_from_user
#include <linux/mutex.h> //For mutexes
#include <asm/page.h> //For PAGE_SHIFT
#include <linux/mm.h> //For find_vma
#include <linux/random.h> //For get_random_bytes
#include <linux/list.h> //For linked lists
#include <linux/slab.h> //For kzalloc, kfree
#include <linux/stddef.h> //For offsetof
#include <linux/scatterlist.h> //For scatterlist struct
#include <linux/dma-mapping.h> //For dma_map_X
#include <asm/cacheflush.h> //For flush_cache_range
#include "pinner.h" //Custom data types and defines shared with userspace
#include "pinner_private.h" //Private custom data types and macros

static DEFINE_MUTEX(users_mutex);
static LIST_HEAD(users);

//Forward-declare miscdev struct
static struct miscdevice pinner_miscdev;

//This function only used in error-handling code
static void put_page_list(struct page **p, int num_pages) {
    int i;
    for (i = 0; i < num_pages; i++) {
        put_page(p[i]);
    }
}

static void pinner_put_sglist_pages(struct scatterlist *sglist, int num_entries) {
    int i;
    
    if(!sglist) {
        //sglist can be NULL in error-handling paths.
        //Gracefully ignore this function call. 
        return;
    }
    
    //This is the counterpart to get_user_pages_fast
    for (i = 0; i < num_entries; i++) {
        put_page(sg_page(&(sglist[i])));
    }
}

static void pinner_free_pinning(struct pinning *p) {
    //Unmap the scatterlist
    //TODO: allow user to set direction
    dma_unmap_sg(pinner_miscdev.this_device, p->sglist, p->num_sg_ents, DMA_BIDIRECTIONAL);
    
    //Put pages
    pinner_put_sglist_pages(p->sglist, p->num_sg_ents);
    
    //Free scatterlist
    kfree(p->sglist);
    
    //Remove pinning from list
    list_del(&(p->list));
    
    //Free pinning struct
    kfree(p);
}

static void pinner_free_pinnings(struct proc_info *info) {
    //printk(KERN_ALERT "Entered pinner_free_pinnings\n");
    //Iterate through the list of pinnings inside this proc_info struct
    //and free them all
    while (!list_empty(&(info->pinning_list))) {
        struct pinning *p = list_entry(info->pinning_list.next, struct pinning, list);
        pinner_free_pinning(p);
    }
}

static void pinner_free_proc_info(struct proc_info *info) {
    //printk(KERN_ALERT "Entered pinner_free_proc_info\n");
    //Free all the pinnings stored in this proc_info struct
    pinner_free_pinnings(info);
    
    //Remove from the list
    mutex_lock(&users_mutex); //Need to watch out for race conditions
    list_del(&(info->list));
    mutex_unlock(&users_mutex);
    
    //Free the struct itself
    kfree(info);
}

static int pinner_send_physlist(struct pinner_cmd *cmd, struct pinning *p) {
    int ret = 0;
    int n;
    int i;
    struct pinner_physlist_entry *entries = NULL;
    
    void *user_num_entries = cmd->physlist;
    void *user_entries = ((void *)cmd->physlist) + offsetof(struct pinner_physlist, entries);
    
    //Allocate space for the entries we'll copy to user space
    entries = kzalloc(p->num_sg_ents * (sizeof(struct pinner_physlist_entry)), GFP_KERNEL);
    if (!entries) {
        printk(KERN_ALERT "pinner: could not allocate buffer of size [%lu]\n", p->num_sg_ents * (sizeof(struct pinner_physlist_entry)));
        ret = -ENOMEM;
        goto send_physlist_cleanup;
    }
    
    //Walk through the struct scatterlist array in the pinning and write the
    //information into the struct_physlist_entries
    for (i = 0; i < p->num_sg_ents; i++) {
        entries[i].addr = p->sglist[i].dma_address + p->sglist[i].offset;
        entries[i].len = p->sglist[i].length;
    }
    
    //Write the num_entries field of the user's pinner_physlist
    n = copy_to_user(user_num_entries, &(p->num_sg_ents), sizeof(unsigned));
    if (n != 0) {
        printk(KERN_ALERT "pinner: could not copy num_entries to userspace\n");
        ret = -EAGAIN;
        goto send_physlist_cleanup;
    }
    //Write the entries themselves
    n = copy_to_user(user_entries, entries, p->num_sg_ents * (sizeof(struct pinner_physlist_entry)));
    if (n  != 0) {
        printk(KERN_ALERT "pinner: could not copy entries to userspace\n");
        ret = -EAGAIN;
        goto send_physlist_cleanup;
    }
    
    send_physlist_cleanup:
    if (entries) kfree(entries);
    return ret;
}

static int pinner_alloc_and_fill_sglist(struct page **page_arr, int num_pages, 
            struct pinning *p, unsigned long first_page_offset, unsigned total_sz) 
{
    int i;
    unsigned first_page_sz;
    
    //Make sure inputs are valid
    if (!page_arr || num_pages <= 0 || !p) {
        printk(KERN_ALERT "pinner: internal error when building scatterlist\n");
        return -EINVAL;
    }
    
    //Allocate an array of struct scatterlists in the pinning
    p->sglist = kzalloc(num_pages * (sizeof(struct scatterlist)), GFP_KERNEL);
    if (!(p->sglist)) {
        printk(KERN_ALERT "pinner: could not allocate buffer of size [%lu]\n", num_pages * (sizeof(struct scatterlist)));
        return -ENOMEM;
    }
    
    //This code is based off an answer on this stackoverflow post:
    //https://stackoverflow.com/questions/5539375/linux-kernel-device-driver-to-dma-from-a-device-into-user-space-memory
    //First page
    if (total_sz < PAGE_SIZE - first_page_offset) {
        first_page_sz = total_sz;
    } else {
        first_page_sz = PAGE_SIZE - first_page_offset;
    }
    sg_set_page(&(p->sglist[0]), page_arr[0], first_page_sz, first_page_offset);
    p->sglist[0].dma_address = page_to_phys(page_arr[0]);
    //Middle pages
    for (i = 1; i < num_pages - 1; i++) {
        sg_set_page(&(p->sglist[i]), page_arr[i], PAGE_SIZE, 0);
        p->sglist[i].dma_address = page_to_phys(page_arr[i]);
    }
    //Last page
    if (num_pages > 1) {
        sg_set_page(
            &(p->sglist[num_pages-1]), page_arr[num_pages-1],
            total_sz - (PAGE_SIZE - first_page_offset) - (num_pages-2)*PAGE_SIZE,
            0
        );
        p->sglist[num_pages-1].dma_address = page_to_phys(page_arr[num_pages-1]);
    }
    
    //Success
    return 0;
}

static int pinner_do_pin(struct pinner_cmd *cmd, struct proc_info *info) {
    int ret = 0;
    
    struct pinning *pin = NULL;
    unsigned long start;
    unsigned long first_pg_offset;
    unsigned long page_sz = (1 << PAGE_SHIFT);
    unsigned long page_mask = (page_sz - 1);
    int num_pages;
    int n;
    struct page **p = NULL;
    
    struct pinner_handle usr_handle;
    
    start = ((unsigned long)cmd->usr_buf | page_mask) - page_mask;
    first_pg_offset = (unsigned long)cmd->usr_buf - start;
    
    //Validate inputs from user's command
    //Note that we add first_pg_offset, as though we're pretending it's part 
    //of the buffer (after all, it's part of the memory we'll end up pinning!)
    num_pages = (first_pg_offset + cmd->usr_buf_sz + page_mask) / page_sz; // = ceil(usr_buf_sz / page_sz)
    if (num_pages > PINNER_MAX_PAGES) {
        printk(KERN_ALERT "pinner: exceeded maximum pinning size\n");
        ret = -EINVAL;
        goto do_pin_error;
    } else if (num_pages <= 0) {
        printk(KERN_ALERT "pinner: invalid pinning size\n");
        ret = -EINVAL;
        goto do_pin_error;
    }
    
    //Attempt to pin pages 
    p = kmalloc(num_pages * (sizeof(struct page *)), GFP_KERNEL);
    if (!p) {
        printk(KERN_ALERT "pinner: could not allocate buffer of size [%lu]\n", num_pages * (sizeof(struct page *)));
        ret = -ENOMEM;
        goto do_pin_error;
    }
    n = get_user_pages_fast(start, num_pages, 1, p);
    if (n != num_pages) {
        //Could not pin all the pages. Just quit and ask the user to try again
        printk(KERN_ERR "pinner: could not satisfy user request\n");
        ret = -EAGAIN;
        goto do_pin_error;
    }
    
    //Maintain our own internal bookkeeping (i.e. add pinning info to list inside proc_info)
    pin = kzalloc(sizeof(struct pinning), GFP_KERNEL);
    if (!pin) {
        printk(KERN_ALERT "pinner: could not allocate buffer of size [%lu]\n", sizeof(struct pinning));
        ret = -ENOMEM;
        goto do_pin_error;
    }
    pin->num_sg_ents = num_pages;
    ret = pinner_alloc_and_fill_sglist(p, num_pages, pin, first_pg_offset, cmd->usr_buf_sz);
    if (ret < 0) {
        goto do_pin_error;
    }
    //Note to self: look out for double-frees, since now these pages are managed by the pinning struct
    p = NULL; //For extra safety against double-freeing
    get_random_bytes(&(pin->magic), sizeof(pin->magic));
    list_add(&(pin->list), &(info->pinning_list)); //CAREFUL: list_add adds the first argument to the second
    
    //Perform the DMA mapping (whatever that means)
    //Well, I know it eventually defers to some architecture-specific assmebly
    //code, so I'm guess it turns off the cache (which is what I want)
    //TODO: allow user to set direction
    ret = dma_map_sg(pinner_miscdev.this_device, pin->sglist, pin->num_sg_ents, DMA_BIDIRECTIONAL);
    if (ret < 0) {
        printk(KERN_ALERT "pinner: Could not perform dma_map_sg\n");
        goto do_pin_error;
    }
    
    //Write the physical address info back to userspace
    ret = pinner_send_physlist(cmd, pin);
    if (ret < 0) {
        goto do_pin_error;
    }
    
    //Give the userspace program a handle that allows them to undo this pinning
    usr_handle.user_magic = info->magic;
    usr_handle.pin_magic = pin->magic;
    n = copy_to_user(cmd->handle, &usr_handle, sizeof(struct pinner_handle));
    if (n != 0) {
        printk(KERN_ALERT "pinner: could not copy handle to userspace\n");
        ret = -EAGAIN;
        goto do_pin_error;
    }
    
    return 0;
    
    do_pin_error:
    
    if (pin) {
        pinner_free_pinning(pin);
    } else if (p) {
        //This is in an else if, since p is inside the pinning struct and will
        //be freed in the call to pinner_free_pinning(p)
        put_page_list(p, num_pages);
        kfree(p);
    }
    return ret;
}


static int pinner_do_flush(struct pinner_cmd *cmd, struct proc_info *info) {
    struct list_head *cur; //For iterating
    struct pinner_handle usr_handle;
    int n;
    struct pinning *found = NULL;
    
    //Copy handle from userspace
    n = copy_from_user(&usr_handle, cmd->handle, sizeof(struct pinner_handle));
    if (n != 0) {
        printk(KERN_ALERT "pinner: flush: could not copy handle from userspace\n");
        return -EAGAIN;
    }
    
    //Ensure that the user's handle matches the correct user_magic. We want to
    //make it very difficult for buggy (or malicious) user code to accidentally
    //unpin someone else's pinnings
    if (usr_handle.user_magic != info->magic) {
        printk(KERN_ALERT "pinner: incorrect user handle. No unpinning was performed\n");
        return -EINVAL;
    }
    
    //Search linearly through pinning structs for correct pin_magic
    //Note: who cares if this is inefficient? It's not like this function is
    //getting called millions of times per second
    for (cur = info->pinning_list.next; cur != &(info->pinning_list); cur = cur->next) {
        struct pinning *p = list_entry(cur, struct pinning, list);
        if (usr_handle.pin_magic == p->magic) {
            found = p;
            break;
        }
    }
    
    if (!found) {
        printk(KERN_ALERT "pinner: incorrect pin handle. No unpinning was performed\n");
        return -EINVAL;
    }
    
    //Perform the cache flushing (I hope this works!)
    //TODO: allow user to set direction
    if ((cmd->usr_buf_sz & 1) == 0) {
        printk(KERN_INFO "pinner: performing dma_sync_sg_for_cpu");
        dma_sync_sg_for_cpu(pinner_miscdev.this_device, found->sglist, found->num_sg_ents, DMA_BIDIRECTIONAL);
    }
    if ((cmd->usr_buf_sz & 0b10) == 0) {
        printk(KERN_INFO "pinner: performing dma_sync_sg_for_device");
        dma_sync_sg_for_device(pinner_miscdev.this_device, found->sglist, found->num_sg_ents, DMA_BIDIRECTIONAL);
    }
    return 0;
}

static int pinner_do_unpin(struct pinner_cmd *cmd, struct proc_info *info) {
    struct list_head *cur; //For iterating
    struct pinner_handle usr_handle;
    int n;
    struct pinning *found = NULL;
    
    //Copy handle from userspace
    n = copy_from_user(&usr_handle, cmd->handle, sizeof(struct pinner_handle));
    if (n != 0) {
        printk(KERN_ALERT "pinner: could not copy handle from userspace\n");
        return -EAGAIN;
    }
    
    //Ensure that the user's handle matches the correct user_magic. We want to
    //make it very difficult for buggy (or malicious) user code to accidentally
    //unpin someone else's pinnings
    if (usr_handle.user_magic != info->magic) {
        printk(KERN_ALERT "pinner: incorrect user handle. No unpinning was performed\n");
        return -EINVAL;
    }
    
    //Search linearly through pinning structs for correct pin_magic
    //Note: who cares if this is inefficient? It's not like this function is
    //getting called millions of times per second
    for (cur = info->pinning_list.next; cur != &(info->pinning_list); cur = cur->next) {
        struct pinning *p = list_entry(cur, struct pinning, list);
        if (usr_handle.pin_magic == p->magic) {
            found = p;
            break;
        }
    }
    
    if (!found) {
        printk(KERN_ALERT "pinner: incorrect pin handle. No unpinning was performed\n");
        return -EINVAL;
    }
    
    //Delete the pinning
    pinner_free_pinning(found);
    
    return 0;
}

static int pinner_open (struct inode *inode, struct file *filp) {
    struct proc_info *info = NULL;
    
	//Allocate and insert a new proc_info. Values should be initialized to zero
    info = kzalloc(sizeof(struct proc_info), GFP_KERNEL);
    if (!info) {
        printk(KERN_ALERT "Could not open pinner driver\n");
        return -ENOMEM;
    }
    
    //Initialize list of pinnings
    INIT_LIST_HEAD(&(info->pinning_list));
    
    //Initialize the magic
    get_random_bytes(&(info->magic), sizeof(info->magic));
    
    //Add to head of list
    mutex_lock(&users_mutex); //Need to watch out for race conditions
    list_add(&(info->list), &users);
    mutex_unlock(&users_mutex);
    
    //Keep link to this struct in filp->private_data
	filp->private_data = info;
    
    printk(KERN_ALERT "Succesfully opened pinner driver\n");
	return 0; //SUCCESS
}

static int pinner_release (struct inode *inode, struct file *filp) {
    struct proc_info *info = filp->private_data;
    
	//Clean up this proc_info struct
    pinner_free_proc_info(info);
    
    printk(KERN_ALERT "Closed pinner driver\n");
	return 0;
}

//Write function. Handles commands from userspace
static ssize_t pinner_write (struct file *filp, char const __user *buf, size_t sz, loff_t *off) {
    int rc;
    struct pinner_cmd cmd;
    struct proc_info *info = filp->private_data;
    
    if (sz != sizeof(struct pinner_cmd)) {
        printk(KERN_ALERT "pinner: bad command struct size [%lu], should be [%lu]\n", sz, sizeof(struct pinner_cmd));
        return -EINVAL;
    }
    
    rc = copy_from_user(&cmd, buf, sizeof(struct pinner_cmd));
    if (rc != 0) {
        printk(KERN_ALERT "pinner: could not copy command struct from userspace. Still need to copy [%d] bytes out of %lu\n", rc, sizeof(struct pinner_cmd));
        return -EAGAIN;
    }
    
    switch(cmd.cmd) {
        case PINNER_PIN:
            return pinner_do_pin(&cmd, info);
            break;
        case PINNER_UNPIN:
            return pinner_do_unpin(&cmd, info);
            break;
        case PINNER_FLUSH: {
            return pinner_do_flush(&cmd, info);
            break;
        }
        default:
            printk(KERN_ALERT "pinner: unrecognized command code [%u]\n", cmd.cmd);
            return -ENOSYS;
    }
	
	return 0;
}


//Structs for registering with misc devices
static struct file_operations pinner_fops = {
	.open = pinner_open,
	.write = pinner_write,
	.release = pinner_release
};

static struct miscdevice pinner_miscdev = { 
	.minor = MISC_DYNAMIC_MINOR, 
	.name = "pinner",
	.fops = &pinner_fops,
	.mode = 0666
};

static int registered = 0;

static int __init pinner_init(void) { 
    int rc;
    
    //Now that everything is safely initialized, make the driver available:
	rc = misc_register(&pinner_miscdev);
	if (rc < 0) {
		printk(KERN_ALERT "Could not register pinner module\n");
	} else {
		printk(KERN_ALERT "pinner module inserted\n"); 
		registered = 1;
	}
    
	return rc; //Propagate error code
} 

static void pinner_exit(void) { 
    //Remove all pinnings and free all proc_infos	
	if (registered) misc_deregister(&pinner_miscdev);
	
    //Probably don't need to lock mutex, since driver has been unregistered
    //mutex_lock(&users_mutex);
    while (!list_empty(&users)) {
        printk(KERN_ALERT "Warning: pinner exit function is freeing things that should have already been freed...\n");
        pinner_free_proc_info(list_entry(users.next, struct proc_info, list));
    }
    //mutex_unlock(&users_mutex);
    
	printk(KERN_ALERT "pinner module removed\n"); 
} 

MODULE_LICENSE("Dual BSD/GPL"); 

module_init(pinner_init); 
module_exit(pinner_exit);
