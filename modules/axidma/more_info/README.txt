This file shows how you can combine UIO with sysfs attribute files to make a 
much more flexible driver. Specifically, I wanted to make an easier AXI DMA 
driver, so this is perfect opportunity to explain how I did it.

The problem with the AXI Timer example driver is that the base address is 
hardcoded. As FPGA developers, our base addresses are always in flux. Woudln't 
it be nice to change them on the fly?

Well, sysfs attribute files were made specifically for that reason. The files 
in this folder are organized as follows:

sysfs_attribute_files.txt:
    Explains how to do sysfs attribute files in general. 

uio_refresher.txt
    The uio tutorial goes more into depth, but this file just catches us up on 
    the important function calls.

interrupt_numbers_refresher.txt
    Unfortunately for us, MPSoC interrupts are a horribly difficult mess to 
    deal with. This file summarizes what you need to know. See the longer 
    linux_kernel_programming/interupt_numbers.txt for full details.

putting_it_together.txt:
    Does what it says on the tin.


Finally, the bits of source are all combined into one compilable C file 
(axidma.c). See linux_kernel_programming/compiling_mpsoc_modules.txt for 
instructions on compiling this file.

A userspace example is given in userspace_example.c, which relies on pinner.h. 
It also uses the pinner module I wrote, which I have not yet documented fully. 
If you go to the UofT-HPRC github, you'll find it in the mpsoc_drivers repo. 

    The pinner driver shows how you can pin userspace memory. It's also quite 
    interesting, and I'll probably write docs for it over the Christmas break.
