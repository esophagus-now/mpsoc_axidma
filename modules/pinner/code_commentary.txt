
Here is how I made this file: 
	I copy pasted the pinner.c file in here, indented it, then added 
	unindented text to explain what is going on. I also added some background 
	information to make it easier to understand.

==========
BACKGROUND
==========

Pinning pages
-------------

A page is said to be "pinned" if it is 1) present in RAM, and 2) guaranteed to 
stay in RAM at the same physical address. For example, this means that it will 
never be swapped out.

This whole module is centered around a single function call: 
get_user_pages_fast. This function takes a userspace virtual address (and a 
range) and does two things:

    - Forces all of its pages to be brought into RAM (in case some were swapped 
      out)
    
    - Prevents any further memory management on these pages. For example, the 
      kernel can sometimes move pages to make physically contiguous space, and 
      of course, will occasionally swap out a page. This behaviour is prevented 
      until the pages are "released"

The rest of the code is essentially bookkeeping, with some extra functionality 
for returning physical address information to the user.

Cache coherency
---------------

One more thing: I was having problems with cache coherency. The solution was to 
use the kernel's "DMA Mapping API". Contrary to what the name implies, I am not 
using it for mapping. Specifically, I use the dma_map_sg function to disable 
caching on the pinned buffer (and dma_unmap_sg to undo that). Also, for extra 
safety, I use dma_sync_sg_for_cpu and dma_sync_sg_for_device, which definitely 
perform a cache flush.

Apparently, you really shouldn't use DMA_BIDIRECTIONAL if you can avoid it, 
since it degrades performance or something. At some point I might get arounf to 
letting the user pass in the desired direction.

==========
COMMENTARY
==========


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
    #include "pinner.h" //Custom data types and defines shared with userspace
    #include "pinner_private.h" //Private custom data types and macros

This is a linked list of proc_info structs. Each process that is using the 
pinner driver gets its own proc_info. We need to be careful about race 
conditions on global variables, so there's a mutex here for guarding accesses

    static DEFINE_MUTEX(users_mutex);
    static LIST_HEAD(users);

    //Forward-declare miscdev struct
    static struct miscdevice pinner_miscdev;


Helper function to release an array of struct page pointers back to the memory 
management system. As per the comment in the source code, it only gets called 
in error-handling code. Specififically, this happens if we need to abort after 
succesfully calling get_user_pages_fast but before anything else.

    //This function only used in error-handling code
    static void put_page_list(struct page **p, int num_pages) {
        int i;
        for (i = 0; i < num_pages; i++) {
            put_page(p[i]);
        }
    }

Travels through an array of struct scatterlist and retrieves the struct page 
pointers. Then put_page is called on each one. This is what actually releases 
the pages back to the memory management code in the kernel, and is separate 
from undoing dma_map_sg

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

A pinning struct basically just hangs on to the struct scatterlist array 
representing the pinned buffer. Every proc_info contains a linked list of these 
pinning structs. 

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

This is a helper function to iterate through all the pinnings in a proc_info 
and free them all

    static void pinner_free_pinnings(struct proc_info *info) {
        //printk(KERN_ALERT "Entered pinner_free_pinnings\n");
        //Iterate through the list of pinnings inside this proc_info struct
        //and free them all
        while (!list_empty(&(info->pinning_list))) {
            struct pinning *p = list_entry(info->pinning_list.next, struct pinning, list);
            pinner_free_pinning(p);
        }
    }

Finally, a function to free a proc_info struct and anything it contains

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

This function takes the information from a pinning and translates it into 
information that userspace can use. It then uses copy_to_user to actually write 
this info back to the user (in the locations the user gave in their cmd 
struct).

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

This function converts an array of struct page pointers into an array of 
scatter-gather (SG) list entries. However, these are not SG entries that any 
DMA would use, they're just a generalized in-kernel representation, and each 
one is stored in a struct scatterlist.

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

This is probably the ugliest part of the whole code. Lots of tricky edge cases. 
What we're doign here is making sure that all the offsets and lengths in the SG 
list are correct. We also save the physical address in the struct scatterlist, 
since this is later used by pinner_send_physlist to inform the user about their 
buffer's physical addresses.
        
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


This is the most complicated function of the module. It has a lot of jobs:
 - Make sure the user didn't ask for something invalid
 
 - Actually call get_user_pages_fast
 
 - Maintain the proc_info and pinning structs to allow us to free the pinned
   buffer later on
   
 - Calling pinner_alloc_and_fill_sglist (as part of filling in the struct
   pinning)
   
 - Calling pinner_send_physlist to return physical address info to the user
 
 - Also writing a handle back to user space. This handle can be used to refer
   to a pinning by the user (when they want to force a cache flush or unpin)


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

Here it is! The all-important call to get_user_pages_fast!

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

I've already mentioned this, but I'm not 100% sure what dma_map_sg does. I 
tried looking at the kernel source, but I can only get so far before it becomes 
architecture-specific assembly code. All I know is that 
drivers/infiniband/core/umem.c does this when try to pin user memory.
        
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

I'm fairly certain this next function isn't really needed, sicne dma_map_sg 
should take care of disabling caching. However, it was easy enough to put in 
and lets me be really sure the cache was flushed.

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
 
These are the functions that do the dirty work. I wasn't sure which one to use, 
so as you can see, I put both! 
        
        //Perform the cache flushing (I hope this works!)
        //TODO: allow user to set direction
        dma_sync_sg_for_cpu(pinner_miscdev.this_device, found->sglist, found->num_sg_ents, DMA_BIDIRECTIONAL);
        dma_sync_sg_for_device(pinner_miscdev.this_device, found->sglist, found->num_sg_ents, DMA_BIDIRECTIONAL);
        return 0;
    }


This function extracts the handle from userspace (using copy_from_user) and 
uses the handle to locate the specific pinning struct to remove. It then 
removes it.

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

When a process first opens the device file, we'll allocate a new proc_info,
initialize it, and add it to the global users list.
Also note that we get some random bytes to put in the proc_info struct. This is
for safety against users that play around with their handles

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

This gets called when the user close()s the device file. We simply free the
associated proc_info struct

    static int pinner_release (struct inode *inode, struct file *filp) {
        struct proc_info *info = filp->private_data;
        
        //Clean up this proc_info struct
        pinner_free_proc_info(info);
        
        printk(KERN_ALERT "Closed pinner driver\n");
        return 0;
    }


This function dispatches control based on the command struct the user sent in

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


The rest of this is bookkeeping...

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

...well, there's one thing here: in case a process really screws up, this 
module exit function makes sure there are no leftover pinnings.

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

This MODULE_LICENSE is actually quite important. The kernel will not let the
driver use certain functions if you don't have it

    MODULE_LICENSE("Dual BSD/GPL"); 

    module_init(pinner_init); 
    module_exit(pinner_exit);
