# Kbuild part: used when invoked from the kernel build system.
ifneq ($(KERNELRELEASE),)
obj-m := edu_drv.o
else

KDIR ?= /lib/modules/$(shell uname -r)/build
TEST_CFLAGS ?= -std=c11 -O2 -Wall -Wextra -Werror

.PHONY: all module test clean
all: module test

module:
	$(MAKE) -C $(KDIR) M=$(CURDIR) modules

test: test_edu

test_edu: test/test_edu.c edu_ioctl.h
	$(CC) $(TEST_CFLAGS) -I$(CURDIR) -o $@ test/test_edu.c

clean:
	$(MAKE) -C $(KDIR) M=$(CURDIR) clean
	$(RM) test_edu
endif
