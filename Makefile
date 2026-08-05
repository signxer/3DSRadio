#-------------------------------------------------------------------------------
# 3DSRadio - Internet Radio Player for Nintendo 3DS
# Standard devkitPro 3DS build
#-------------------------------------------------------------------------------
TITLE          := 3DSRadio
DESCRIPTION    := Internet Radio Player
AUTHOR         := 3DSRadio
VERSION_MAJOR  := 1
VERSION_MINOR  := 0
VERSION_MICRO  := 0
PRODUCT_CODE   := CTR-RADIO
UNIQUE_ID      := 0x7F500

#-------------------------------------------------------------------------------
# devkitPro configuration
#-------------------------------------------------------------------------------
ifneq ($(strip $(DEVKITARM)),)

  # Standard variables for 3ds_rules
  TARGET        := $(TITLE)
  BUILD         := build
  SOURCES       := source
  INCLUDES      := include
  DATA          := data
  ROMFS         := romfs

  # Include devkitPro's standard 3DS build rules
  include $(DEVKITARM)/3ds_rules

  # Architecture flags
  ARCH          := -march=armv6k -mtune=mpcore -mfloat-abi=hard -mtp=soft

  # Compiler flags
  CFLAGS        := -g -Wall -Wextra -Wshadow -O2 -std=gnu11 \
                   $(ARCH) -mword-relocations -ffunction-sections \
                   -D__3DS__ -DHAVE_3DS

  CFLAGS        += $(INCLUDE)  # Set by 3ds_rules

  # Linker flags
  LDFLAGS       := -specs=3dsx.specs $(ARCH) -Wl,-Map,$(TARGET).map

  # Libraries
  LIBS          := -lcitro2d -lcitro3d -lcurl -lmbedtls -lmbedx509 \
                   -lmbedcrypto -lpng -ljpeg -lz -lctru -lm

  # Override the default link command to add our libraries
  $(TARGET).elf: LIBS := $(LIBS)

  .PHONY: all clean cia

#-------------------------------------------------------------------------------
# Targets
#-------------------------------------------------------------------------------
all: $(TARGET).3dsx

# CIA package
cia: $(TARGET).3dsx
	@echo "Building CIA..."
	@bannertool makebanner -i romfs/banner.png -a romfs/banner.wav -o romfs/banner.bnr 2>/dev/null; \
	bannertool makesmdh -s "$(TITLE)" -l "$(DESCRIPTION)" -p "$(AUTHOR)" \
		-i romfs/icon.png -o romfs/icon.icn 2>/dev/null; \
	makerom -f cia -o $(TARGET).cia -rsf romfs/template.rsf \
		-target t -exefslogo -elf $(TARGET).elf \
		-icon romfs/icon.icn -banner romfs/banner.bnr \
		-DAPP_TITLE="$(TITLE)" -DAPP_PRODUCT_CODE="$(PRODUCT_CODE)" \
		-DAPP_UNIQUE_ID="$(UNIQUE_ID)" 2>/dev/null; \
	echo "CIA built: $(TARGET).cia"

clean:
	@rm -rf $(BUILD) $(TARGET).3dsx $(TARGET).elf $(TARGET).smdh \
		$(TARGET).cia *.map romfs/banner.bnr romfs/icon.icn
	@echo "Cleaned"

else
  $(error DEVKITARM is not set. Install devkitPro or use Docker.)
endif
