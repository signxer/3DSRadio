#-------------------------------------------------------------------------------
# 3DSRadio - Internet Radio Player for Nintendo 3DS
# Self-contained build (does not rely on 3ds_rules for rules)
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
# Toolchain
#-------------------------------------------------------------------------------
ifneq ($(strip $(DEVKITARM)),)
  include $(DEVKITARM)/3ds_rules

  TARGET        := $(TITLE)
  BUILD         := build
  SOURCES       := source
  INCLUDES      := include
  DATA          := data
  ROMFS         := romfs

  # Toolchain (set explicitly)
  PREFIX        := $(DEVKITARM)/bin/arm-none-eabi-
  CC            := $(PREFIX)gcc
  CXX           := $(PREFIX)g++
  AS            := $(PREFIX)as
  LD            := $(PREFIX)gcc
  AR            := $(PREFIX)ar
  OBJCOPY       := $(PREFIX)objcopy

  ARCH          := -march=armv6k -mtune=mpcore -mfloat-abi=hard -mtp=soft

  CFLAGS        := -g -Wall -Wextra -Wshadow -O2 -std=gnu11 \
                   $(ARCH) -mword-relocations -ffunction-sections \
                   -D__3DS__ -DHAVE_3DS \
                   -I$(CURDIR)/$(INCLUDES) \
                   -I$(CTRULIB)/include \
                   -I$(PORTLIBS)/include

  CXXFLAGS      := $(CFLAGS) -fno-rtti -fno-exceptions
  ASFLAGS       := -g $(ARCH)

  LDFLAGS       := -specs=3dsx.specs $(ARCH) -Wl,-Map,$(TARGET).map

  LIBS          := -lcitro2d -lcitro3d -lcurl -lmbedtls -lmbedx509 \
                   -lmbedcrypto -lpng -ljpeg -lz -lctru -lm

  LIBDIRS       := $(CTRULIB) $(PORTLIBS)
  LDFLAGS       += $(foreach dir,$(LIBDIRS),-L$(dir)/lib)

  # Source auto-discovery
  OFILES        := $(patsubst $(SOURCES)/%.c,%.o,$(wildcard $(SOURCES)/*.c))
  OFILES        += $(patsubst $(SOURCES)/%.cpp,%.o,$(wildcard $(SOURCES)/*.cpp))

  VPATH         := $(CURDIR)/$(SOURCES):$(CURDIR)/$(DATA)

  .PHONY: all clean cia

#-------------------------------------------------------------------------------
# Build rules
#-------------------------------------------------------------------------------
all: $(BUILD) $(TARGET).3dsx

$(BUILD):
	@mkdir -p $@

$(TARGET).smdh:
	@python3 scripts/gen_smdh.py $@ "$(TITLE)" "$(DESCRIPTION)" "$(AUTHOR)"

$(TARGET).3dsx: $(TARGET).elf $(TARGET).smdh
	@3dsxtool $< $@ --romfs=$(CURDIR)/$(ROMFS) --smdh=$(TARGET).smdh
	@echo "  built $(TARGET).3dsx"

$(TARGET).elf: $(OFILES:%=$(BUILD)/%)
	@echo "  linking $(TARGET).elf"
	@$(LD) $(LDFLAGS) $^ $(LIBS) -o $@

$(BUILD)/%.o: %.c
	@echo "  CC $<"
	@$(CC) -MMD -MP -MF $(BUILD)/$*.d $(CFLAGS) -c $< -o $@

$(BUILD)/%.o: %.cpp
	@echo "  CXX $<"
	@$(CXX) -MMD -MP -MF $(BUILD)/$*.d $(CXXFLAGS) -c $< -o $@

#-------------------------------------------------------------------------------
# CIA package
#-------------------------------------------------------------------------------
cia: $(TARGET).3dsx
	@echo "  building CIA..."
	@bannertool makebanner -i romfs/banner.png -a romfs/banner.wav -o romfs/banner.bnr 2>/dev/null; \
	bannertool makesmdh -s "$(TITLE)" -l "$(DESCRIPTION)" -p "$(AUTHOR)" \
		-i romfs/icon.png -o romfs/icon.icn 2>/dev/null; \
	makerom -f cia -o $(TARGET).cia -rsf romfs/template.rsf \
		-target t -exefslogo -elf $(TARGET).elf \
		-icon romfs/icon.icn -banner romfs/banner.bnr \
		-DAPP_TITLE="$(TITLE)" -DAPP_PRODUCT_CODE="$(PRODUCT_CODE)" \
		-DAPP_UNIQUE_ID="$(UNIQUE_ID)" 2>/dev/null; \
	echo "  CIA built: $(TARGET).cia"

clean:
	@rm -rf $(BUILD) $(TARGET).3dsx $(TARGET).elf $(TARGET).smdh \
		$(TARGET).cia *.map romfs/banner.bnr romfs/icon.icn
	@echo "  cleaned"

-include $(BUILD)/*.d

else
  $(error DEVKITARM is not set. Install devkitPro or use Docker.)
endif
