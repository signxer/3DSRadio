#-------------------------------------------------------------------------------
# 3DSRadio - Internet Radio Player for Nintendo 3DS
# Based on devkitPro 3DS template
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
# devkitARM configuration
#-------------------------------------------------------------------------------
ifneq ($(strip $(DEVKITARM)),)
  include $(DEVKITARM)/3ds_rules

  TARGET        := $(TITLE)
  BUILD         := build
  SOURCES       := source
  INCLUDES      := include
  DATA          := data
  ROMFS         := romfs

  ARCH          := -march=armv6k -mtune=mpcore -mfloat-abi=hard -mtp=soft

  CFLAGS        := -g -Wall -Wextra -Wshadow -O2 -std=gnu11 \
                   $(ARCH) -mword-relocations -ffunction-sections \
                   -D__3DS__ -DHAVE_3DS \
                   $(foreach dir,$(INCLUDES),-I$(CURDIR)/$(dir))

  LDFLAGS       := -specs=3dsx.specs $(ARCH) -Wl,-Map,$(TARGET).map

  LIBS          := -lcitro2d -lcitro3d -lcurl -lmbedtls -lmbedx509 \
                   -lmbedcrypto -lpng -ljpeg -lz -lctru -lm

  # Source files
  OFILES_SRC    := $(patsubst %.c,%.o,$(wildcard $(SOURCES)/*.c))
  OFILES_SRC    += $(patsubst %.cpp,%.o,$(wildcard $(SOURCES)/*.cpp))
  OFILES_BIN    := $(patsubst %.bin,%.o,$(wildcard $(DATA)/*.bin))

  OFILES        := $(OFILES_SRC:$(SOURCES)/%.o=%.o)
  OFILES        += $(OFILES_BIN:$(DATA)/%.o=%.o)

  VPATH         := $(CURDIR)/$(SOURCES):$(CURDIR)/$(DATA)

  .PHONY: all clean cia

#-------------------------------------------------------------------------------
# Build targets
#-------------------------------------------------------------------------------
all: $(BUILD)

$(BUILD):
	@[ -d $@ ] || mkdir -p $@
	@$(MAKE) $(TARGET).3dsx

$(TARGET).3dsx: $(TARGET).elf
	@echo "Built $(TARGET).3dsx"

$(TARGET).elf: $(OFILES)
	@echo "Linking..."
	@$(CC) -specs=3dsx.specs $(ARCH) -Wl,-Map,$(TARGET).map \
		-o $@ $(foreach f,$(OFILES),$(BUILD)/$f) $(LIBS)

%.o: %.c
	@echo "Compiling $<..."
	@$(CC) -MMD -MP -MF $(BUILD)/$*.d $(CFLAGS) -c $< -o $(BUILD)/$@

%.o: %.cpp
	@echo "Compiling $<..."
	@$(CXX) -MMD -MP -MF $(BUILD)/$*.d $(CXXFLAGS) -c $< -o $(BUILD)/$@

%.o: %.s
	@echo "Assembling $<..."
	@$(CC) -x assembler-with-cpp $(ASFLAGS) -c $< -o $(BUILD)/$@

%.o: %.bin
	@echo "Embedding $<..."
	@$(BIN2S) $< > $(BUILD)/$*.s
	@$(CC) -MMD -MP -MF $(BUILD)/$*.d $(ASFLAGS) -c $(BUILD)/$*.s -o $(BUILD)/$@

#-------------------------------------------------------------------------------
# CIA package
#-------------------------------------------------------------------------------
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

#-------------------------------------------------------------------------------
# Clean
#-------------------------------------------------------------------------------
clean:
	@rm -rf $(BUILD) $(TARGET).3dsx $(TARGET).elf $(TARGET).smdh \
		$(TARGET).cia *.map romfs/banner.bnr romfs/icon.icn
	@echo "Cleaned"

-include $(BUILD)/*.d

else
  $(error DEVKITARM is not set. Install devkitPro or use Docker.)
endif
