# SPDX-License-Identifier: MIT
ifeq ($(strip $(DEVKITPRO)),)
$(error "Please set DEVKITPRO in your environment")
endif

TARGET := stream-conf
BUILD := build
PREFIX := $(DEVKITPRO)/devkitA64/bin/aarch64-none-elf-
include $(DEVKITPRO)/libounce/switch2_cpu.mk

ARCH := $(SWITCH2_CPU) -mtp=soft -fPIE
CFLAGS := $(ARCH) -std=gnu11 -O2 -Wall -Wextra -ffunction-sections -fdata-sections \
    -D__SWITCH__ -D__SWITCH2__ \
    -I$(DEVKITPRO)/portlibs/switch2/include -I$(DEVKITPRO)/libounce/include -Isource
LDFLAGS := $(ARCH) -specs=$(DEVKITPRO)/libounce/switch.specs -Wl,--gc-sections
LIBS := -L$(DEVKITPRO)/portlibs/switch2/lib -L$(DEVKITPRO)/libounce/lib -lkaihen -lounce

OBJS := $(BUILD)/main.o $(BUILD)/ipc.o

all: $(TARGET).nro

$(BUILD):
	mkdir -p $@

$(BUILD)/%.o: source/%.c source/config.h | $(BUILD)
	$(PREFIX)gcc $(CFLAGS) -c $< -o $@

$(BUILD)/$(TARGET).elf: $(OBJS)
	$(PREFIX)gcc $(LDFLAGS) $^ $(LIBS) -o $@

$(TARGET).nso: $(BUILD)/$(TARGET).elf
	$(DEVKITPRO)/tools/bin/elf2nro $< $@

$(TARGET).nro: $(BUILD)/$(TARGET).elf
	$(DEVKITPRO)/tools/bin/elf2nro $< $@

clean:
	rm -rf $(BUILD) $(TARGET).nro

.PHONY: all clean
