#include <linux/kernel.h> //print functions
#include <linux/init.h> //for __init
#include <linux/device.h> //For struct device
#include <linux/module.h> //for module init and exit macros
#include <linux/mutex.h> //For mutexes
#include <linux/sysfs.h> //For struct kobj_atrtibute and sysfs_create_file
#include <linux/kobject.h> //For kobjects
#include <linux/interrupt.h> //IRQF_SHARED
#include <linux/uio_driver.h> //UIO stuff
#include <asm/io.h> //For ioremap
#include <linux/irqdomain.h> //For irq_find_host
#include <linux/of.h> //For device tree struct types
#include <linux/irq.h> //For irq_desc struct and irq_to_desc

#define REGS_SPAN 0x1000

//Virtual address to AXI DMA register space
static void *axidma_virt = NULL;

//Make sure only one user at a time, and disable sysfs files when in use
static int in_use = 0;
static DEFINE_MUTEX(in_use_mutex);

//sysfs-controlled variables
static int axidma_enable = 0;
static unsigned long axidma_phys_base = 0xA0000000;
static int axidma_irq_line = 0;


//AXI DMA interrupt handler
static irqreturn_t axidma_irq_handler(int irq, struct uio_info *dev) {
    //The interrupt flags are in buts 12, 11, and 10 of the status registers
    uint32_t *MM2S_DMASR = (uint32_t*) (axidma_virt + 0x04);
    uint32_t *S2MM_DMASR = (uint32_t*) (axidma_virt + 0x34);
    
    if (!axidma_virt) {
        printk(KERN_ALERT "REALLY BAD ERROR: AXI DMA interrupt triggered, but no way to access its registers!\n");
        return IRQ_NONE;
    }
    
    if (((*MM2S_DMASR >> 12) & 0b111) || ((*S2MM_DMASR >> 12) & 0b111)) {
        printk(KERN_INFO "MM2S_DMASR: %x, S2MM_DMASR: %x\n", *MM2S_DMASR, *S2MM_DMASR);
        *MM2S_DMASR = 0xFFFFFFFF;
        *S2MM_DMASR = 0xFFFFFFFF;
        return IRQ_HANDLED; 
    }
    
    return IRQ_NONE; 
}

//UIO driver file operations
int axidma_open (struct uio_info *info, struct inode *inode) {
    mutex_lock(&in_use_mutex);
    if (in_use) {
        mutex_unlock(&in_use_mutex);
        printk(KERN_ERR "AXI DMA module in use\n");
        return -EBUSY;
    }
    
    in_use = 1;
    mutex_unlock(&in_use_mutex);
    
    return 0;
}

int axidma_release (struct uio_info *info, struct inode *inode) {
    mutex_lock(&in_use_mutex);
    in_use = 0; //Don't bother checking if it was already 0
    mutex_unlock(&in_use_mutex);
    
    return 0;
}

//sysfs show and store functions
static ssize_t enable_show  (struct kobject *kobj, struct kobj_attribute *attr, char *buf) {
    return sprintf(buf, "%d\n", axidma_enable);
}

//enable_store is special, since it also takes care of registering with UIO
//forward-declare the uio_info and device structs
static struct device axidma_device;
static struct uio_info axidma_uio_info;
static ssize_t enable_store (struct kobject *kobj, struct kobj_attribute *attr, 
                            const char *buf, size_t count)
{
    int tmp = 0;
    
    //Check if the driver is in use
    mutex_lock(&in_use_mutex);
    if (in_use) {
        printk(KERN_ERR "Error! Cannot modify parameters while AXI DMA is in use\n");
        mutex_unlock(&in_use_mutex);
        return count;
    }
    mutex_unlock(&in_use_mutex);
    
    if(sscanf(buf, "%d", &tmp) != 1) {
        printk(KERN_ERR "WARNING: could not parse enable from user input!\n");
        return count;
    }
    
    if (tmp) {
        if (!axidma_enable) {
            int rc;
            //register UIO
            //First get the interrupt number
            struct device_node *dn;
            struct irq_domain *dom;
            struct irq_fwspec dummy_fwspec = {
                .param_count = 3,
                .param = {0, 89 + axidma_irq_line, 4} 
            };
            int virq;
            
            //Find the Linux irq number
            dn = of_find_node_by_name(NULL, "interrupt-controller");
            if (!dn) {
                printk(KERN_ERR "Could not find device node for \"interrupt-controller\"\n");
                axidma_enable = 0;
                return count;
            }
            dom = irq_find_host(dn);
            if (!dom) {
                printk(KERN_ERR "Could not find irq domain\n");
                axidma_enable = 0;
                return count;
            }
            
            dummy_fwspec.fwnode = dom->fwnode;
            virq = irq_create_fwspec_mapping(&dummy_fwspec);
            
            axidma_uio_info.irq = virq;
            axidma_uio_info.mem[0].addr = axidma_phys_base;
            
            rc = uio_register_device(&axidma_device, &axidma_uio_info);
            if (rc < 0) {
                printk(KERN_ERR "Could not register UIO device for some reason\n");
                axidma_enable = 0;
                return count;
            }
            
            axidma_virt = ioremap_nocache(axidma_phys_base, 0x1000);
            if (axidma_virt == NULL) {
                printk(KERN_ERR "Could not remap device memory\n");
                axidma_enable = 0;
                return count;
            }
        } 
        axidma_enable = 1;
    } else {
        if (axidma_enable) {
            uio_unregister_device(&axidma_uio_info);
            iounmap(axidma_virt);
            axidma_virt = NULL;
        }
        axidma_enable = 0;
    }
    return count; 
}


static ssize_t phys_base_show  (struct kobject *kobj, struct kobj_attribute *attr, char *buf) {
    return sprintf(buf, "%lx\n", axidma_phys_base);
}

static ssize_t phys_base_store (struct kobject *kobj, struct kobj_attribute *attr, 
                            const char *buf, size_t count)
{
    unsigned long tmp;
    //Check if the driver is in use
    mutex_lock(&in_use_mutex);
    if (in_use) {
        printk(KERN_ERR "axidma: Cannot modify parameters while AXI DMA is in use\n");
        mutex_unlock(&in_use_mutex);
        return count;
    }
    mutex_unlock(&in_use_mutex);
    
    if(sscanf(buf, "%lx", &tmp) != 1) {
        printk(KERN_ERR "axidma: could not parse base_phys from user input\n");
        return count;
    }
    
    if (tmp < 0xA0000000 || tmp > 0xB0000000) {
        printk(KERN_ERR "axidma: address out of range\n");
        return count;
    }
    
    axidma_phys_base = tmp;
    return count; 
}

static ssize_t irq_line_show  (struct kobject *kobj, struct kobj_attribute *attr, char *buf) {
    return sprintf(buf, "%d\n", axidma_irq_line);
}

static ssize_t irq_line_store (struct kobject *kobj, struct kobj_attribute *attr, 
                            const char *buf, size_t count)
{
    int tmp;
    //Check if the driver is in use
    mutex_lock(&in_use_mutex);
    if (in_use) {
        printk(KERN_ERR "axidma: Cannot modify parameters while AXI DMA is in use\n");
        mutex_unlock(&in_use_mutex);
        return count;
    }
    mutex_unlock(&in_use_mutex);
    
    if (sscanf(buf, "%d", &tmp) != 1) {
        printk(KERN_ERR "axidma: could not parse irq_line from user input!\n");
        return count;
    }
    
    if (tmp < 0 || tmp > 7) {
        printk(KERN_ERR "axidma: irq number out of range\n");
        return count;
    }
    
    axidma_irq_line = tmp;
    return count; 
}

//Structs needed for sysfs
static struct kobject *axidma_kobject;
static struct kobj_attribute axidma_enable_attr;
static struct kobj_attribute axidma_phys_base_attr;
static struct kobj_attribute axidma_irq_line_attr;

//structs needed to register with uio
//Just to satisfy the struct device
static void dummy_release(struct device *dev) {}

static struct device axidma_device = {
    .init_name = "axidma",
    .release = dummy_release
};
static struct uio_info axidma_uio_info = {
    .name = "axidma",
    .version = "1.0",
    //.irq = TBD, 
    .irq_flags = IRQF_SHARED,
    .handler = axidma_irq_handler,
    //Unlike tutorial doc, I take a shortcut and don't separately defien the uio_mem struct
    .mem = {
        {
            .name = "axidma_regs", 
            .memtype = UIO_MEM_PHYS, 
            /*.addr = TBD,*/ 
            .size = REGS_SPAN
        }
    }
};

static int __init axidma_init(void) {
    int rc = 0;
    
    //Register the struct device. May as well do it here
    //TODO: maybe use the sysfs struct device functions?    
    rc = device_register(&axidma_device);
    if (rc < 0) {
        printk(KERN_ERR "Could not register device with kernel\n");
        return rc;
    }    
    
    axidma_kobject = kobject_create_and_add("axidma", NULL); //TODO: error-checking
    
    axidma_enable_attr.attr.name = "enable";
    axidma_enable_attr.attr.mode = 0666;
    axidma_enable_attr.show = enable_show;
    axidma_enable_attr.store = enable_store;
    
    axidma_phys_base_attr.attr.name = "phys_base";
    axidma_phys_base_attr.attr.mode = 0666;
    axidma_phys_base_attr.show = phys_base_show;
    axidma_phys_base_attr.store = phys_base_store;
    
    axidma_irq_line_attr.attr.name = "irq_line";
    axidma_irq_line_attr.attr.mode = 0666;
    axidma_irq_line_attr.show = irq_line_show;
    axidma_irq_line_attr.store = irq_line_store;
    
    rc = sysfs_create_file(axidma_kobject, &(axidma_enable_attr.attr));
    if (rc) {
        printk(KERN_ERR "Could not create sysfs files");
        device_unregister(&axidma_device);
        kobject_put(axidma_kobject);
        return rc;
    }
    
    rc = sysfs_create_file(axidma_kobject, &(axidma_phys_base_attr.attr));
    if (rc) {
        printk(KERN_ERR "Could not create sysfs files");
        device_unregister(&axidma_device);
        kobject_put(axidma_kobject);
        return rc;
    }
    
    rc = sysfs_create_file(axidma_kobject, &(axidma_irq_line_attr.attr));
    if (rc) {
        printk(KERN_ERR "Could not create sysfs files");
        device_unregister(&axidma_device);
        kobject_put(axidma_kobject);
        return rc;
    }
    
    return 0;
}

void axidma_exit(void) {    
    //Make sure we really clean everything up
    if (axidma_enable) {
        printk(KERN_ERR "Warning: axidma module is trying to clean up loose ends...\n");
        uio_unregister_device(&axidma_uio_info);
        iounmap(axidma_virt);
    }
    
    //Clear out sysfs files
    kobject_put(axidma_kobject);
    
    //Unregister device
    device_unregister(&axidma_device);
}

MODULE_LICENSE("Dual BSD/GPL"); 
module_init(axidma_init);
module_exit(axidma_exit);
