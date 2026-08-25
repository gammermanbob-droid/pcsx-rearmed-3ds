#---------------------------------------------------------------------------------
.SUFFIXES:
#---------------------------------------------------------------------------------

ifeq ($(strip $(DEVKITARM)),)
$(error "Please set DEVKITARM in your environment. export DEVKITARM=<path to>devkitARM")
endif

TOPDIR ?= $(CURDIR)
include $(DEVKITARM)/3ds_rules

#---------------------------------------------------------------------------------
# pcsx_rearmed_libretro_ctr.a is the vendored PS1 core (see
# external/pcsx_rearmed, a git submodule), built via its own upstream
# Makefile.libretro platform=ctr target -- that target already carries
# the exact ARM11/3DS-tuned flags this app's own ARCH line below
# mirrors, so the two object codes are ABI-compatible. Build it first
# if it isn't already there, rather than duplicating its (large,
# upstream-maintained) build rules here.
#---------------------------------------------------------------------------------
PCSX_DIR	:=	$(TOPDIR)/external/pcsx_rearmed
PCSX_CORE	:=	$(PCSX_DIR)/pcsx_rearmed_libretro_ctr.a

#---------------------------------------------------------------------------------
TARGET		:=	pcsx_rearmed_3ds
BUILD		:=	build
SOURCES		:=	source
DATA		:=	data
INCLUDES	:=	include
ROMFS		:=	romfs

#---------------------------------------------------------------------------------
ARCH	:=	-march=armv6k -mtune=mpcore -mfloat-abi=hard -mtp=soft

CFLAGS	:=	-g -Wall -O2 -mword-relocations \
			-ffunction-sections \
			$(ARCH) -DARM11 -D_3DS -D__3DS__

CFLAGS	+=	$(INCLUDE) -D__3DS__ \
			-I$(PCSX_DIR)/deps/libretro-common/include

CXXFLAGS	:= $(CFLAGS) -fno-rtti -fno-exceptions -std=gnu++11

ASFLAGS	:=	-g $(ARCH)
LDFLAGS	=	-specs=3dsx.specs -g $(ARCH) -Wl,-Map,$(notdir $*.map)

# $(PCSX_CORE) must come before -lm: GNU ld only pulls symbols out of a
# static archive it's already scanned, so libm has to be scanned *after*
# the core object that actually references libm functions (roundf etc.)
# or those references are left undefined.
LIBS	:=	$(PCSX_CORE) -lcitro2d -lcitro3d -lctru -lm

#---------------------------------------------------------------------------------
LIBDIRS	:= $(CTRULIB)

#---------------------------------------------------------------------------------
ifneq ($(BUILD),$(notdir $(CURDIR)))
#---------------------------------------------------------------------------------

export OUTPUT	:=	$(CURDIR)/$(TARGET)
export TOPDIR	:=	$(CURDIR)

export VPATH	:=	$(foreach dir,$(SOURCES),$(CURDIR)/$(dir)) \
			$(foreach dir,$(DATA),$(CURDIR)/$(dir))

export DEPSDIR	:=	$(CURDIR)/$(BUILD)

CFILES		:=	$(foreach dir,$(SOURCES),$(notdir $(wildcard $(dir)/*.c)))
CPPFILES	:=	$(foreach dir,$(SOURCES),$(notdir $(wildcard $(dir)/*.cpp)))
SFILES		:=	$(foreach dir,$(SOURCES),$(notdir $(wildcard $(dir)/*.s)))
BINFILES	:=	$(foreach dir,$(DATA),$(notdir $(wildcard $(dir)/*.*)))

ifeq ($(strip $(CPPFILES)),)
	export LD	:=	$(CC)
else
	export LD	:=	$(CXX)
endif

export OFILES_SOURCES 	:=	$(CPPFILES:.cpp=.o) $(CFILES:.c=.o) $(SFILES:.s=.o)
export OFILES_BIN	:=	$(addsuffix .o,$(BINFILES))
export OFILES := $(OFILES_BIN) $(OFILES_SOURCES)
export HFILES	:=	$(addsuffix .h,$(subst .,_,$(BINFILES)))

export INCLUDE	:=	$(foreach dir,$(INCLUDES),-I$(CURDIR)/$(dir)) \
			$(foreach dir,$(LIBDIRS),-I$(dir)/include) \
			-I$(CURDIR)/$(BUILD)

export LIBPATHS	:=	$(foreach dir,$(LIBDIRS),-L$(dir)/lib)

export _3DSXDEPS	:=	$(if $(NO_SMDH),,$(OUTPUT).smdh)

export APP_TITLE	:=	PCSX ReARMed 3DS
export APP_DESCRIPTION	:=	SweepDS Emu -- PS1 (PCSX ReARMed core)
export APP_AUTHOR	:=	SweepDS Emu Project

ifeq ($(strip $(ICON)),)
	icons := $(wildcard *.png)
	ifneq (,$(findstring $(TARGET).png,$(icons)))
		export APP_ICON := $(TOPDIR)/$(TARGET).png
	else
		ifneq (,$(findstring icon.png,$(icons)))
			export APP_ICON := $(TOPDIR)/icon.png
		endif
	endif
else
	export APP_ICON := $(TOPDIR)/$(ICON)
endif

ifeq ($(strip $(NO_SMDH)),)
	export _3DSXFLAGS += --smdh=$(CURDIR)/$(TARGET).smdh
endif

ifneq ($(ROMFS),)
	export _3DSXFLAGS += --romfs=$(CURDIR)/$(ROMFS)
endif

.PHONY: all clean core

all: core $(ROMFS)/ps1_logo.t3x $(ROMFS)/boot_chime.pcm $(ROMFS)/menu_music.pcm $(ROMFS)/intro_video.h264 $(BUILD) $(DEPSDIR)
	@$(MAKE) --no-print-directory -C $(BUILD) -f $(CURDIR)/Makefile

core: $(PCSX_CORE)

$(PCSX_CORE):
	@echo "building pcsx_rearmed core for ctr..."
	@$(MAKE) -C $(PCSX_DIR) -f Makefile.libretro platform=ctr -j$$(nproc 2>/dev/null || sysctl -n hw.ncpu)

# The ANALOG toggle's PS1-logo badge (see video.c's videoDrawAnalogToggle) --
# generated from gfx/ps1_logo.png via tex3ds rather than checked in as a
# binary .t3x, same reasoning as building the vendored core from source.
$(ROMFS)/ps1_logo.t3x: gfx/ps1_logo.png
	@mkdir -p $(ROMFS)
	tex3ds -o $@ $<

# Raw signed-16-bit PCM (see main.c's boot-chime playback and menu.c's
# looping browser music) -- pre-decoded on the build machine via ffmpeg
# (see audio_src/README) rather than embedding an MP3/AAC decoder in
# the app itself, since NDSP wants raw PCM anyway and neither clip
# needs re-encoding at runtime.
$(ROMFS)/boot_chime.pcm: audio_src/boot_chime.pcm
	@mkdir -p $(ROMFS)
	cp $< $@

$(ROMFS)/menu_music.pcm: audio_src/menu_music.pcm
	@mkdir -p $(ROMFS)
	cp $< $@

# New3DS-only hardware-decoded boot intro (see intro_video.c) -- a raw
# Annex-B H.264 elementary stream, pre-encoded via ffmpeg (see
# audio_src/README.md) at a resolution/bitrate the MVD hardware decoder
# can keep up with in real time.
$(ROMFS)/intro_video.h264: audio_src/intro_video.h264
	@mkdir -p $(ROMFS)
	cp $< $@

$(BUILD):
	@mkdir -p $@

ifneq ($(DEPSDIR),$(BUILD))
$(DEPSDIR):
	@mkdir -p $@
endif

clean:
	@echo clean ...
	@rm -fr $(BUILD) $(TARGET).3dsx $(OUTPUT).smdh $(TARGET).elf

distclean: clean
	@$(MAKE) -C $(PCSX_DIR) -f Makefile.libretro platform=ctr clean

#---------------------------------------------------------------------------------
else

$(OUTPUT).3dsx	:	$(OUTPUT).elf $(_3DSXDEPS)

$(OFILES_SOURCES) : $(HFILES)

$(OUTPUT).elf	:	$(OFILES)

%.bin.o	%_bin.h :	%.bin
	@echo $(notdir $<)
	@$(bin2o)

-include $(DEPSDIR)/*.d

#---------------------------------------------------------------------------------------
endif
#---------------------------------------------------------------------------------------
