I made my own AXI DMA driver using UIO. The full details are in 
mpsoc_programming/drivers/uio_with_sysfs. This driver, along with the pinner 
driver, make it possible to use the AXI DMA in a much easier way. At some 
point, I will put together a userspace library to simplify things a little bit.

Its features include:
    - Ability to reconfigure driver with sysfs (instead of with device tree)
    - Allow userspace to directly access AXI DMA registers
    - Allow usrespace to wait for interrupts

========
OVERVIEW
========

First, we'll talk about the sysfs files used to configure the AXI DMA driver. 
Then, we'll talk about configuring the AXI DMA core. Unfortunately, the AXI DMA 
product guide doesn't explain some really important aspects of using it (stuff 
involving TLAST, for example), so we'll cover those. Finally, I'll briefly 
explain what's going on when you use the driver from userspace.


====================================
INSERTING AND CONFIGURING THE DRIVER
====================================

Inserting the driver is the same as anything else: 

    sudo insmod axidma.ko

However, you may wonder why no new devices were created in /dev. Once the 
driver is inserted, it exposes the following files in sysfs, which must be 
configured before the driver creates a device file:

/sys/axidma/enable:
    Write "1" or "0" to enable/disable the driver. It is safest to disable it 
    before reprogramming a bitstream, but you can be lazy about this. The more 
    important reason to disable it is if you want to change the other 
    paraemeters (explained below).
    
    Important: only enable the driver once the other parameters have been 
    correctly set.

/sys/axidma/irq_line:
    Write an integer to this file to tell the driver which interrupt line you 
    are using. Specifically, this should be the bit number on the pl_ps_irq 
    input on the Zynq block that the AXI DMA is wired into. The driver will 
    take care of converting this to the right number.

/sys/axidma/phys_base:
    Write an address (IN HEX!) to this file to tell the driver where your AXI 
    DMA is. As usual, this is the address you selected in the Address Editor.

After configuring the files, a device file will be created. However, I 
unfortunately haven't figured out how to change the name of the device file. To 
find out what it is, go to /sys/devices/axidma/uio. There should be a folder in 
here called "uioN", where N is a number. The device file is at /dev/uioN. On my 
MPSoC, this is usually /dev/uio1


====================================
CONFIGURING THE AXI DMA AND ZYNQ IPS
====================================

This is something I've only been able to discover after a lot of googling, and 
a lot of trial and error. 

First of all, I use the S_AXI_HPC0_FPD slave port at 128 bits wide. I use an 
AXI interconnect between the AXI DMA and the Zynq. All of the AXI DMA's AXI 
Masters go through this interconnect so that they can share the PS's AXI Slave. 
Furthermore, I wire a constant 0b1011 into ARCACHE and AWCACHE (you'll have to 
expand the AXI Slaves on the interconnect to do this)

I also enabled unaligned reads. This isn't necessary, but it's a lot more 
convenient.

Basically, you can use any other congfiguration you want, but don't enable 
"Micro DMA". This tries to aggressivley minimize area, but it usually doesn't 
work with the drivers, since it requires everything to be perfectly aligned and 
to have perfect sizes etc.


==========================================
AXI DMA, TLAST, AND SCATTER-GATHER ENTRIES
==========================================

This gave me conniptions. The AXI DMA is supremely annoying to work with, since 
it has very inconvenient behaviour when it comes to TLAST signals, and the 
product guide just doesn't explain anything about it.

You need to know three things:

    1. If the AXI DMA is told to fill a buffer of size X, but the TLAST signal 
       is asserted before X bytes are transferred, it will stop prematurely and 
       raise an interrupt. In this case, it will NOT keep filling further 
       transfers in the incomplete buffer; it will fetch the next SG entry with 
       its SOF bit set and start copying there.      
       
    2. If the AXI DMA is told to fill a buffer of size X, but the TLAST signal 
       is asserted after the X bytes are transferred, then the AXI DMA will be 
       FROZEN FOREVER. Or, at least, that's what it does for me...
       
    3. A transfer can span multiple scatter-gather buffers, provided the 
       scatter-gather entries have correct SOF and EOF bits set.

Sorry, what was that about the scatter-gather entries?

Well, suppose I had two buffers: one at address 0x1000, and one at address 
0x3000. Suppose each of them has size 0x50. If my scatter-gather entries look 
like this:


 +--------------------------------------------------------------------------+
 |        |xxxx|                 |xxxx|                                     |
 +--------------------------------------------------------------------------+
           ^                      ^
      0x1000                 0x3000
           ^                      ^
           |                      |
           |                      +--+
           |                         |
  +--------|+      +------> +--------|-+
  |buffer--+|      |        |buffer+-+ |
  |len=0x50 |      |        |len=0x50  |
  |next+-----------+        |next      |
  |SOF      |               |EOF       |
  +---------+               +----------+
  (note: these SG entries _are_ stored in 
   memory somewhere; I just drew them here
   for clarity)

The first entry is marked as Start-Of-Frame, and points to the buffer at 
0x1000. The second entry is marked as End-Of-Frame, and points to the buffer at 
0x3000. In this case the AXI DMA will try to read 0x100 bytes; the first 0x50 
will go in the buffer at 0x1000, and the rest in the buffer at 0x3000.

If, on the other hand, we had: 

 +--------------------------------------------------------------------------+
 |        |xxxx|                 |xxxx|                                     |
 +--------------------------------------------------------------------------+
           ^                      ^
      0x1000                 0x3000
           ^                      ^
           |                      |
           |                      +--+
           |                         |
  +--------|+      +------> +--------|-+
  |buffer--+|      |        |buffer+-+ |
  |len=0x50 |      |        |len=0x50  |
  |next+-----------+        |next      |
  |SOF, EOF |               |SOF, EOF  |
  +---------+               +----------+

then the AXI DMA would try to read one buffer of size 0x50, and then another of 
size 0x50. Say the stream input to the AXI DMA had TLAST set after only 0x20 
bytes. Then, 0x20 bytes would be written to the first buffer, and the AXI DMA 
will write any further transfers into the second buffer.

One last thing: the AXI DMA core will actually edit the SG entries to tell you 
how many bytes it wrote. This is one of the few things that is well explained 
in the product guide, so I'll refer you there.


================
USING THE DRIVER
================

This is the same as using any other UIO-based driver. Once you've found the 
right filename, open() the file:

    #include <fcntl.h>
    #include <unistd.h>
    
    
    int fd = open("/dev/uioN", O_RDWR);
    ...
    close(fd);

You can get access to the AXI DMA's registers with an mmap() command:

    #include <sys/mman.h>
    
    char *axidma_regs = mmap(0, 0x1000, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    ...
    munmap(regs, 0x1000);

And you can wait for an interrupt by read()ing an unsigned from it:

    unsigned pending;
    read(fd, &pending, sizeof(pending));

The number returned is the number of interrupts that were caught since the last
time you called the read() function.

Pro tip: you really should check every single return value from a system call, 
and print an error message to the user so that they know what's going on. For 
example,

    #include <fcntl.h>
    #include <string.h>
    
    int fd = open("/dev/uioN", O_RDWR);
    if (fd == -1) {
        perror("Could not open /dev/uioN");
        return -1;
    }

The perror function will print out the string you gave it, followed by the 
error string generated from the system. For example, if you didn't have 
permission to access the /dev/uioN file, the message would show up as

    Could not open /dev/uioN: Permission denied

Believe me it's worth the effort!
