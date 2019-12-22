#include <stdio.h>
#include <fcntl.h> //open
#include <sys/mman.h> //mmap
#include <unistd.h> //close
#include <stdint.h>


typedef struct {
    unsigned    MD      :1; //When 1, timer in generate mode (o.w., capture) 
    unsigned    UD      :1; //When 1, timer counts down (otherwise, up) 
    unsigned    GEN     :1; //When 1, enable external generate signal 
    unsigned    CAP     :1; //When 1, enables external capture trigger  
    unsigned    ARH     :1; //When 1, timer auto-reloads. Otherwise, it holds 
    unsigned    LOAD    :1; //When 1, timer loaded from TLR register. Remember to clear it in order to let the timer run! 
    unsigned    ENI     :1; //Enable interrupt  
    unsigned    EN      :1; //Enable timer 
    unsigned    INT     :1; //1 if interrupt has occured (0 otherwise)
    unsigned    PWM     :1; //Enable PWM mode 
    unsigned    ENALL   :1; //Enable all timers
    unsigned    CASC    :1; //Enable Cascading mode
    unsigned            :20; //Pad up to 32 bits
} TCSR_bits;

typedef struct {
    TCSR_bits TCSR;
    uint32_t TLR;
    uint32_t TCR;
} axitimer_regs;

TCSR_bits disable = {
    .MD      = 0,
    .UD      = 0,
    .GEN     = 0,
    .CAP     = 0,
    .ARH     = 0,
    .LOAD    = 0,
    .ENI     = 0,
    .EN      = 0,
    .INT     = 1, //Writing one will clear any leftover interrupts
    .PWM     = 0,
    .ENALL   = 0,
    .CASC    = 0,
};

TCSR_bits load = {
    .MD      = 0,
    .UD      = 0,
    .GEN     = 0,
    .CAP     = 0,
    .ARH     = 0,
    .LOAD    = 1,
    .ENI     = 0,
    .EN      = 0,
    .INT     = 0,
    .PWM     = 0,
    .ENALL   = 0,
    .CASC    = 0,
};

TCSR_bits one_int_per_sec = {
    .MD      = 0,
    .UD      = 1,
    .GEN     = 0,
    .CAP     = 0,
    .ARH     = 1,
    .LOAD    = 0,
    .ENI     = 1,
    .EN      = 1,
    .INT     = 0, 
    .PWM     = 0,
    .ENALL   = 1,
    .CASC    = 0,
};

uint32_t period_1s = 100000000; //Clock is at 100MHz

#define MAP_SIZE (0x1000)

int main(int argc, char **argv) {
    int ret = 0;
    int fd = -1;
    volatile axitimer_regs *base = MAP_FAILED;
    
    printf("%x\n", *((uint32_t*)(&one_int_per_sec)));
    
    if (argc != 2) {
        puts("Please give the device file as an argument to this program");
        return 0;
    }
    
    fd = open(argv[1], O_RDWR);
    if (fd == -1) {
        char line[80];
        snprintf(line, 79, "Could not open %s\n", argv[1]);
        perror(line);
        ret = -1;
        goto cleanup;
    }
    
    base = (volatile axitimer_regs *) mmap(0, MAP_SIZE, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    if (base == MAP_FAILED) {
        perror("Could not mmap device memory");
        ret = -1;
        goto cleanup;
    }
    
    base->TCSR = disable;
    base->TLR = period_1s;
    base->TCSR = load;
    base->TCSR = one_int_per_sec;
    
    unsigned pending;
    
    int i;
    for (i = 0; i < 10; i++) {
        puts("Waiting for interrupt");
        fflush(stdout);
        read(fd, &pending, sizeof(pending));
        printf("Current time value is %u\n", base->TCR);
    }
        
    
    
    cleanup:
    if (base != MAP_FAILED) {
        base->TCSR = disable;
        munmap((void*)base, MAP_SIZE);
    }
    if (fd != -1) close(fd);
    
    return ret;
}
