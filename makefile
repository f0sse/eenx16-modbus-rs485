
#
# Copied from the kernel tree (tools/scripts/Makefile.include)
#
#| "Makefiles suck: This macro sets a default value of $(2) for
#|  the variable named by $(1), unless the variable has been set
#|  by environment or command line. This is necessary for CC and
#|  AR because make sets default values, so the simpler ?=
#|  approach won't work as expected."
#
define allow-override
	$(if $(or $(findstring environment,$(origin $(1))),\
		$(findstring command line,$(origin $(1)))),,\
	$(eval $(1) = $(2)))
endef


#
# Append a compiler flag to a list, or
# override an existing one, according
# to a given pattern.
#
# $(1) : flag list
# $(2) : flag pattern
# $(3) : override
#
define append-flag
	$(if $(filter $(2),$(strip $(value $(1)))),,\
	$(eval override $(1) = $(value $(1)) $(3)))
endef


#------------------------------------------------------------------------------#
#
#  VARIABLES
#
#------------------------------------------------------------------------------#

$(call allow-override,DEBUG,1)
$(call allow-override,CC,gcc)
$(call allow-override,AR,ar)
$(call allow-override,AS,as)
$(call allow-override,LD,ld)
$(call allow-override,STRIP,strip)
$(call allow-override,RL,ranlib)

OS_NAME := $(shell uname)

ifeq ($(strip $(DEBUG)),1)

$(call allow-override,CFLAGS,-Og -g)

override CFLAGS := $(filter-out -DNDEBUG,$(strip $(CFLAGS)))

$(call append-flag,CFLAGS,-std=%,-std=c99)
$(call append-flag,CFLAGS,-Wall,-Wall)
$(call append-flag,CFLAGS,-Wextra,-Wextra)
$(call append-flag,CFLAGS,-Wconversion,-Wconversion)
$(call append-flag,CFLAGS,-Wshadow,-Wshadow)
$(call append-flag,CFLAGS,-Wno-unused-variable,-Wno-unused-variable)

override CFLAGS += -DDEBUG -pipe

else ifeq ($(strip $(DEBUG)),SAN) # Sanitizers

override CC=clang
$(call allow-override,CFLAGS,-Og -ggdb3)

override CFLAGS := $(filter-out -DNDEBUG,$(strip $(CFLAGS)))
override CFLAGS := $(filter-out -fsanitize%,$(strip $(CFLAGS)))

$(call append-flag,CFLAGS,-std=%,-std=c99)
$(call append-flag,CFLAGS,-Wconversion,-Wconversion)
$(call append-flag,CFLAGS,-Wextra,-Wextra)

override CFLAGS += \
	-DDEBUG -pipe           \
	-fsanitize=address      \
	-fsanitize=leak         \
	-fsanitize=null         \
	-fsanitize=bounds       \
	-fsanitize=object-size

else #!DEBUG

$(call allow-override,CFLAGS,-O3)

override CFLAGS := $(filter-out -DDEBUG,$(strip $(CFLAGS)))

$(call append-flag,CFLAGS,-std=%,-std=c99)
$(call append-flag,CFLAGS,-Wall,-Wall)
$(call append-flag,CFLAGS,-Wshadow,-Wshadow)
$(call append-flag,CFLAGS,-Wfatal-errors,-Wfatal-errors)

override CFLAGS += -DNDEBUG -pipe -Wl,-s -Wno-unused-command-line-argument

endif

DEPDIR := .deps
DFLAGS  = -MT $@ -MMD -MP -MF $(DEPDIR)/$(subst /,.,$*).Td
LIBS   := -lc


#------------------------------------------------------------------------------#
#
#  PROJECT
#
#------------------------------------------------------------------------------#

export CFLAGS LDFLAGS ARFLAGS LIBS

SOURCES      := $(shell find src -path 'src/*'              \
                                 -name "*.c"                \
                                 -exec printf '%s ' "{}" \; )

P_LIBS       := 
P_CFLAGS     := -Iinc
P_LDFLAGS    := 

OBJECTS      := $(SOURCES:%.c=%.lo)
DEPENDS      := $(patsubst %,$(DEPDIR)/%,$(subst /,.,$(SOURCES:%.c=%.d)))

TARGETS      := $(sort all build clean dist help)

EXE          := modbus
DISTNAME     := modbus

CLEAN_LIST    = $(EXE)
CLEAN_LIST   += $(OBJECTS)
CLEAN_LIST   += $(DISTNAME).tar.xz


#------------------------------------------------------------------------------#
#
#  Rules
#
#------------------------------------------------------------------------------#

override LIBS    := $(LIBS) $(P_LIBS)
override CFLAGS  := $(CFLAGS) $(P_CFLAGS)
override LDFLAGS := $(LDFLAGS) $(P_LDFLAGS)

all: clean build

dist: clean
	@tar --owner=0 --group=0      \
	     -Jcvf $(DISTNAME).tar.xz \
	     -Hpax                    \
	     makefile                 \
	     inc                      \
	     src

help:
	@printf '%s\n' "$(strip $(TARGETS))"

build: $(EXE)

clean:
	- @rm -vf $(CLEAN_LIST)
	- @rm -rf $(DEPDIR)

$(EXE): $(OBJECTS)
	@printf '%10s %s\n' '[CCLD]' $@
	@$(CC) -o $@ $(CFLAGS) $(LDFLAGS) $^ $(LIBS)

%.lo: FINDEP = $(DEPDIR)/$(subst /,.,$*).d
%.lo: TMPDEP = $(DEPDIR)/$(subst /,.,$*).Td
%.lo: %.c | $(DEPDIR)
	@printf '%10s %s\n' '[CC]' $@
	@$(CC) $(DFLAGS) $(CFLAGS) -fPIC -DPIC -o $@ -c $<
	@mv -f $(TMPDEP) $(FINDEP) && touch $@

$(DEPDIR):
	@mkdir $@

$(DEPENDS):

-include $(DEPENDS)

.PHONY: $(TARGETS)
