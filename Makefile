# Kbuild Makefile for task_lister kernel module
#
# When invoked by the kernel build system (kbuild), the variable
# KERNELRELEASE is defined. In that context we declare our object target.
#
# When invoked directly by the user (make), KERNELRELEASE is empty and
# we re-invoke make pointing at the kernel's own build system, which
# in turn comes back here with KERNELRELEASE set.

# obj-m tells kbuild to build this as a loadable module (.ko).
# If it were obj-y the code would be compiled into the kernel image instead.
obj-m := task_lister.o

# Kernel build directory — matches the running kernel on Arch Linux.
# Change this if cross-compiling or targeting a different kernel version.
KDIR ?= /lib/modules/$(shell uname -r)/build

# Where to build (default: in-tree next to the source).
PWD := $(shell pwd)

# Default target: build the module.
all:
	$(MAKE) -C $(KDIR) M=$(PWD) modules

# Clean up all kbuild-generated artefacts.
clean:
	$(MAKE) -C $(KDIR) M=$(PWD) clean

# Install the module into the running kernel's module tree.
# Requires root; run:  sudo make install
install: all
	$(MAKE) -C $(KDIR) M=$(PWD) modules_install
	depmod -a

# Convenience targets — load and unload the module.
# 'make load'   → inserts the module
# 'make unload' → removes the module (safe even if not loaded)
load: all
	sudo insmod task_lister.ko

unload:
	sudo rmmod task_lister 2>/dev/null || true

# Show module info without loading it.
info: all
	modinfo task_lister.ko

.PHONY: all clean install load unload info
