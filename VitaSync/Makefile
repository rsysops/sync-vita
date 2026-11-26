TARGET	= VitaSync
OBJS	= src/main.o src/audio.o src/speaker.o
LIBS	= -lSceSysmemForDriver_stub -lSceThreadmgrForDriver_stub \
	-lSceCpuForDriver_stub -lSceUdcdForDriver_stub \
	-lSceDisplayForDriver_stub -lSceIftuForDriver_stub \
	-lSceCtrlForDriver_stub -ltaihenForKernel_stub -lSceTouchForDriver_stub \
	-lSceAppMgrForDriver_stub -lSceKernelSuspendForDriver_stub \
	-lSceSysclibForDriver_stub -ltaihenModuleUtils_stub \
	-lSceDmacmgrForDriver_stub
# SceSysclibForDriver must always be linked: it gives memset, memmove and
# the division helpers from the kernel. Without it they come from vitasdk's
# libc, which imports them from the user-mode SceLibKernel, and a kernel
# plugin with that import doesn't work on the Vita.
ifeq ($(DEBUG), 1)
	OBJS	+= debug/log.o
	CFLAGS	+= -DDEBUG -Idebug
	LIBS	+= -lSceIofilemgrForDriver_stub
endif

# Test only: logs every USB start/stop/activate/deactivate call, with how
# long it took, to ur0:data/VitaSync_probe.txt (see USB_PROBE in main.c).
ifeq ($(PROBE), 1)
	CFLAGS	+= -DUSB_PROBE
	LIBS	+= -lSceIofilemgrForDriver_stub
endif

ifeq ($(DISPLAY_OFF_OLED), 1)
	CFLAGS	+= -DDISPLAY_OFF_OLED
	LIBS	+= -lSceOledForDriver_stub
endif

ifeq ($(DISPLAY_OFF_LCD), 1)
	CFLAGS	+= -DDISPLAY_OFF_LCD
	LIBS	+= -lSceLcdForDriver_stub
endif

PREFIX	= arm-vita-eabi
CC	= $(PREFIX)-gcc
CFLAGS	+= -Wl,-q -Wall -O2 -nostartfiles -mcpu=cortex-a9 -mthumb-interwork -Iinclude -ItaiHEN
DEPS	= $(OBJS:.o=.d)

all: $(TARGET).skprx

%.skprx: %.velf
	vita-make-fself -c $< $@

%.velf: %.elf
	vita-elf-create -e $(TARGET).yml $< $@

$(TARGET).elf: $(OBJS)
	$(CC) $(CFLAGS) $^ -o $@ $(LDFLAGS) $(LIBS)

%.o: %.c
	$(CC) $(CFLAGS) -MMD -MP -c $< -o $@

.PHONY: clean send

clean:
	@rm -rf $(TARGET).skprx $(TARGET).velf $(TARGET).elf $(OBJS) $(DEPS)

send: $(TARGET).skprx
	curl -T $(TARGET).skprx ftp://$(PSVITAIP):1337/ux0:/data/tai/kplugin.skprx
	@echo "Sent."

launch: send
	echo "load_skprx ux0:/data/tai/kplugin.skprx" | nc $(PSVITAIP) 1338
	@echo "Launched!"

taisend: $(TARGET).skprx
	curl -T $(TARGET).skprx ftp://$(PSVITAIP):1337/ux0:/tai/$(TARGET).skprx
	@echo "Sent."

-include $(DEPS)
