# Makefile for c_ncnn C wrapper library

LIBRARY = libc_ncnn
VERSION = 1.0.0

# Directories
SRC_DIR = src
INC_DIR = include
BUILD_DIR = build
INSTALL_PREFIX ?= /usr/local

# Source files
SOURCES = $(SRC_DIR)/c_ncnn.cpp $(SRC_DIR)/http_client.cpp
OBJECTS = $(SOURCES:$(SRC_DIR)/%.cpp=$(BUILD_DIR)/%.o)

# Compiler flags
CXX = g++
CXXFLAGS = -std=c++11 -fPIC -Wall -Wextra -Wno-unused-function -O2
INCLUDES = -I$(INC_DIR)

# Linker flags
LDFLAGS = -shared
LIBS = 

# Check for optional dependencies
# Note: All dependencies are optional - the library will build without them
HAVE_NCNN = $(shell pkg-config --exists ncnn 2>/dev/null && echo yes || echo no)
HAVE_ONNXRUNTIME = $(shell pkg-config --exists onnxruntime 2>/dev/null && echo yes || echo no)

# Custom HTTP client is built-in (HTTP-only, no HTTPS)
# No external dependencies needed for downloading models over HTTP
$(info Using built-in HTTP client (HTTP-only, no HTTPS support))

ifeq ($(HAVE_NCNN),yes)
    CXXFLAGS += -DHAVE_NCNN $(shell pkg-config --cflags ncnn)
    LIBS += $(shell pkg-config --libs ncnn)
endif

ifeq ($(HAVE_ONNXRUNTIME),yes)
    CXXFLAGS += -DHAVE_ONNXRUNTIME $(shell pkg-config --cflags onnxruntime)
    LIBS += $(shell pkg-config --libs onnxruntime)
endif

# Default target
all: $(BUILD_DIR)/$(LIBRARY).so $(BUILD_DIR)/$(LIBRARY).a

# Create build directory
$(BUILD_DIR):
	mkdir -p $(BUILD_DIR)

# Compile source files
$(BUILD_DIR)/%.o: $(SRC_DIR)/%.cpp | $(BUILD_DIR)
	$(CXX) $(CXXFLAGS) $(INCLUDES) -c $< -o $@

# Build shared library
$(BUILD_DIR)/$(LIBRARY).so: $(OBJECTS) | $(BUILD_DIR)
	$(CXX) $(LDFLAGS) -o $@ $(OBJECTS) $(LIBS)

# Build static library
$(BUILD_DIR)/$(LIBRARY).a: $(OBJECTS) | $(BUILD_DIR)
	ar rcs $@ $(OBJECTS)

# Install
install: all
	mkdir -p $(INSTALL_PREFIX)/lib
	mkdir -p $(INSTALL_PREFIX)/include
	cp $(BUILD_DIR)/$(LIBRARY).so $(INSTALL_PREFIX)/lib/
	cp $(BUILD_DIR)/$(LIBRARY).a $(INSTALL_PREFIX)/lib/
	cp $(INC_DIR)/c_ncnn.h $(INSTALL_PREFIX)/include/

# Clean
clean:
	rm -rf $(BUILD_DIR)

# Test (simple compilation test)
test: all
	@echo "Library built successfully"
	@echo "NCNN support: $(HAVE_NCNN)"
	@echo "ONNX Runtime support: $(HAVE_ONNXRUNTIME)"
	@echo "HTTP client: Built-in (HTTP-only, no HTTPS)"

.PHONY: all install clean test

