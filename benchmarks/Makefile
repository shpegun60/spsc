CXX ?= g++
ROOT := $(abspath ..)
BUILD_DIR ?= .build
ifeq ($(OS),Windows_NT)
EXEEXT ?= .exe
endif
TARGET := $(BUILD_DIR)/spsc_bench$(EXEEXT)
ASSEMBLY := $(BUILD_DIR)/hotpath_probe.s
DEPENDENCY := $(BUILD_DIR)/spsc_bench.d

CXXFLAGS ?= -std=c++17 -O3 -DNDEBUG -march=native -pthread
CPPFLAGS += -I$(ROOT) -I$(ROOT)/src -I$(ROOT)/third_party/rigtorp_spscqueue/include \
  -DSPSC_ENABLE_SHADOW_INDICES=1 -DSPSC_SHADOW_ALLOW_32BIT=0

ifneq (,$(findstring g++,$(notdir $(CXX))))
CXXFLAGS += --param=destructive-interference-size=64
endif

.PHONY: all assembly

all: $(TARGET)

$(BUILD_DIR):
	mkdir -p $(BUILD_DIR)

$(TARGET): spsc_bench.cpp $(ROOT)/third_party/rigtorp_spscqueue/include/rigtorp/SPSCQueue.h Makefile | $(BUILD_DIR)
	$(CXX) $(CPPFLAGS) $(CXXFLAGS) -MMD -MP -MF $(DEPENDENCY) -MQ $@ -o $@ $<

assembly: hotpath_probe.cpp | $(BUILD_DIR)
	$(CXX) $(CPPFLAGS) $(CXXFLAGS) -S -o $(ASSEMBLY) $<

-include $(DEPENDENCY)
