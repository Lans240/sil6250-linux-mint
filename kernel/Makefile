# sil6250 - out-of-tree build
#
#   make            build sil6250.ko against the running kernel
#   make clean      remove build artifacts
#   sudo make install / modules_install
#
obj-m += sil6250.o

KDIR ?= /lib/modules/$(shell uname -r)/build
PWD  := $(shell pwd)

# Out-of-tree modules must be built with the same toolchain as the kernel.
# Auto-detect from the target kernel's build config (clang -> LLVM=1, gcc ->
# unset). Override explicitly on the command line if needed (e.g. make LLVM=1).
ifeq ($(origin LLVM),undefined)
  ifeq ($(shell grep -sq '^CONFIG_CC_IS_CLANG=y' $(KDIR)/.config && echo clang),clang)
    LLVM := 1
    export LLVM
  endif
endif

all:
	$(MAKE) -C $(KDIR) M=$(PWD) modules

clean:
	$(MAKE) -C $(KDIR) M=$(PWD) clean

modules_install install:
	$(MAKE) -C $(KDIR) M=$(PWD) modules_install

.PHONY: all clean modules_install install
