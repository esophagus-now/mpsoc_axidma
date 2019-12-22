#include <linux/kernel.h> //print functions
#include <linux/init.h> //for __init, see code
#include <linux/module.h> //for module init and exit macros
#include <linux/interrupt.h> //IRQF_SHARED
#include <linux/uio_driver.h> //UIO stuff
#include <linux/device.h> //For struct device
#include <asm/io.h> //For ioremap
#include <linux/irqdomain.h> //For irq_find_host
#include <linux/of.h> //For device tree struct types
#include <linux/irq.h> //For irq_desc struct and irq_to_desc

#define T0INT_SHIFT (8)
#define T0INT_MASK (1 << T0INT_SHIFT)
#define AXITIMER_BASE (0xA0000000)

//Hang onto kernel virtual address for AXI timer regs
static volatile uint32_t *virt;

static irqreturn_t axitimer_irq(int irq, struct uio_info *dev) {
    //Check if this interrupt was for us
    uint32_t status = *virt;
    
    printk(KERN_INFO "axitimer_irq called\n");
    
    if (status & T0INT_MASK) {
        printk(KERN_INFO "axitimer interrupt handled\n");
        //This interrupt was for us. Clear the T0INT bit
        virt[0] |= T0INT_MASK;
        return IRQ_HANDLED; //Signal to kernel that interrupt was handled
    }
    
    return IRQ_NONE; //Interrupt wasn't for us; tell Linux to try other handlers
}

static struct uio_info axi_timer = {
    .name = "mpsoc_axitimer",
    .version = "1.0",
    //.irq = 89, //See "smarter" irq number finding code in init function
    .irq_flags = IRQF_SHARED,
    .handler = axitimer_irq,
    .mem = {
        {.name = "axi_timer_regs", .memtype = UIO_MEM_PHYS, .addr = AXITIMER_BASE, .size = 16}
    }
};

static struct uio_mem axi_timer_regs = {
    .name = "axi_timer_regs",
    .memtype = UIO_MEM_PHYS,
    .addr = AXITIMER_BASE,
    .size = 0x1000
};

static void dummy_release(struct device *dev) {
    printk(KERN_INFO "There, it's released, OK???\n");
    return;
}

//UIO forces us to make a struct device. Anyway, I guess it's worth doing, since
//it will make nice sysfs entries...
static struct device axitimer_dev = {
    .init_name = "axitimer",
    .release = dummy_release
};

static int __init our_init(void) {     
    int rc = 0;
    struct device_node *dn;
    struct irq_domain *dom;
    struct irq_desc *irqd;
    struct irq_data *irq_data;
    int virq;
    //Perform IO remapping
    virt = ioremap_nocache(AXITIMER_BASE, 4);
    if (virt == NULL) {
        printk(KERN_ERR "Could not remap device memory\n");
        rc = -ENOMEM;
        goto axitimer_err_remap;
    }
    
    *virt = 0; //Disable the timer
    
    //Create the struct device for the axi timer
    rc = device_register(&axitimer_dev);
    if (rc < 0) {
        printk(KERN_ERR "Could not register device with kernel\n");
        goto axitimer_err_device_register;
    }    
    
    //Find the Linux irq number
    dn = of_find_node_by_name(NULL, "interrupt-controller");
    if (!dn) {
        printk(KERN_ERR "Could not find device node for \"interrupt-controller\"\n");
        goto axitimer_err_device_register;
    }
    dom = irq_find_host(dn);
    if (!dom) {
        printk(KERN_ERR "Could not find irq domain\n");
        goto axitimer_err_device_register;
    }
    printk(KERN_INFO "Using domain with name %s\n", dom->name);
    
    virq = irq_find_mapping(dom, 121);
    if (!virq) {        
        struct irq_fwspec our_fwspec = {
            .fwnode = dom->fwnode,
            .param_count = 3,
            .param = {0, 89, 4}
        };
        
        printk(KERN_INFO "Allocating descriptor for hwirq 121...\n");
        
        virq = irq_create_fwspec_mapping(&our_fwspec);
    }
    if (virq <= 0) {
        printk(KERN_ERR "Could not allocate irq\n");
        if (virq == 0) {
            rc = -ECANCELED;
        } else {
            rc = virq;
        }
        
        goto axitimer_err_device_register;
    }
    printk(KERN_INFO "Using linux irq #%d\n", virq);
    
    irqd = irq_to_desc(virq);
    printk(KERN_INFO "irqd->irq_data.chip = %p\n", irqd->irq_data.chip);
    if (irqd->irq_data.chip == NULL || irqd->irq_data.chip == &dummy_irq_chip) {
        printk(KERN_ERR "No chip has been associated!!\n");
    } else {
        printk(KERN_INFO "Chip name is %s\n", irqd->irq_data.chip->name);
    }
    irq_data = irq_domain_get_irq_data(dom, virq);
    if (irq_data == NULL) {
        printk(KERN_ERR "Error: hwirq %d does not belong to the domain\n", virq);
    } else {
        printk(KERN_INFO "The chip for hwirq %d has name %s\n", virq, irq_data->chip->name);
    }
    
    //Finish setting up uio_info struct
    axi_timer.irq = virq;
    axi_timer.mem[0] = axi_timer_regs;
    //Register with UIO
    rc = uio_register_device(&axitimer_dev, &axi_timer);
    if (rc < 0) {
        printk(KERN_ERR "Could not register device with UIO\n");
        device_unregister(&axitimer_dev);
        iounmap(virt);
        return rc; //Propagate error code
    }
    
    printk(KERN_INFO "AXI timer driver inserted!\n"); 
	return 0;
    
    axitimer_err_device_register:
    iounmap(virt);
    axitimer_err_remap:
    return rc;
} 

static void our_exit(void) { 
    //Unregister with UIO
    uio_unregister_device(&axi_timer);
    
    //Unregister device
    device_unregister(&axitimer_dev);
    
    //Unmap IO memory
    iounmap(virt);
    
	printk(KERN_INFO "AXI timer driver removed!\n"); 
} 

MODULE_LICENSE("Dual BSD/GPL"); 

module_init(our_init); 
module_exit(our_exit);
