#
# Makefile for a Video Disk Recorder plugin
#

# The official name of this plugin.
# This name will be used in the '-P...' option of VDR to load the plugin.
# By default the main source file also carries this name.

PLUGIN = softhddevice-drm-gles

### Configuration (edit this for your needs)

# Source documentation:
DOXYGEN  ?= /usr/bin/doxygen
DOXYFILE  = doxygen/Doxyfile

# Use OpenGL/ES for OSD? Disable autodetection by setting GLES=1 or GLES=0 with make command
GLES ?= $(shell pkg-config --exists glesv2 egl gbm && echo 1)
# enable this to write the OSD as png into /tmp (only GLES mode)
PNG ?= 0
# enable this to mark the corners of the rectangles on the OSD
GRID ?= 0

CONFIG :=
#CONFIG += -DMEDIA_DEBUG	# enable mediaplayer dumps
#CONFIG += -DALSA_DEBUG		# enable alsa logs
#CONFIG += -DFFMPEG_DEBUG	# enable ffmpeg logs

ifeq ($(GLES),1)
CONFIG += -DUSE_GLES			# build with OpenGL/ES support
ifeq ($(PNG),1)
CONFIG += -DWRITE_PNG			# enable writing OSD to png file
endif
ifeq ($(GRID),1)
CONFIG += -DGRIDPOINTS			# mark gridpoints
endif
endif

### The version number of this plugin (taken from the main source file):

VERSION = $(shell grep 'static const char \*const VERSION *=' $(PLUGIN).cpp | awk '{ print $$7 }' | sed -e 's/[";]//g')

### The Git revision:
# On a tag with clean tree: empty string
# On a tag with dirty tree: show hash with -dirty (prefixed with dash)
# Not on a tag: show hash (with -dirty if dirty, prefixed with dash)
GIT_DESCRIBE ?= $(shell git describe --exact-match HEAD 2>/dev/null >/dev/null && git diff --quiet && git diff --cached --quiet && echo "" || (HASH=$$(git describe --always --dirty --exclude "*" 2>/dev/null || echo "unknown"); echo "-$$HASH"))

### The directory environment:

# Use package data if installed...otherwise assume we're under the VDR source directory:
PKGCFG = $(if $(VDRDIR),$(shell pkg-config --variable=$(1) $(VDRDIR)/vdr.pc),$(shell PKG_CONFIG_PATH="$$PKG_CONFIG_PATH:../../.." pkg-config --variable=$(1) vdr))
LIBDIR = $(call PKGCFG,libdir)
LOCDIR = $(call PKGCFG,locdir)
PLGCFG = $(call PKGCFG,plgcfg)
#
TMPDIR ?= /tmp

### The compiler options:

export CFLAGS	= $(call PKGCFG,cflags)
export CXXFLAGS = $(call PKGCFG,cxxflags)

ifeq ($(CFLAGS),)
$(warning CFLAGS not set)
endif
ifeq ($(CXXFLAGS),)
$(warning CXXFLAGS not set)
endif

### The version number of VDR's plugin API:

APIVERSION = $(call PKGCFG,apiversion)

### Allow user defined options to overwrite defaults:

-include $(PLGCFG)

### The name of the distribution archive:

ARCHIVE = $(PLUGIN)-$(VERSION)
PACKAGE = vdr-$(ARCHIVE)

### The name of the shared object file:

SOFILE = libvdr-$(PLUGIN).so

### softhddevice config

_CFLAGS += $(shell pkg-config --cflags alsa libavcodec libavfilter libdrm)
LIBS += $(shell pkg-config --libs alsa libavcodec libavfilter libdrm)
ifeq ($(GLES),1)
### Check for openglosd dependencies
ifneq ($(shell pkg-config --exists glesv2 && echo 1),1)
$(error ERROR: Missing openglosd dependency: gles2)
endif
ifneq ($(shell pkg-config --exists egl && echo 1),1)
$(error ERROR: Missing openglosd dependency: egl)
endif
ifneq ($(shell pkg-config --exists gbm && echo 1),1)
$(error ERROR: Missing openglosd dependency: gbm)
endif
ifneq ($(shell pkg-config --exists freetype2 && echo 1),1)
$(error ERROR: Missing openglosd dependency: freetype2)
endif
ifneq ($(shell pkg-config --exists glm && echo 1),1)
ifneq ($(shell test -d /usr/include/glm && echo 1),1)
$(error ERROR: Missing openglosd dependency: glm)
endif
endif
_CFLAGS += $(shell pkg-config --cflags gbm glesv2 egl)
LIBS += $(shell pkg-config --libs gbm glesv2 egl)
_CFLAGS += $(shell pkg-config --cflags freetype2)
LIBS += $(shell pkg-config --libs freetype2)
ifeq ($(PNG),1)
_CFLAGS += $(shell pkg-config --cflags libpng)
LIBS += $(shell pkg-config --libs libpng)
endif
endif

### Includes and Defines (add further entries here):

INCLUDES +=

DEFINES += -DPLUGIN_NAME_I18N='"$(PLUGIN)"' -D_GNU_SOURCE $(CONFIG) -DGIT_DESCRIBE='"$(GIT_DESCRIBE)"'

### Make it standard

override CXXFLAGS += $(_CFLAGS) $(DEFINES) $(INCLUDES) \
    -g -ggdb3 -W -Wall -Wextra -Winit-self -Werror=overloaded-virtual
override CFLAGS	  += $(_CFLAGS) $(DEFINES) $(INCLUDES) \
    -g -ggdb3 -W -Wall -Wextra -Winit-self

### The object files (add further files here):

OBJS = $(PLUGIN).o \
	audio.o \
	buf2rgb.o \
	codec_audio.o \
	codec_video.o \
	config.o \
	drmbuffer.o \
	drmdevice.o \
	drmplane.o \
	filllevel.o \
	grab.o \
	h264parser.o \
	logger.o \
	mediaplayer.o \
	pes.o \
	pidcontroller.o \
	pipreceiver.o \
	ringbuffer.o \
	softhddevice.o \
	softhdmenu.o \
	softhdsetupmenu.o \
	softhdosd.o \
	threads.o \
	videorender.o \
	videostream.o

ifeq ($(GLES),1)
OBJS += openglosd.o
endif

SRCS = $(wildcard $(OBJS:.o=.c)) $(PLUGIN).cpp

### The main target:

all: $(SOFILE) i18n

### Dependencies:

MAKEDEP = $(CXX) -MM -MG
DEPFILE = .dependencies
$(DEPFILE): Makefile
	@$(MAKEDEP) $(CXXFLAGS) $(SRCS) > $@

-include $(DEPFILE)

### Internationalization (I18N):

PODIR	  = po
I18Npo	  = $(wildcard $(PODIR)/*.po)
I18Nmo	  = $(addsuffix .mo, $(foreach file, $(I18Npo), $(basename $(file))))
I18Nmsgs  = $(addprefix $(DESTDIR)$(LOCDIR)/, $(addsuffix /LC_MESSAGES/vdr-$(PLUGIN).mo, $(notdir $(foreach file, $(I18Npo), $(basename $(file))))))
I18Npot	  = $(PODIR)/$(PLUGIN).pot

%.mo: %.po
	msgfmt -c -o $@ $<

$(I18Npot): $(SRCS)
	xgettext -C -cTRANSLATORS --no-wrap --no-location -k -ktr -ktrNOOP \
	-k_ -k_N --package-name=vdr-$(PLUGIN) --package-version=$(VERSION) \
	--msgid-bugs-address='<see README>' -o $@ `find *.cpp`

%.po: $(I18Npot)
	msgmerge -U --no-wrap --no-location --backup=none -q -N $@ $<
	@touch $@

$(I18Nmsgs): $(DESTDIR)$(LOCDIR)/%/LC_MESSAGES/vdr-$(PLUGIN).mo: $(PODIR)/%.mo
	install -D -m644 $< $@

.PHONY: i18n
i18n: $(I18Nmo) $(I18Npot)

install-i18n: $(I18Nmsgs)

### Targets:

$(OBJS): Makefile

# Force rebuild of main plugin object to update GIT_DESCRIBE
.PHONY: force
$(PLUGIN).o: force

$(SOFILE): $(OBJS)
	$(CXX) $(CXXFLAGS) $(LDFLAGS) -shared $(OBJS) $(LIBS) -o $@

install-lib: $(SOFILE)
	install -D $^ $(DESTDIR)$(LIBDIR)/$^.$(APIVERSION)

install: install-lib install-i18n

dist: $(I18Npo) clean
	@-rm -rf $(TMPDIR)/$(ARCHIVE)
	@mkdir $(TMPDIR)/$(ARCHIVE)
	@cp -a * $(TMPDIR)/$(ARCHIVE)
	@tar czf $(PACKAGE).tgz -C $(TMPDIR) $(ARCHIVE)
	@-rm -rf $(TMPDIR)/$(ARCHIVE)
	@echo Distribution package created as $(PACKAGE).tgz

clean:
	@-rm -f $(PODIR)/*.mo $(PODIR)/*.pot
	@-rm -f $(DEPFILE) *.o *.so *.tgz core* *~
	@-rm -rf srcdoc
	@$(MAKE) -C tests clean 2>/dev/null || true

# Unit tests:
.PHONY: test
test:
	@$(MAKE) -C tests test

# Source documentation:
srcdoc:
	@cat $(DOXYFILE) > $(DOXYFILE).tmp
	@echo PROJECT_NUMBER = $(VERSION) >> $(DOXYFILE).tmp
	@chmod +x $(DOXYFILE).filter
	$(DOXYGEN) $(DOXYFILE).tmp
	@rm $(DOXYFILE).tmp

## Private Targets:

HDRS=	$(wildcard *.h)

indent:
	for i in $(SRCS) $(HDRS); do \
		indent $$i; \
		unexpand -a $$i | sed -e s/constconst/const/ > $$i.up; \
		mv $$i.up $$i; \
	done
