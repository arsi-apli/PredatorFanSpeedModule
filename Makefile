KERNELRELEASE=$(shell uname -r)
obj-m	:= acerhdf.o
KDIR	:= /lib/modules/$(KERNELRELEASE)/build
INST	:= $(DESTDIR)/lib/modules/$(KERNELRELEASE)/updates
PWD	:= $(shell pwd)

default:
	$(MAKE) -C $(KDIR)  SUBDIRS=$(PWD) modules
install: default
	[ -d $(INST) ] || mkdir $(INST)
	cp acerhdf.ko $(INST)/
	depmod -a
uninstall:
	rm -f $(INST)/acerhdf.ko
	depmod -a

tempclean:
	rm -Rf *.o .tmp_versions .io.* *.mod.c Module.symvers

clean:
	rm -Rf *.ko *.o .tmp_versions .acerhdf.* *.mod.c Module.symvers modules.order Module.markers
