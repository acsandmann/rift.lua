NAME=rift
CC?=clang
PKG_CONFIG?=pkg-config
LUA?=lua
CFLAGS?=-std=c99 -O3 -g -fPIC
MODULE_LDFLAGS?=-bundle -undefined dynamic_lookup
INSTALL_DIR=$(HOME)/.local/share/rift.lua

LUA_DIR?=lua-5.4.7
LUA_PC_NAMES?=lua5.5 lua-5.5 lua lua5.4 lua-5.4 lua54 lua-54
TARGET_ARCH?=$(shell uname -m)
USE_SYSTEM_LUA?=auto
LINK_LUA?=0

ifeq ($(origin LUA_PC),undefined)
  LUA_PC_CANDIDATES=$(LUA_PC_NAMES)
else
  LUA_PC_CANDIDATES=$(LUA_PC)
endif

RESOLVED_LUA_CFLAGS:=$(shell sh scripts/find-lua-cflags.sh "$(PKG_CONFIG)" "$(TARGET_ARCH)" "$(LUA)" $(LUA_PC_CANDIDATES))
RESOLVED_LUA_PC:=$(shell sh scripts/find-lua-pc.sh "$(PKG_CONFIG)" "$(TARGET_ARCH)" $(LUA_PC_CANDIDATES))

WANT_SYSTEM_LUA=$(filter-out 0 false no,$(USE_SYSTEM_LUA))

ifeq ($(WANT_SYSTEM_LUA),)
  LUA_MODE=vendored
else ifneq ($(RESOLVED_LUA_CFLAGS),)
  LUA_MODE=system
else ifeq ($(USE_SYSTEM_LUA),auto)
  LUA_MODE=vendored
else
  $(error USE_SYSTEM_LUA=$(USE_SYSTEM_LUA) requested, but no Lua headers matching '$(LUA)' were found. Try make LUA=/opt/homebrew/bin/lua or set USE_SYSTEM_LUA=auto to fall back to $(LUA_DIR))
endif

ifeq ($(LUA_MODE),system)
  $(info Using Lua headers matching '$(LUA)': $(RESOLVED_LUA_CFLAGS))
  LUA_CFLAGS=$(RESOLVED_LUA_CFLAGS)
else
  $(info Using bundled Lua headers from $(LUA_DIR))
  LUA_CFLAGS=-I$(LUA_DIR)/src
endif

ifeq ($(LINK_LUA),1)
  ifeq ($(LUA_MODE),system)
    LUA_LIBS=$(shell $(PKG_CONFIG) --libs $(RESOLVED_LUA_PC))
    LUA_DEPS=
  else
    LUA_LIBS=bin/liblua.a
    LUA_DEPS=bin/liblua.a
  endif
else
  LUA_LIBS=
  LUA_DEPS=
endif

LIBS=$(LUA_LIBS) -framework CoreFoundation
ARCH?=-arch $(TARGET_ARCH)

bin/$(NAME).so: src/$(NAME).c src/*.c $(LUA_DEPS) | bin
	$(CC) $(CFLAGS) $(MODULE_LDFLAGS) $(ARCH) $(LUA_CFLAGS) $(filter %.c,$^) $(LIBS) -o bin/$(NAME).so

install: bin/$(NAME).so | $(INSTALL_DIR)
	mkdir -p $(INSTALL_DIR)
	mv bin/$(NAME).so $(INSTALL_DIR)

uninstall:
	rm -rf $(INSTALL_DIR)/$(NAME).so

clean:
	rm -rf bin
	cd $(LUA_DIR) && make clean

bin/liblua.a: | bin
	$(MAKE) -C $(LUA_DIR)
	cp $(LUA_DIR)/src/liblua.a bin

bin:
	mkdir bin

$(INSTALL_DIR):
	mkdir -p $(INSTALL_DIR)
