CC ?= cc
CARGO ?= cargo
HERE := $(dir $(abspath $(lastword $(MAKEFILE_LIST))))
ROOT := $(abspath $(HERE)/../..)
BRIDGE_MANIFEST := $(HERE)/../bridge/Cargo.toml
BRIDGE_LIB := $(HERE)/../bridge/target/release/libsil6250_tod_bridge.a
PLUGIN := $(HERE)/libfprint-2-tod-sil6250.so
PKG_CFLAGS := $(shell pkg-config --cflags libfprint-2-tod-1 libfprint-2)
PKG_LIBS := $(shell pkg-config --libs libfprint-2-tod-1 libfprint-2)

.PHONY: all clean
all: $(PLUGIN)

$(BRIDGE_LIB): $(wildcard $(ROOT)/sil6250/src/*.rs) \
	$(wildcard $(HERE)/../bridge/src/*.rs) $(BRIDGE_MANIFEST)
	$(CARGO) build --locked --manifest-path $(BRIDGE_MANIFEST) --release

$(PLUGIN): sil6250-tod.c sil6250-tod.h $(BRIDGE_LIB)
	$(CC) -std=gnu11 -Wall -Wextra -Werror -fPIC -shared \
	  $(PKG_CFLAGS) -o $@ sil6250-tod.c $(BRIDGE_LIB) \
	  $(PKG_LIBS) -lssl -lcrypto -ldl -lpthread -lm

clean:
	rm -f $(PLUGIN)
