# Build libsigrok4DSL.a from the pinned DSView submodule, then link the headless
# `dscap` capture tool against it. All objects land in ./build/; the submodule
# tree is never modified. (CMake is also supported — see CMakeLists.txt.)
# CMake conventionally owns build/, so Make keeps its objects in obj/.
DV    := third_party/DSView
SR    := $(DV)/libsigrok4DSL
CM    := $(DV)/common
BUILD := obj

CC      := gcc
CFLAGS  := -std=c99 -O2 -fPIC -D_DEFAULT_SOURCE -Wall -Wno-unused-parameter \
           -I$(SR) -I$(CM) $(shell pkg-config --cflags glib-2.0)
LDLIBS  := $(shell pkg-config --libs glib-2.0) -lusb-1.0 -lz -lm -lpthread

# Exact source set DSView compiles into libsigrok4DSL (from its CMakeLists),
# plus the common xlog + minizip it depends on. Paths are relative to $(DV).
SRCS := \
  libsigrok4DSL/version.c libsigrok4DSL/strutil.c libsigrok4DSL/std.c \
  libsigrok4DSL/session_driver.c libsigrok4DSL/session.c libsigrok4DSL/log.c \
  libsigrok4DSL/hwdriver.c libsigrok4DSL/error.c libsigrok4DSL/backend.c \
  libsigrok4DSL/trigger.c libsigrok4DSL/dsdevice.c libsigrok4DSL/lib_main.c \
  libsigrok4DSL/output/output.c libsigrok4DSL/output/csv.c \
  libsigrok4DSL/output/gnuplot.c libsigrok4DSL/output/srzip.c libsigrok4DSL/output/vcd.c \
  libsigrok4DSL/input/input.c libsigrok4DSL/input/in_binary.c \
  libsigrok4DSL/input/in_vcd.c libsigrok4DSL/input/in_wav.c \
  libsigrok4DSL/hardware/demo/demo.c \
  libsigrok4DSL/hardware/DSL/dslogic.c libsigrok4DSL/hardware/DSL/dscope.c \
  libsigrok4DSL/hardware/DSL/command.c libsigrok4DSL/hardware/DSL/dsl.c \
  libsigrok4DSL/hardware/common/usb.c libsigrok4DSL/hardware/common/ezusb.c \
  common/log/xlog.c \
  common/minizip/zip.c common/minizip/unzip.c common/minizip/ioapi.c

OBJS := $(addprefix $(BUILD)/,$(SRCS:.c=.o))

# dscap's own sources — the CLI is split across these translation units.
DSCAP_SRCS := $(addprefix src/,dscap.c json.c cfg.c dsl.c)
DSCAP_HDRS := $(addprefix src/,dscap.h json.h cfg.h dsl.h)

all: dscap

$(BUILD)/%.o: $(DV)/%.c
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) -c $< -o $@

$(BUILD)/libsigrok4DSL.a: $(OBJS)
	ar rcs $@ $(OBJS)

dscap: $(DSCAP_SRCS) $(DSCAP_HDRS) $(BUILD)/libsigrok4DSL.a
	$(CC) $(CFLAGS) $(DSCAP_SRCS) $(BUILD)/libsigrok4DSL.a $(LDLIBS) -o dscap

clean:
	rm -rf $(BUILD) dscap

.PHONY: all clean
