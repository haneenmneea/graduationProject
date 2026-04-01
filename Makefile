CONTIKI_PROJECT = hello
CONTIKI = $(HOME)/iot-lab-contiki-ng/contiki-ng
ARCH_PATH = $(HOME)/iot-lab-contiki-ng/arch
all: $(CONTIKI_PROJECT)
include $(CONTIKI)/Makefile.include
