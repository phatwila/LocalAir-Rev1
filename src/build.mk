## Device OS user-part build hook for LocalAir.
##
## This file is included by the Particle local gcc-arm build when APPDIR
## points at this project. Keep the source list and include path explicit so
## the app sources actually compile under the local Device OS checkout.

LOCALAIR_USER_SRC_PATH ?= $(if $(SOURCE_PATH),$(SOURCE_PATH)$(USRSRC),$(APPDIR)/src)

CPPSRC += $(call target_files,$(USRSRC),*.cpp)
CSRC += $(call target_files,$(USRSRC),*.c)
INCLUDE_DIRS += $(LOCALAIR_USER_SRC_PATH)
INCLUDE_DIRS += $(APPDIR)/target

EXTRA_USER_CFLAGS ?= $(EXTRA_CFLAG) $(EXTRA_CFLAGS)

ifneq ($(strip $(EXTRA_USER_CFLAGS)),)
CFLAGS += $(EXTRA_USER_CFLAGS)
CXXFLAGS += $(EXTRA_USER_CFLAGS)
endif
