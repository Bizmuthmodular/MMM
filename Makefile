# Makefile for disting NT Plugin Development
# Supports both FAUST-generated and hand-written C++ plugins

# ============================================================================
# Configuration Variables
# ============================================================================

# Path to disting NT API (update this to your API location)
NT_API_PATH ?= $(shell pwd)/include

# FAUST paths (update if you have FAUST installed)
FAUSTARCH ?= $(shell pwd)
FAUST ?= faust

# ARM Toolchain
CC = arm-none-eabi-c++
AR = arm-none-eabi-ar

# Compiler flags for disting NT (ARM Cortex-M7)
CFLAGS = -std=c++11 \
         -mcpu=cortex-m7 \
         -mfpu=fpv5-d16 \
         -mfloat-abi=hard \
         -mthumb \
         -fno-rtti \
         -fno-exceptions \
         -Os \
         -fPIC \
         -Wall \
         -I$(NT_API_PATH)

# Python for FAUST post-processing
PYTHON = python3

# ============================================================================
# Targets
# ============================================================================

# Default target
.PHONY: all
all: help

# Help target
.PHONY: help
help:
	@echo "disting NT Plugin Makefile"
	@echo "=========================="
	@echo ""
	@echo "Usage:"
	@echo "  make faust DSP=yourfile.dsp OUT=output.o  - Compile FAUST DSP to plugin"
	@echo "  make cpp SOURCE=yourfile.cpp OUT=output.o - Compile C++ source to plugin"
	@echo "  make mmmtest                               - Compile mmmtest.cpp to mmmtest.o"
	@echo "  make BKICK                                 - Compile BKICK plugin"
	@echo "  make BizmuthSeq                            - Compile BizmuthSeq plugin"
	@echo "  make Chords                                - Compile Chords plugin"
	@echo "  make clean                                 - Remove generated files"
	@echo ""
	@echo "Environment Variables:"
	@echo "  NT_API_PATH  - Path to disting NT API includes (default: ./include)"
	@echo "  FAUSTARCH    - Path to FAUST architecture files (default: ./)"
	@echo "  FAUST        - FAUST compiler command (default: faust)"
	@echo ""

# ============================================================================
# FAUST Compilation
# ============================================================================

# Compile FAUST DSP file to disting NT plugin
.PHONY: faust
faust:
	@if [ -z "$(DSP)" ]; then \
		echo "Error: DSP variable not set. Usage: make faust DSP=yourfile.dsp OUT=output.o"; \
		exit 1; \
	fi
	@if [ -z "$(OUT)" ]; then \
		echo "Error: OUT variable not set. Usage: make faust DSP=yourfile.dsp OUT=output.o"; \
		exit 1; \
	fi
	@echo "Compiling FAUST: $(DSP) -> $(OUT)"
	@# Step 1: FAUST -> C++
	$(FAUST) -a $(FAUSTARCH)/nt_arch.cpp -uim -nvi -mem -o $(basename $(OUT)).cpp $(DSP)
	@# Step 2: Remove small arrays optimization
	$(PYTHON) $(FAUSTARCH)/remove_small_arrays.py $(basename $(OUT)).cpp
	@# Step 3: Apply metadata
	$(PYTHON) $(FAUSTARCH)/apply_metadata.py $(basename $(OUT)).cpp
	@# Step 4: Compile to .o
	$(CC) $(CFLAGS) -c -o $(OUT) $(basename $(OUT)).cpp
	@# Step 5: Cleanup intermediate file
	rm -f $(basename $(OUT)).cpp
	@echo "Plugin compiled successfully: $(OUT)"

# ============================================================================
# C++ Compilation
# ============================================================================

# Compile C++ source directly to disting NT plugin
.PHONY: cpp
cpp:
	@if [ -z "$(SOURCE)" ]; then \
		echo "Error: SOURCE variable not set. Usage: make cpp SOURCE=yourfile.cpp OUT=output.o"; \
		exit 1; \
	fi
	@if [ -z "$(OUT)" ]; then \
		echo "Error: OUT variable not set. Usage: make cpp SOURCE=yourfile.cpp OUT=output.o"; \
		exit 1; \
	fi
	@echo "Compiling C++: $(SOURCE) -> $(OUT)"
	$(CC) $(CFLAGS) -c -o $(OUT) $(SOURCE)
	@echo "Plugin compiled successfully: $(OUT)"

# ============================================================================
# Specific Targets
# ============================================================================

# Compile mmmtest.cpp
.PHONY: mmmtest
mmmtest:
	@echo "Compiling mmmtest.cpp -> mmmtest.o"
	$(CC) $(CFLAGS) -c -o mmmtest.o mmmtest.cpp
	@echo "Plugin compiled successfully: mmmtest.o"

# Compile BKICK plugin
.PHONY: BKICK
BKICK:
	@echo "Compiling BKICK/BKICK.cpp -> BKICK/BKICK.o"
	$(CC) $(CFLAGS) -c -o BKICK/BKICK.o BKICK/BKICK.cpp
	@echo "Plugin compiled successfully: BKICK/BKICK.o"

# Compile BizmuthSeq plugin
.PHONY: BizmuthSeq
BizmuthSeq:
	@echo "Compiling BizmuthSeq/BizmuthSeq.cpp -> BizmuthSeq/BizmuthSeq.o"
	$(CC) $(CFLAGS) -c -o BizmuthSeq/BizmuthSeq.o BizmuthSeq/BizmuthSeq.cpp
	@echo "Plugin compiled successfully: BizmuthSeq/BizmuthSeq.o"

# Compile Chords plugin
.PHONY: Chords
Chords:
	@echo "Compiling Chords/Chords.cpp -> Chords/Chords.o"
	$(CC) $(CFLAGS) -c -o Chords/Chords.o Chords/Chords.cpp
	@echo "Plugin compiled successfully: Chords/Chords.o"

# ============================================================================
# Utility Targets
# ============================================================================

# Clean generated files
.PHONY: clean
clean:
	@echo "Cleaning generated files..."
	rm -f *.o *.cpp.bak
	@echo "Clean complete."

# Display current configuration
.PHONY: config
config:
	@echo "Current Configuration:"
	@echo "  NT_API_PATH  = $(NT_API_PATH)"
	@echo "  FAUSTARCH    = $(FAUSTARCH)"
	@echo "  FAUST        = $(FAUST)"
	@echo "  CC           = $(CC)"
	@echo "  CFLAGS       = $(CFLAGS)"

# ============================================================================
# Pattern Rules (Advanced)
# ============================================================================

# Automatic rule: .dsp -> .o (FAUST compilation)
%.o: %.dsp
	@echo "Auto-compiling FAUST: $< -> $@"
	$(FAUST) -a $(FAUSTARCH)/nt_arch.cpp -uim -nvi -mem -o $*.cpp $<
	$(PYTHON) $(FAUSTARCH)/remove_small_arrays.py $*.cpp
	$(PYTHON) $(FAUSTARCH)/apply_metadata.py $*.cpp
	$(CC) $(CFLAGS) -c -o $@ $*.cpp
	rm -f $*.cpp
	@echo "Plugin compiled: $@"

# Automatic rule: .cpp -> .o (C++ compilation)
# Note: This is disabled by default to avoid conflicts with intermediate files
# Uncomment if you want automatic .cpp -> .o compilation
# %.o: %.cpp
# 	@echo "Auto-compiling C++: $< -> $@"
# 	$(CC) $(CFLAGS) -c -o $@ $<
# 	@echo "Plugin compiled: $@"
