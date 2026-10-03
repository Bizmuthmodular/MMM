# !!MMM - Modular Music Maker
## disting NT Plugin Development Environment

This folder contains everything you need to develop sound-generating plugins for the Expert Sleepers disting NT module using FAUST and C++.

---

## 📁 Directory Structure

```
!!MMM/
├── FAUST/                   # FAUST source repositories
│   ├── faust-compiler/     # Complete FAUST compiler source (3,874 files)
│   │   ├── architecture/   # Platform architectures
│   │   ├── compiler/       # Compiler source code
│   │   ├── examples/       # FAUST DSP examples
│   │   ├── libraries/      # Standard libraries (included)
│   │   └── tools/          # Build and utility tools
│   └── faust-libraries/    # FAUST libraries repository
│       ├── stdfaust.lib    # Standard library import
│       ├── oscillators.lib # Oscillator functions
│       ├── filters.lib     # Filter functions
│       ├── envelopes.lib   # Envelope generators
│       ├── reverbs.lib     # Reverb algorithms
│       └── [50+ more...]   # Physics, effects, spatial, etc.
├── FAUST examples/          # Additional FAUST example library
├── include/distingnt/       # disting NT API headers (5 files)
│   ├── api.h               # Main plugin API
│   ├── microtuning.h       # Microtuning/scale support
│   ├── serialisation.h     # JSON preset handling
│   ├── slot.h              # Algorithm slot management
│   └── wav.h               # WAV/sample playback
├── mmmtest.cpp             # Your test plugin file (empty)
├── faust2distingnt         # FAUST → disting NT compiler script
├── nt_arch.cpp             # FAUST architecture bridge
├── apply_metadata.py       # Metadata processor
├── remove_small_arrays.py  # Memory optimizer
├── Makefile                # Build system
└── README.md               # This file
```

---

## 🚀 Quick Start

### Option 1: Compile FAUST DSP to Plugin

```bash
cd /Users/andraseichstaedt/Documents/\!\!MMM
make faust DSP=path/to/yourfile.dsp OUT=myplugin.o
```

### Option 2: Compile C++ Source to Plugin

```bash
make cpp SOURCE=mmmtest.cpp OUT=mmmtest.o
```

### Option 3: Use the FAUST Script Directly

```bash
./faust2distingnt yourfile.dsp output.o
```

---

## 🛠️ Requirements

Before compiling, ensure you have:

1. **ARM Toolchain**: `arm-none-eabi-c++` (for cross-compilation)
   - macOS: `brew install --cask gcc-arm-embedded`
   - Linux: `sudo apt-get install gcc-arm-none-eabi`

2. **FAUST Compiler**: You have two options:
   - **Option A (Recommended)**: Build from the included source in `FAUST/faust-compiler/`
     ```bash
     cd FAUST/faust-compiler
     make
     sudo make install
     ```
   - **Option B**: Download precompiled binary from [faust.grame.fr](https://faust.grame.fr/downloads/)

3. **Python 3**: For post-processing scripts (usually pre-installed on macOS/Linux)

4. **Environment Variables**:
   ```bash
   export NT_API_PATH="/Users/andraseichstaedt/Documents/!!MMM/include"
   export FAUSTARCH="/Users/andraseichstaedt/Documents/!!MMM"
   export FAUST_LIB_PATH="/Users/andraseichstaedt/Documents/!!MMM/FAUST/faust-libraries"
   ```

   Add these to your `~/.zshrc` or `~/.bashrc` to make them permanent.

---

## 📖 Makefile Usage

Run `make help` to see all available commands:

- `make faust DSP=file.dsp OUT=output.o` - Compile FAUST to plugin
- `make cpp SOURCE=file.cpp OUT=output.o` - Compile C++ to plugin
- `make mmmtest` - Compile mmmtest.cpp specifically
- `make config` - Display current build configuration
- `make clean` - Remove generated files

---

## 🎵 Creating Your First Plugin

### Using FAUST

1. **Create a DSP file** (e.g., `oscillator.dsp`):
   ```faust
   import("stdfaust.lib");

   freq = hslider("frequency", 440, 20, 20000, 1);
   gain = hslider("gain", 0.5, 0, 1, 0.01);

   process = os.osc(freq) * gain;
   ```

2. **Compile it**:
   ```bash
   make faust DSP=oscillator.dsp OUT=oscillator.o
   ```

3. **Deploy** `oscillator.o` to your disting NT module

### Using C++ Directly

1. Edit [mmmtest.cpp](mmmtest.cpp) to implement the disting NT plugin API
2. See [include/distingnt/api.h](include/distingnt/api.h:1) for interface details
3. Compile with: `make mmmtest`

---

## 📚 Resources

### Local Resources
- **FAUST Compiler Source**: `FAUST/faust-compiler/` - Full compiler with documentation
- **FAUST Libraries**: `FAUST/faust-libraries/` - 50+ DSP library files
  - `oscillators.lib` - Various oscillator types (sine, saw, square, triangle, phasor)
  - `filters.lib` - Filters (lowpass, highpass, bandpass, etc.)
  - `reverbs.lib` - Reverb algorithms (Freeverb, Zita, etc.)
  - `delays.lib` - Delay lines and echo effects
  - `envelopes.lib` - ADSR and other envelopes
  - `physmodels.lib` - Physical modeling (strings, tubes, etc.)
  - And 40+ more specialized libraries!
- **FAUST Examples**: `FAUST examples/` - Additional example library
- **API Headers**: `include/distingnt/` - Full disting NT API documentation

### Online Resources
- **disting NT API**: https://github.com/expertsleepersltd/distingNT_API
- **FAUST Documentation**: https://faustdoc.grame.fr
- **FAUST Libraries Reference**: https://faustlibraries.grame.fr
- **FAUST Quick Reference**: https://faust.grame.fr/doc/manual/

---

## ⚠️ Hardware Constraints

The disting NT runs on **ARM Cortex-M7** with limited resources:

- ✅ Use fixed I/O counts
- ✅ Avoid large delay buffers
- ✅ Optimize for size (`-Os` flag)
- ❌ No dynamic memory allocation during DSP
- ❌ No file I/O during processing
- ❌ No unbounded recursion

---

## 🔧 Troubleshooting

**"arm-none-eabi-c++ not found"**
- Install ARM embedded toolchain for your platform
- macOS: `brew install --cask gcc-arm-embedded`
- Linux: `sudo apt-get install gcc-arm-none-eabi`

**"faust: command not found"**
- Install FAUST from https://faust.grame.fr/downloads/

**Compilation errors**
- Check that `NT_API_PATH` points to the `include/` directory
- Verify FAUST architecture files are in `FAUSTARCH`

---

## 📝 Notes

- Firmware **v1.7.0+** required on disting NT module
- This FAUST integration is temporary; it may merge into official FAUST project
- Join the community Discord for support (linked from ModWiggler forum)

---

**Happy sound design! 🎛️🎶**
