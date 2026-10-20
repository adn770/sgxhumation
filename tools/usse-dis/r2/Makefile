# radare2 USSE arch plugin — out-of-tree build.
# Uses the workspace r2 build by default; override R2_PREFIX for a system r2.
R2_PREFIX ?= $(abspath ../../radare2-prefix)
R2_INC    := $(R2_PREFIX)/include/libr
R2_USER_PLUGINS := $(shell $(R2_PREFIX)/bin/radare2 -H R2_USER_PLUGINS 2>/dev/null)

UNAME := $(shell uname)
ifeq ($(UNAME),Darwin)
  SOEXT   := dylib
  LDEXTRA := -undefined dynamic_lookup
else
  SOEXT   := so
  LDEXTRA :=
endif

CFLAGS  += -fPIC -Wall -Wextra -O2 -std=c11 -I$(R2_INC) -I..
PLUGIN   := arch_usse.$(SOEXT)

$(PLUGIN): arch_usse.c ../usse.c ../usse.h ../usse_form.c ../usse_form.h $(wildcard ../forms/*.inc)
	$(CC) $(CFLAGS) -shared -o $@ arch_usse.c ../usse.c ../usse_form.c $(LDEXTRA)

install: $(PLUGIN)
	mkdir -p "$(R2_USER_PLUGINS)"
	cp $(PLUGIN) "$(R2_USER_PLUGINS)/"
	@echo "installed $(PLUGIN) -> $(R2_USER_PLUGINS)"

uninstall:
	rm -f "$(R2_USER_PLUGINS)/$(PLUGIN)"

clean:
	rm -f $(PLUGIN)

.PHONY: install uninstall clean
