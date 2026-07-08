CC ?= cc

UNAME_S := $(shell uname -s)
UNAME_M := $(shell uname -m)

ifeq ($(UNAME_S),Darwin)
  ifeq ($(UNAME_M),arm64)
    DETECTED_PLATFORM := macos-arm64
  endif
endif
ifeq ($(UNAME_S),Linux)
  ifneq (,$(filter x86_64 amd64,$(UNAME_M)))
    DETECTED_PLATFORM := linux-x86_64
  endif
endif

PLATFORM ?= $(DETECTED_PLATFORM)
SUPPORTED_PLATFORMS := macos-arm64 linux-x86_64

ifeq (,$(filter $(PLATFORM),$(SUPPORTED_PLATFORMS)))
  $(error unsupported PLATFORM "$(PLATFORM)"; expected one of: $(SUPPORTED_PLATFORMS))
endif

BUILD_DIR := build
TARGET := $(BUILD_DIR)/$(PLATFORM)/device_uplink_demo
PROBE_TARGET := $(BUILD_DIR)/$(PLATFORM)/tirtc_accel_device_probe
OBJ_DIR := $(BUILD_DIR)/$(PLATFORM)/obj
SRC_DIR := src
TIRTC_SDK_DIR := 3rd/$(PLATFORM)
TIRTC_INCLUDE_DIR := $(TIRTC_SDK_DIR)/include/tirtc
TIRTC_LIB_DIR := $(TIRTC_SDK_DIR)/lib

SRCS := \
	$(SRC_DIR)/main.c \
	$(SRC_DIR)/device_demo_streamer.c

OBJS := $(SRCS:$(SRC_DIR)/%.c=$(OBJ_DIR)/%.o)
PROBE_SRCS := \
	$(SRC_DIR)/accel_device_probe.c
PROBE_OBJS := $(PROBE_SRCS:$(SRC_DIR)/%.c=$(OBJ_DIR)/%.o)
CPPFLAGS += -I$(SRC_DIR) -I$(TIRTC_INCLUDE_DIR)
CFLAGS += -std=c11 -Wall -Wextra -Werror

TIRTC_LIBS := $(TIRTC_LIB_DIR)/libTiRTC.a
TIRTC_RUNTIME_LIBS :=

ifeq ($(PLATFORM),macos-arm64)
TIRTC_LIBS := $(TIRTC_LIB_DIR)/libTiRTC.dylib
TIRTC_RUNTIME_LIBS := $(TIRTC_LIB_DIR)/libTiRTC.dylib $(TIRTC_LIB_DIR)/libtgrtc.dylib
LDLIBS := -L$(TIRTC_LIB_DIR) -lTiRTC -Wl,-rpath,@executable_path -pthread -lm
endif

ifeq ($(PLATFORM),linux-x86_64)
LDLIBS := $(TIRTC_LIBS) -pthread -lm -ldl
endif

.PHONY: all probe clean clean-platform print-platform

all: $(TARGET) $(PROBE_TARGET)

$(TARGET): $(OBJS) $(TIRTC_LIBS)
	@mkdir -p $(dir $@)
	$(CC) $(OBJS) $(LDLIBS) -o $@
ifneq ($(strip $(TIRTC_RUNTIME_LIBS)),)
	cp $(TIRTC_RUNTIME_LIBS) $(dir $@)
endif

$(PROBE_TARGET): $(PROBE_OBJS) $(TIRTC_LIBS)
	@mkdir -p $(dir $@)
	$(CC) $(PROBE_OBJS) $(LDLIBS) -o $@
ifneq ($(strip $(TIRTC_RUNTIME_LIBS)),)
	cp $(TIRTC_RUNTIME_LIBS) $(dir $@)
endif

probe: $(PROBE_TARGET)

$(OBJ_DIR)/%.o: $(SRC_DIR)/%.c
	@mkdir -p $(dir $@)
	$(CC) $(CPPFLAGS) $(CFLAGS) -c $< -o $@

print-platform:
	@printf '%s\n' "$(PLATFORM)"

clean-platform:
	rm -rf $(BUILD_DIR)/$(PLATFORM)

clean:
	rm -rf $(BUILD_DIR)
