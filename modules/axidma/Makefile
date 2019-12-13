export ARCH:=arm64
export CROSS_COMPILE:=aarch64-linux-gnu-

CC=$(CROSS_COMPILE)gcc

obj-m += axidma.o

KDIR  := /home/mahkoe/research/stale/linux-xlnx
PWD		:= $(shell pwd)

default:
	${MAKE} -C ${KDIR} M=${PWD} modules

clean:
	${MAKE} -C ${KDIR} M=${PWD} clean
	
