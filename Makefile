# USB Hub Diagnostics - devkitPPC / libogc (Wii)
# Build: make        Clean: make clean        Install: make install SDROOT=/path/to/sd
ifeq ($(strip $(DEVKITPPC)),)
$(error "Set DEVKITPPC (e.g. export DEVKITPPC=/opt/devkitpro/devkitPPC)")
endif

include $(DEVKITPPC)/wii_rules

TARGET   := boot
BUILD    := build
SOURCES  := source
INCLUDES :=

CFLAGS   := -g -O2 -Wall $(MACHDEP) $(INCLUDE)
LDFLAGS  := -g $(MACHDEP) -Wl,-Map,$(notdir $@).map
LIBS     := -lwiiuse -lbte -lfat -logc -lm
LIBDIRS  :=

ifneq ($(BUILD),$(notdir $(CURDIR)))

export OUTPUT   := $(CURDIR)/$(TARGET)
export VPATH    := $(foreach dir,$(SOURCES),$(CURDIR)/$(dir))
export DEPSDIR  := $(CURDIR)/$(BUILD)
CFILES          := $(foreach dir,$(SOURCES),$(notdir $(wildcard $(dir)/*.c)))
export OFILES   := $(CFILES:.c=.o)
export LD       := $(CC)
export INCLUDE  := $(foreach dir,$(INCLUDES),-iquote $(CURDIR)/$(dir)) \
                   $(foreach dir,$(LIBDIRS),-I$(dir)/include) \
                   -I$(CURDIR)/$(BUILD) -I$(LIBOGC_INC)
export LIBPATHS := $(foreach dir,$(LIBDIRS),-L$(dir)/lib) -L$(LIBOGC_LIB)

.PHONY: $(BUILD) clean install
$(BUILD):
	@[ -d $@ ] || mkdir -p $@
	@$(MAKE) --no-print-directory -C $(BUILD) -f $(CURDIR)/Makefile

clean:
	@rm -fr $(BUILD) $(OUTPUT).elf $(OUTPUT).dol

# copies the HBC-ready folder into SDROOT/apps/
install: $(BUILD)
	@test -n "$(SDROOT)" || (echo "usage: make install SDROOT=/path/to/sd"; exit 1)
	@mkdir -p $(SDROOT)/apps/usb-hub-diagnostics
	@cp $(OUTPUT).dol apps/usb-hub-diagnostics/boot.dol
	@cp apps/usb-hub-diagnostics/* $(SDROOT)/apps/usb-hub-diagnostics/

else
DEPENDS := $(OFILES:.o=.d)
$(OUTPUT).dol: $(OUTPUT).elf
$(OUTPUT).elf: $(OFILES)
-include $(DEPENDS)
endif
