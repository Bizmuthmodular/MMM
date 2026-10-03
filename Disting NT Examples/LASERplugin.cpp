// Disting NT plugin: Draws a rotating wireframe cube on an XY oscilloscope.
// • The cube is normalised using a single scale factor so all vertices lie on a
//   unit sphere.
// • Each edge is traversed in a fixed order with blanked reposition moves so the
//   geometry is correct with no undesired path jumps.
// • No hidden‐line culling (all edges are always drawn when the beam is on).
// • BlankWindow (0…1000 μs) sets per‐edge blank length; BlankPhase (–1000…+1000 μs) shifts that blank window.
// • Intensity "on" = +5 V, "off" = 0 V.
//
// Pages:
//   1. Frequency   [1 – 1000 Hz]
//   2. Rotation    [RotX, RotY, RotZ each 0 – 360°]
//   3. Camera      [Distance (0.01 – 10), Projection (Ortho/Persp), Polarity (Normal/Inverted)]
//   4. Routing     [X Out (0–27), Y Out (0–27), Int Out (0–27)]
//   5. Blanking    [BlankWindow (0…1000 μs), BlankPhase (–1000…+1000 μs)]
//   6. Quantize    [Resolution (0–100)]
//   7. AmpMod      [AmpMod, AmpCorse, AmpFine, AmpWave, AmpPhase]
//
// All initializer lists exactly match their array dimensions.

#include "distingnt/api.h"
#include <cmath>
#include <cstdint>
#include <cstring>
#include <new>

static constexpr int faceSizeMax = 6;

//—-----------------------------------------------------------------------------------------------
struct Segment { uint8_t a; uint8_t b; uint8_t draw; };

// Horizontal Lines
static const float lineVerts[64][3] = {
    // We'll generate these dynamically, but need space for up to 32 lines * 2 endpoints each
    {0,0,0}, {0,0,0} // Placeholder - will be filled dynamically
};

static const Segment linePath[64] = {
    // We'll generate these dynamically too
    {0,1,1} // Placeholder
};

// 1) Cube Vertex Data & Drawing Segments

static const float rawCubeVerts[8 * 3] = {
    -1.0f, -1.0f, -1.0f,
     1.0f, -1.0f, -1.0f,
     1.0f,  1.0f, -1.0f,
    -1.0f,  1.0f, -1.0f,
    -1.0f, -1.0f,  1.0f,
     1.0f, -1.0f,  1.0f,
     1.0f,  1.0f,  1.0f,
    -1.0f,  1.0f,  1.0f
};

static const Segment cubeSegments[] = {
    {0,1,1}, {1,2,1}, {2,3,1}, {3,0,1},
    {0,4,1}, {4,5,1}, {5,6,1}, {6,7,1}, {7,4,1},
    {4,1,0}, {1,5,1}, {5,2,0}, {2,6,1}, {6,3,0},
    {3,7,1}, {7,0,0}
};

static const int numSegments = sizeof(cubeSegments)/sizeof(cubeSegments[0]);

// Octahedron
static const float octaVerts[6][3] = {
    { 1,  0,  0},
    {-1,  0,  0},
    { 0,  1,  0},
    { 0, -1,  0},
    { 0,  0,  1},
    { 0,  0, -1}
};
static const Segment octaPath[] = {
    {0,2,1},{2,4,1},{4,3,1},{3,0,1},{0,4,1},{4,1,1},{1,2,1},{2,5,1},{5,3,1},{3,1,1},{1,5,1},{5,0,1}
};

// 2) Scale and Quantization

struct Scale {
    const int* steps;
    int numSteps;
    int span;
};

struct Quantizer {
    Scale scale;
    float minCV;
    float maxCV;

    float quantize(float cv) const {
    float norm = (cv - minCV) / (maxCV - minCV);
    float octaves = maxCV - minCV;
    int totalSpan = static_cast<int>(scale.span * octaves + 0.5f); // round to nearest int
    int value = static_cast<int>(norm * totalSpan);

    int octave = value / scale.span;
    int withinOctave = value % scale.span;

    int closest = scale.steps[0];
    int minDiff = abs(withinOctave - closest);
    for (int i = 1; i < scale.numSteps; ++i) {
        int diff = abs(withinOctave - scale.steps[i]);
        if (diff < minDiff) {
            minDiff = diff;
            closest = scale.steps[i];
        }
    }

    int quantized = octave * scale.span + closest;
    float output = (float)quantized / totalSpan * (maxCV - minCV) + minCV;
    return output;
    }
};

// Chromatic (already present)
static const int chromatic[] = { 0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11 };
static const Scale scaleChromatic = { chromatic, 12, 12 };

// Major (Ionian)
static const int major[] = { 0, 2, 4, 5, 7, 9, 11 };
static const Scale scaleMajor = { major, 7, 12 };

// Natural Minor (Aeolian)
static const int minor[] = { 0, 2, 3, 5, 7, 8, 10 };
static const Scale scaleMinor = { minor, 7, 12 };

// Harmonic Minor
static const int harmonicMinor[] = { 0, 2, 3, 5, 7, 8, 11 };
static const Scale scaleHarmonicMinor = { harmonicMinor, 7, 12 };

// Major Pentatonic
static const int majorPentatonic[] = { 0, 2, 4, 7, 9 };
static const Scale scaleMajorPentatonic = { majorPentatonic, 5, 12 };

// Minor Pentatonic
static const int minorPentatonic[] = { 0, 3, 5, 7, 10 };
static const Scale scaleMinorPentatonic = { minorPentatonic, 5, 12 };

// Hirajoshi (Japanese pentatonic)
static const int hirajoshi[] = { 0, 2, 3, 7, 8 };
static const Scale scaleHirajoshi = { hirajoshi, 5, 12 };

// Gamelan (Slendro-like, 5 notes, nearly equidistant)
static const int gamelan[] = { 0, 2, 5, 7, 10 };
static const Scale scaleGamelan = { gamelan, 5, 12 };

// 5th (Tritone scale: root and tritone)
static const int fifths[] = { 0, 7 };
static const Scale scaleFifths = { fifths, 2, 12 };

// Diminished 5th (6-semitone jumps: root and tritone)
static const int dim5th[] = { 0, 6 };
static const Scale scaleDim5th = { dim5th, 2, 12 };

//—-----------------------------------------------------------------------------------------------
// 5) Parameter Definitions
//—-----------------------------------------------------------------------------------------------

static const _NT_parameter paramFreq = {
    .name        = "Frequency",
    .min         = 1,
    .max         = 1000,
    .def         = 50,
    .unit        = kNT_unitHz,
    .scaling     = kNT_scalingNone,
    .enumStrings = NULL
};

static const _NT_parameter paramRotX = {
    .name        = "RotX",
    .min         = 0,
    .max         = 360,
    .def         = 0,
    .unit        = kNT_unitNone,
    .scaling     = kNT_scalingNone,
    .enumStrings = NULL
};

static const _NT_parameter paramRotY = {
    .name        = "RotY",
    .min         = 0,
    .max         = 360,
    .def         = 0,
    .unit        = kNT_unitNone,
    .scaling     = kNT_scalingNone,
    .enumStrings = NULL
};

static const _NT_parameter paramRotZ = {
    .name        = "RotZ",
    .min         = 0,
    .max         = 360,
    .def         = 0,
    .unit        = kNT_unitNone,
    .scaling     = kNT_scalingNone,
    .enumStrings = NULL
};

static const _NT_parameter paramDistance = {
    .name        = "Distance",
    .min         = 1,    // scales 0.01…10.00
    .max         = 1000,
    .def         = 500,
    .unit        = kNT_unitNone,
    .scaling     = kNT_scalingNone,
    .enumStrings = NULL
};

static const char* const projEnumStrings[] = { "Orthographic", "Perspective", NULL };
static const _NT_parameter paramProjection = {
    .name        = "Projection",
    .min         = 0,
    .max         = 1,
    .def         = 1,
    .unit        = kNT_unitEnum,
    .scaling     = kNT_scalingNone,
    .enumStrings = projEnumStrings
};

static const char* const polarityEnum[] = { "Normal", "Inverted", NULL };
static const _NT_parameter paramPolarity = {
    .name        = "Polarity",
    .min         = 0,
    .max         = 1,
    .def         = 0,
    .unit        = kNT_unitEnum,
    .scaling     = kNT_scalingNone,
    .enumStrings = polarityEnum
};

static const _NT_parameter paramSize = {
    .name        = "Size",
    .min         = 0,
    .max         = 100,
    .def         = 100,
    .unit        = kNT_unitNone,
    .scaling     = kNT_scalingNone,
    .enumStrings = NULL
};

static const _NT_parameter paramXOut = {
    .name        = "X Out",
    .min         = 0,
    .max         = 27,
    .def         = 12,
    .unit        = kNT_unitNone,
    .scaling     = kNT_scalingNone,
    .enumStrings = NULL
};

static const _NT_parameter paramYOut = {
    .name        = "Y Out",
    .min         = 0,
    .max         = 27,
    .def         = 13,
    .unit        = kNT_unitNone,
    .scaling     = kNT_scalingNone,
    .enumStrings = NULL
};

static const _NT_parameter paramIOut = {
    .name        = "Int Out",
    .min         = 0,
    .max         = 27,
    .def         = 14,
    .unit        = kNT_unitNone,
    .scaling     = kNT_scalingNone,
    .enumStrings = NULL
};

static const _NT_parameter paramBlankWindow = {
    .name        = "BlankWindow",
    .min         = 0,
    .max         = 1000,
    .def         = 10,
    .unit        = kNT_unitNone, // μs
    .scaling     = kNT_scalingNone,
    .enumStrings = NULL
};

static const _NT_parameter paramBlankPhase = {
    .name        = "BlankPhase",
    .min         = -1000,
    .max         = +1000,
    .def         = 0,
    .unit        = kNT_unitNone, // μs
    .scaling     = kNT_scalingNone,
    .enumStrings = NULL
};

static const char* const modCourseStrings[] = {
    "/4", "/3", "/2", "0",
    "x2",  "x3",  "x4",  "x5",  "x6",  "x7",  "x8",  "x9",
    "x10", "x11", "x12", "x13", "x14", "x15", "x16", "x17",
    "x18", "x19", "x20", "x21", "x22", "x23", "x24", "x25",
    "x26", "x27", "x28", "x29", "x30", "x31", "x32",
    NULL
};

static const _NT_parameter paramResolution = {
    .name        = "Resolution",
    .min         = 0,
    .max         = 100,
    .def         = 0,
    .unit        = kNT_unitNone,
    .scaling     = kNT_scalingNone,
    .enumStrings = NULL
};

static const _NT_parameter paramAmpMod = {
    .name        = "AmpMod",
    .min         = 0,
    .max         = 127,
    .def         = 0,
    .unit        = kNT_unitNone,
    .scaling     = kNT_scalingNone,
    .enumStrings = NULL
};

static const _NT_parameter paramAmpCorse = {
    .name        = "AmpCorse",
    .min         = 0,
    .max         = 34,
    .def         = 4,
    .unit        = kNT_unitEnum,
    .scaling     = kNT_scalingNone,
    .enumStrings = modCourseStrings
};

static const char* const modWaveStrings[] = {
    "Square", "Triangle", "Saw", "Ramp", "Sine", NULL
};

static const _NT_parameter paramAmpFine = {
    .name        = "AmpFine",
    .min         = -100,
    .max         = 100,
    .def         = 0,
    .unit        = kNT_unitNone,
    .scaling     = kNT_scalingNone,
    .enumStrings = NULL
};

static const _NT_parameter paramAmpWave = {
    .name        = "AmpWave",
    .min         = 0,
    .max         = 4,
    .def         = 4,
    .unit        = kNT_unitEnum,
    .scaling     = kNT_scalingNone,
    .enumStrings = modWaveStrings
};

static const _NT_parameter paramAmpPhase = {
    .name        = "AmpPhase",
    .min         = 0,
    .max         = 360,
    .def         = 0,
    .unit        = kNT_unitNone,
    .scaling     = kNT_scalingNone,
    .enumStrings = NULL
};

static const _NT_parameter paramXOffset = {
    .name        = "X Offset",
    .min         = -100,
    .max         = 100,
    .def         = 0,
    .unit        = kNT_unitNone,
    .scaling     = kNT_scalingNone,
    .enumStrings = NULL
};

static const _NT_parameter paramYOffset = {
    .name        = "Y Offset",
    .min         = -100,
    .max         = 100,
    .def         = 0,
    .unit        = kNT_unitNone,
    .scaling     = kNT_scalingNone,
    .enumStrings = NULL
};

static const _NT_parameter paramZOffset = {
    .name        = "Z Offset",
    .min         = -100,
    .max         = 100,
    .def         = 0,
    .unit        = kNT_unitNone,
    .scaling     = kNT_scalingNone,
    .enumStrings = NULL
};

static const _NT_parameter paramXOffMod = {
    .name        = "XoffMod",
    .min         = 0,
    .max         = 100,
    .def         = 0,
    .unit        = kNT_unitNone,
    .scaling     = kNT_scalingNone,
    .enumStrings = NULL
};

static const _NT_parameter paramYOffMod = {
    .name        = "YoffMod",
    .min         = 0,
    .max         = 100,
    .def         = 0,
    .unit        = kNT_unitNone,
    .scaling     = kNT_scalingNone,
    .enumStrings = NULL
};

static const _NT_parameter paramZOffMod = {
    .name        = "ZoffMod",
    .min         = 0,
    .max         = 100,
    .def         = 0,
    .unit        = kNT_unitNone,
    .scaling     = kNT_scalingNone,
    .enumStrings = NULL
};

static const _NT_parameter paramOffCorse = {
    .name        = "OffCorse",
    .min         = 0,
    .max         = 34,
    .def         = 4,
    .unit        = kNT_unitEnum,
    .scaling     = kNT_scalingNone,
    .enumStrings = modCourseStrings
};

static const _NT_parameter paramOffFine = {
    .name        = "OffFine",
    .min         = -100,
    .max         = 100,
    .def         = 0,
    .unit        = kNT_unitNone,
    .scaling     = kNT_scalingNone,
    .enumStrings = NULL
};

static const _NT_parameter paramOffWave = {
    .name        = "OffWave",
    .min         = 0,
    .max         = 4,
    .def         = 4,
    .unit        = kNT_unitEnum,
    .scaling     = kNT_scalingNone,
    .enumStrings = modWaveStrings
};

static const _NT_parameter paramOffPhase = {
    .name        = "OffPhase",
    .min         = 0,
    .max         = 360,
    .def         = 0,
    .unit        = kNT_unitNone,
    .scaling     = kNT_scalingNone,
    .enumStrings = NULL
};

static const _NT_parameter paramNPM = {
    .name        = "NPM",
    .min         = 0,
    .max         = 100,
    .def         = 100,
    .unit        = kNT_unitNone,
    .scaling     = kNT_scalingNone,
    .enumStrings = NULL
};

static const _NT_parameter paramNpmCurve = {
    .name        = "NpmCurve",
    .min         = 0,
    .max         = 100,
    .def         = 100,
    .unit        = kNT_unitNone,
    .scaling     = kNT_scalingNone,
    .enumStrings = NULL
};

static const char* const solidEnumStrings[] = {
    "HorizontalLines", "Cube", "Octahedron", "Circles", NULL
};

static const _NT_parameter paramSolid = {
    .name        = "Solid",
    .min         = 0,
    .max         = 3,
    .def         = 1, // Default to Cube
    .unit        = kNT_unitEnum,
    .scaling     = kNT_scalingNone,
    .enumStrings = solidEnumStrings
};

static const char* const quantiseEnumStrings[] = {
    "Unquantised",
    "Chromatic",
    "Major",
    "Minor",
    "HarmonicMinor",
    "MajPent",
    "MinPent",
    "Hirajoshi",
    "Gamelan",
    "Fifths",
    "Dim5th",
    NULL
};
static const _NT_parameter paramQuantise = {
    .name        = "Quantise",
    .min         = 0,
    .max         = 10,
    .def         = 0,
    .unit        = kNT_unitEnum,
    .scaling     = kNT_scalingNone,
    .enumStrings = quantiseEnumStrings
};

static const _NT_parameter paramNCircles = {
    .name        = "NCircles",
    .min         = 1,
    .max         = 32,
    .def         = 8,
    .unit        = kNT_unitNone,
    .scaling     = kNT_scalingNone,
    .enumStrings = NULL
};

static const char* const circXCorseEnum[] = { "On", "Off", NULL };
static const _NT_parameter paramCircXCorse = {
    .name        = "CircXCorse",
    .min         = 0,
    .max         = 1,
    .def         = 0, // 0 = On (default), 1 = Off
    .unit        = kNT_unitEnum,
    .scaling     = kNT_scalingNone,
    .enumStrings = circXCorseEnum
};

static const _NT_parameter allParams[] = {
    paramFreq,         //  0
    paramRotX,         //  1
    paramRotY,         //  2
    paramRotZ,         //  3
    paramDistance,     //  4
    paramProjection,   //  5
    paramPolarity,     //  6
    paramSize,         //  7  
    paramXOut,         //  8
    paramYOut,         //  9
    paramIOut,         // 10
    paramBlankWindow,  // 11
    paramBlankPhase,   // 12
    paramResolution,   // 13
    paramAmpMod,       // 14
    paramAmpCorse,     // 15
    paramAmpFine,      // 16
    paramAmpWave,      // 17
    paramAmpPhase,     // 18
    paramXOffset,      // 19
    paramYOffset,      // 20
    paramZOffset,      // 21
    paramXOffMod,      // 22
    paramYOffMod,      // 23
    paramZOffMod,      // 24
    paramOffCorse,     // 25
    paramOffFine,      // 26
    paramOffWave,      // 27
    paramOffPhase,     // 28
    paramNPM,          // 29 
    paramNpmCurve,     // 30
    paramSolid,        // 31
    paramQuantise,     // 32  
    paramNCircles,     // 33
    paramCircXCorse,   // 34
};

static const uint8_t page1_indices[] = { 0, 32 };
static const uint8_t page2_indices[] = { 1, 2, 3 };
static const uint8_t page3_indices[] = { 4, 5, 6, 7, 29, 30 };
static const uint8_t page4_indices[] = { 8, 9, 10 };
static const uint8_t page5_indices[] = { 11, 12 };
static const uint8_t page6_indices[] = { 13 };
static const uint8_t page7_indices[] = { 14, 15, 16, 17, 18 };
static const uint8_t page8_indices[] = { 19, 20, 21, 22, 23, 24, 25, 26, 27, 28 };
static const uint8_t page9_indices[] = { 31, 33, 34 }; // Solid selection

static const _NT_parameterPage pages[] = {
    { "Frequency",   2,  page1_indices },
    { "Rotation",    3,  page2_indices },
    { "Camera",      6,  page3_indices },
    { "Routing",     3,  page4_indices },
    { "Blanking",    2,  page5_indices },
    { "Quantize",    1,  page6_indices },
    { "AmpMod",      5,  page7_indices },
    { "Offset",      10, page8_indices },
    { "Solids",      3,  page9_indices }
};

static const _NT_parameterPages parameterPages = {
    .numPages = 9,
    .pages    = pages
};

//—-----------------------------------------------------------------------------------------------
// 6) Per‐Instance State Structure
//—-----------------------------------------------------------------------------------------------

struct PolyInstance : public _NT_algorithm {
    float phase;
    float sinX, cosX;
    float sinY, cosY;
    float sinZ, cosZ;
    float freq_Hz;
    float cameraDist;
    int   projectionMode; // 0=Ortho, 1=Persp
    int   polarity;       // 0=Normal, 1=Inverted
    int   xOutBus, yOutBus, iOutBus;

    // Amplitude modulation state
    float ampModAmt;       // 0..1
    int   ampCorseIdx;     // 0..34
    int   ampFine;         // -100..100 (0.1 Hz units)
    int   ampWave;         // 0..4
    float ampPhaseOffset;  // 0..1
    float ampPhase;        // 0..1 running phase

    float blankWindow_us; // 0…1000 μs
    float blankPhase_us;  // –1000…+1000 μs
    int   resolution;     // 0..100

    // Offset
    float offsetX, offsetY, offsetZ;
    float xModAmt, yModAmt, zModAmt;
    int   offCorseIdx;
    int   offFine;
    int   offWave;
    float offPhase;
    float offPhaseOffset;

    // Size
    float size;          // 0–100

    // NPM (Number of Points per Move) and Curve
    float npm;       // 0.0 to 1.0
    float npmCurve;  // 0.0 to 1.0

    // Solids
    int solidType; // 0=Tetrahedron, 1=Cube, 2=Octahedron, 3=Circles
    // Quantization
    int quantiseMode; // 0=Unquantised, 1=Semitones
    // Circles
    int nCircles; // Number of circles to draw (1-32)
    int circXCorse; // 0 = On, 1 = Off

    PolyInstance() {
        parameters       = nullptr;
        parameterPages   = nullptr;
        vIncludingCommon = nullptr;
        v                = nullptr;
        phase            = 0.0f;
        sinX = 0.0f; cosX = 1.0f;
        sinY = 0.0f; cosY = 1.0f;
        sinZ = 0.0f; cosZ = 1.0f;
        freq_Hz          = 50.0f;
        cameraDist       = 5.0f;
        projectionMode   = 1;
        polarity         = 0;
        xOutBus = 12; yOutBus = 13; iOutBus = 14;
        resolution       = 0;
        ampModAmt        = 0.0f;
        ampCorseIdx      = 4;
        ampFine          = 0;
        ampWave          = 4;
        ampPhaseOffset   = 0.0f;
        ampPhase         = 0.0f;
        blankWindow_us   = 0.0f;
        blankPhase_us    = 0.0f;
        offsetX = 0.0f;
        offsetY = 0.0f;
        offsetZ = 0.0f;
        xModAmt = yModAmt = zModAmt = 0.0f;
        offCorseIdx = 4;
        offFine = 0;
        offWave = 4;
        offPhase = 0.0f;
        offPhaseOffset = 0.0f;
        size = 1.0f;
        npm = 1.0f;
        npmCurve = 1.0f;
        solidType = 1;
        quantiseMode = 0;
        nCircles = 8;
        circXCorse = 0;
    }
};

//—-----------------------------------------------------------------------------------------------
// 7) Shared DRAM Allocation & Initialization
//—-----------------------------------------------------------------------------------------------

static const int SHARED_DRAM_BYTES = 256;

static float (*sharedVerts)[3] = nullptr;

void calculateStaticRequirements(_NT_staticRequirements& req) {
    req.dram = SHARED_DRAM_BYTES;
}

void initialise(_NT_staticMemoryPtrs& ptrs, const _NT_staticRequirements& /*req*/) {
    uint8_t* dram = ptrs.dram;
    sharedVerts = reinterpret_cast<float(*)[3]>(dram);
    memcpy(sharedVerts, rawCubeVerts, sizeof(rawCubeVerts));

    float maxL = 0.0f;
    for (int i = 0; i < 8; ++i) {
        float x = sharedVerts[i][0];
        float y = sharedVerts[i][1];
        float z = sharedVerts[i][2];
        float L = sqrtf(x*x + y*y + z*z);
        if (L > maxL) maxL = L;
    }
    if (maxL == 0.0f) maxL = 1.0f;
    float invL = 1.0f / maxL;
    for (int i = 0; i < 8; ++i) {
        sharedVerts[i][0] *= invL;
        sharedVerts[i][1] *= invL;
        sharedVerts[i][2] *= invL;
    }
}

//—-----------------------------------------------------------------------------------------------
// 8) Per-Instance Memory Requirements
//—-----------------------------------------------------------------------------------------------

void calculateRequirements(_NT_algorithmRequirements& req, const int32_t* /*specs*/) {
    req.numParameters = sizeof(allParams) / sizeof(allParams[0]);
    req.sram = 4096;
    req.dram = 0;
    req.dtc  = 0;
    req.itc  = 0;
}

//—-----------------------------------------------------------------------------------------------
// 9) Utility: Normalize a 3‐vector
//—-----------------------------------------------------------------------------------------------

static void normalize3(float* v) {
    float L = sqrtf(v[0]*v[0] + v[1]*v[1] + v[2]*v[2]);
    if (L > 0.0f) {
        v[0] /= L;
        v[1] /= L;
        v[2] /= L;
    }
}

static inline float oscWave(int type, float phase) {
    phase -= floorf(phase);
    switch (type) {
        case 0: // Square
            return (phase < 0.5f) ? 1.0f : -1.0f;
        case 1: // Triangle
            return (phase < 0.5f) ? (4.0f*phase-1.0f) : (3.0f-4.0f*phase);
        case 2: // Saw
            return 1.0f - 2.0f*phase;
        case 3: // Ramp
            return 2.0f*phase - 1.0f;
        case 4: // Sine
        default:
            return sinf(2.0f * 3.14159265f * phase);
    }
}

static inline float getCourseFactor(int idx) {
    if (idx == 0) return 0.25f;
    if (idx == 1) return 1.0f / 3.0f;
    if (idx == 2) return 0.5f;
    if (idx == 3) return 1.0f;
    return static_cast<float>(idx - 2);
}

//—-----------------------------------------------------------------------------------------------
// 10) ParameterChanged
//—-----------------------------------------------------------------------------------------------

void parameterChanged(_NT_algorithm* baseSelf, int p) {
    PolyInstance* inst = reinterpret_cast<PolyInstance*>(baseSelf);
    int16_t raw;
    switch (p) {
        case 0: // Frequency
            raw = inst->v[0];
            inst->freq_Hz = static_cast<float>(raw);
            break;
        case 1: // RotX
            raw = inst->v[1];
            {
                float r = raw * (3.14159265f / 180.0f);
                inst->sinX = sinf(r);
                inst->cosX = cosf(r);
            }
            break;
        case 2: // RotY
            raw = inst->v[2];
            {
                float r = raw * (3.14159265f / 180.0f);
                inst->sinY = sinf(r);
                inst->cosY = cosf(r);
            }
            break;
        case 3: // RotZ
            raw = inst->v[3];
            {
                float r = raw * (3.14159265f / 180.0f);
                inst->sinZ = sinf(r);
                inst->cosZ = cosf(r);
            }
            break;
        case 4: // Distance
            raw = inst->v[4];
            {
                float d = raw * 0.01f;
                if (d < 0.111f) d = 0.111f;
                inst->cameraDist = d;
            }
            break;
        case 5: // Projection
            raw = inst->v[5];
            inst->projectionMode = (raw != 0 ? 1 : 0);
            break;
        case 6: // Polarity
            raw = inst->v[6];
            inst->polarity = raw;
            break;
        case 7: // Size
            raw = inst->v[7];
            inst->size = static_cast<float>(raw) / 100.0f; // 0.0 to 1.0
            break;
        case 8: // X Out
            raw = inst->v[8];
            inst->xOutBus = raw;
            break;
        case 9: // Y Out
            raw = inst->v[9];
            inst->yOutBus = raw;
            break;
        case 10: // Int Out
            raw = inst->v[10];
            inst->iOutBus = raw;
            break;
        case 11: // BlankWindow
            raw = inst->v[11];
            inst->blankWindow_us = static_cast<float>(raw);
            break;
        case 12: // BlankPhase
            raw = inst->v[12];
            inst->blankPhase_us = static_cast<float>(raw);
            break;
        case 13: // Resolution
            raw = inst->v[13];
            inst->resolution = raw;
            break;
        case 14: // AmpMod
            raw = inst->v[14];
            inst->ampModAmt = static_cast<float>(raw) / 127.0f;
            break;
        case 15: // AmpCorse
            raw = inst->v[15];
            inst->ampCorseIdx = raw;
            break;
        case 16: // AmpFine
            raw = inst->v[16];
            inst->ampFine = raw;
            break;
        case 17: // AmpWave
            raw = inst->v[17];
            inst->ampWave = raw;
            break;
        case 18: // AmpPhase
            raw = inst->v[18];
            inst->ampPhaseOffset = static_cast<float>(raw) / 360.0f;
            break;
        case 19: // X Offset
            raw = inst->v[19];
            inst->offsetX = static_cast<float>(raw) * 0.01f;
            break;
        case 20: // Y Offset
            raw = inst->v[20];
            inst->offsetY = static_cast<float>(raw) * 0.01f;
            break;
        case 21: // Z Offset
            raw = inst->v[21];
            inst->offsetZ = static_cast<float>(raw) * 0.01f;
            break;
        case 22: // XoffMod
            raw = inst->v[22];
            inst->xModAmt = static_cast<float>(raw) / 100.0f;
            break;
        case 23: // YoffMod
            raw = inst->v[23];
            inst->yModAmt = static_cast<float>(raw) / 100.0f;
            break;
        case 24: // ZoffMod
            raw = inst->v[24];
            inst->zModAmt = static_cast<float>(raw) / 100.0f;
            break;
        case 25: // OffCorse
            raw = inst->v[25];
            inst->offCorseIdx = raw;
            break;
        case 26: // OffFine
            raw = inst->v[26];
            inst->offFine = raw;
            break;
        case 27: // OffWave
            raw = inst->v[27];
            inst->offWave = raw;
            break;
        case 28: // OffPhase
            raw = inst->v[28];
            inst->offPhaseOffset = static_cast<float>(raw) / 360.0f;
            break;
        case 29: // NPM
            raw = inst->v[29];
            inst->npm = static_cast<float>(raw) / 100.0f; // 0.0 to 1.0
            break;
        case 30: // NpmCurve
            raw = inst->v[30];
            inst->npmCurve = static_cast<float>(raw) / 100.0f; // 0.0 to 1.0
            break;
        case 31: // Solid
            raw = inst->v[31];
            inst->solidType = raw;
            break;
        case 32: // Quantise
            raw = inst->v[32];
            inst->quantiseMode = raw;
            break;
        case 33: // NCircles
            raw = inst->v[33];
            inst->nCircles = raw;
            break;
        case 34: // CircXCorse
            raw = inst->v[34];
            inst->circXCorse = raw;
            break;
    }
}

//—-----------------------------------------------------------------------------------------------
// 11) Construct Algorithm Instance
//—-----------------------------------------------------------------------------------------------

_NT_algorithm* constructAlgorithm(const _NT_algorithmMemoryPtrs& ptrs,
                                  const _NT_algorithmRequirements& /*req*/,
                                  const int32_t* /*specs*/) {
    void* sramBase = ptrs.sram;
    PolyInstance* inst = new (sramBase) PolyInstance();
    inst->parameters       = allParams;
    inst->parameterPages   = &parameterPages;
    return reinterpret_cast<_NT_algorithm*>(inst);
}

//—-----------------------------------------------------------------------------------------------
// 12) Audio‐Rate step: Draw Eulerian cycle (no culling)
//—-----------------------------------------------------------------------------------------------

// Quantisation
static const Scale* getScaleByQuantiseIdx(int idx) {
    switch (idx) {
        case 1:  return &scaleChromatic;
        case 2:  return &scaleMajor;
        case 3:  return &scaleMinor;
        case 4:  return &scaleHarmonicMinor;
        case 5:  return &scaleMajorPentatonic;
        case 6:  return &scaleMinorPentatonic;
        case 7:  return &scaleHirajoshi;
        case 8:  return &scaleGamelan;
        case 9:  return &scaleFifths;
        case 10: return &scaleDim5th;
        default: return nullptr; // 0 = Unquantised
    }
}

static float quantiseFrequency(float freq, int quantiseMode) {
    if (quantiseMode == 0) return freq; // Unquantised

    const Scale* scale = getScaleByQuantiseIdx(quantiseMode);
    if (!scale) return freq;

    // Map Hz to 0-5V (1V/oct, 440Hz = A4 = 3.6667V)
    float v = logf(freq / 440.0f) / logf(2.0f) + 3.6666667f;
    Quantizer quant;
    quant.scale = *scale;
    quant.minCV = 0.0f;
    quant.maxCV = 5.0f;
    float quantV = quant.quantize(v);
    float quantFreq = 440.0f * powf(2.0f, quantV - 3.6666667f);
    return quantFreq;
}

void step(_NT_algorithm* baseSelf, float* busFrames, int numFramesBy4) {
    PolyInstance* inst = reinterpret_cast<PolyInstance*>(baseSelf);

    // Use instance parameters directly
    float freq_Hz        = inst->freq_Hz;
    float rotX           = inst->v[1];
    float rotY           = inst->v[2];
    float rotZ           = inst->v[3];
    float cameraDist     = inst->cameraDist;
    float size           = inst->size;
    float blankWindow_us = inst->blankWindow_us;
    float blankPhase_us  = inst->blankPhase_us;
    float resolution     = float(inst->resolution);
    float ampModAmt      = inst->ampModAmt;
    int   ampCorseIdx    = inst->ampCorseIdx;
    int   ampFine        = inst->ampFine;
    int   ampWave        = inst->ampWave;
    float ampPhaseOffset = inst->ampPhaseOffset;
    float offsetX        = inst->offsetX;
    float offsetY        = inst->offsetY;
    float offsetZ        = inst->offsetZ;
    float xModAmt        = inst->xModAmt;
    float yModAmt        = inst->yModAmt;
    float zModAmt        = inst->zModAmt;
    int   offCorseIdx    = inst->offCorseIdx;
    int   offFine        = inst->offFine;
    int   offWave        = inst->offWave;
    float offPhaseOffset = inst->offPhaseOffset;
    int   solidType      = inst->solidType;
    int   nCircles       = inst->nCircles;

    // Use rotation angles directly
    float rotX_rad = rotX * (3.14159265f / 180.0f);
    float rotY_rad = rotY * (3.14159265f / 180.0f);
    float rotZ_rad = rotZ * (3.14159265f / 180.0f);
    float sinX = sinf(rotX_rad), cosX = cosf(rotX_rad);
    float sinY = sinf(rotY_rad), cosY = cosf(rotY_rad);
    float sinZ = sinf(rotZ_rad), cosZ = cosf(rotZ_rad);

    // --- Select geometry based on solidType ---
    const float* verts = nullptr;
    int numVerts = 0;
    const Segment* segments = nullptr;
    int numSegs = 0;

    switch (solidType) {
        case 0: { // Horizontal Lines
            int numLines = inst->nCircles;
            if (numLines < 1) numLines = 1;
            if (numLines > 32) numLines = 32;
            
            float lineSpacing = 2.0f / (numLines + 1);
            
            // Increase line width to take advantage of full ±12V range
            // Lines will extend from -2.0 to +2.0 in coordinate space
            // After 5.0f scaling: -10V to +10V (well within ±12V range)
            float lineWidth = 2.0f;
            
            // Create vertices for horizontal lines
            static float dynamicVerts[64 * 3];
            static Segment dynamicSegments[64];
            
            for (int line = 0; line < numLines; line++) {
                float y = -1.0f + (line + 1) * lineSpacing;
                int vertIdx = line * 2;
                
                dynamicVerts[vertIdx * 3 + 0] = -lineWidth;
                dynamicVerts[vertIdx * 3 + 1] = y;
                dynamicVerts[vertIdx * 3 + 2] = 0.0f;
                
                dynamicVerts[(vertIdx + 1) * 3 + 0] = lineWidth;
                dynamicVerts[(vertIdx + 1) * 3 + 1] = y;
                dynamicVerts[(vertIdx + 1) * 3 + 2] = 0.0f;
                
                dynamicSegments[line].a = vertIdx;
                dynamicSegments[line].b = vertIdx + 1;
                dynamicSegments[line].draw = 1;
            }
            
            verts = dynamicVerts;
            numVerts = numLines * 2;
            segments = dynamicSegments;
            numSegs = numLines;
            break;
        }
        case 1: // Cube
            verts = &rawCubeVerts[0];
            numVerts = 8;
            segments = cubeSegments;
            numSegs = sizeof(cubeSegments)/sizeof(cubeSegments[0]);
            break;
        case 2: // Octahedron
            verts = &octaVerts[0][0];
            numVerts = 6;
            segments = octaPath;
            numSegs = sizeof(octaPath)/sizeof(octaPath[0]);
            break;
        case 3: { // Circles
            int numCircles;
            if (inst->circXCorse == 0) {
                float offCourseFac = getCourseFactor(offCorseIdx);
                numCircles = static_cast<int>(offCourseFac * nCircles + 0.5f);
            } else {
                numCircles = nCircles;
            }
            if (numCircles < 1) numCircles = 1;

            // Fix: Create different radii for concentric circles
            float minR = 0.1f, maxR = 0.5f; // Different min/max for concentric effect
            int   numFrames = numFramesBy4 * 4;
            float fs        = static_cast<float>(NT_globals.sampleRate);
            float freq      = quantiseFrequency(freq_Hz, inst->quantiseMode);

            int busXidx = inst->xOutBus;
            int busYidx = inst->yOutBus;
            int busIidx = inst->iOutBus;
            float* busX = busFrames + busXidx * numFrames;
            float* busY = busFrames + busYidx * numFrames;
            float* busI = busFrames + busIidx * numFrames;

            float phase = inst->phase;
            float ampPhase = inst->ampPhase;
            float offPhase = inst->offPhase;

            float circleFrac = 1.0f / numCircles;

            for (int i = 0; i < numFrames; ++i) {
                float p = phase;
                int circleIdx = static_cast<int>(p / circleFrac);
                if (circleIdx >= numCircles) circleIdx = numCircles - 1; // Safety clamp
                float localPhase = (p - circleIdx * circleFrac) / circleFrac;

                float Iout = 5.0f;
                
                // Fix: Calculate radius based on circle index for concentric circles
                float r;
                if (numCircles == 1) {
                    r = maxR; // Single circle at max radius
                } else {
                    r = minR + (maxR - minR) * (float)circleIdx / (numCircles - 1);
                }

                float theta = localPhase * 2.0f * 3.14159265f;
                float Xr = r * cosf(theta);
                float Yr = r * sinf(theta);
                float Zr = 0.0f;
                
                // Rotation
                float Y1 = cosX * Yr - sinX * Zr;
                float Z1 = sinX * Yr + cosX * Zr;
                float X1 = Xr;
                float X2 = cosY * X1 + sinY * Z1;
                float Z2 = -sinY * X1 + cosY * Z1;
                float Y2 = Y1;
                Xr = cosZ * X2 - sinZ * Y2;
                Yr = sinZ * X2 + cosZ * Y2;
                Zr = Z2;

                // Offset modulation oscillator
                float offCourseFac = getCourseFactor(offCorseIdx);
                float offFreqBase  = freq * offCourseFac;
                float offFreq      = offFreqBase + (static_cast<float>(offFine) * 0.1f);
                if (offFreq < 0.0f) offFreq = 0.0f;
                float offVal = oscWave(offWave, offPhase + offPhaseOffset);

                // Apply offset and offset modulation
                Xr += offsetX + xModAmt * offVal;
                Yr += offsetY + yModAmt * offVal;
                Zr += offsetZ + zModAmt * offVal;

                // Size scaling
                Xr *= size;
                Yr *= size;
                Zr *= size;

                // Resolution quantisation
                if (resolution > 0) {
                    float invRes = 101.0f - resolution;
                    float scaleQ = invRes * 0.5f;
                    Xr = roundf((Xr + 1.0f) * scaleQ) / scaleQ - 1.0f;
                    Yr = roundf((Yr + 1.0f) * scaleQ) / scaleQ - 1.0f;
                    Zr = roundf((Zr + 1.0f) * scaleQ) / scaleQ - 1.0f;
                }

                // Projection
                float Xv, Yv;
                if (inst->projectionMode == 1) {
                    if (inst->polarity == 0) {
                        float dcam = Zr + cameraDist;
                        if (dcam == 0.0f) dcam = 0.0001f;
                        float scale = cameraDist / dcam;
                        Xv = 5.0f * (Xr * scale);
                        Yv = 5.0f * (Yr * scale);
                    } else {
                        float dcam = Zr + cameraDist;
                        float scale = dcam / cameraDist;
                        Xv = 5.0f * (Xr * scale);
                        Yv = 5.0f * (Yr * scale);
                    }
                } else {
                    Xv = 5.0f * Xr;
                    Yv = 5.0f * Yr;
                }

                // Amplitude modulation
                float ampVal = oscWave(ampWave, ampPhase + ampPhaseOffset);
                float ampMul = 1.0f + ampModAmt * ampVal;
                float outX = Xv * ampMul;
                float outY = Yv * ampMul;

                busX[i] = outX;
                busY[i] = outY;
                busI[i] = Iout;

                // Advance phase and ampPhase and offPhase
                phase += (freq / fs);
                if (phase >= 1.0f) phase -= 1.0f;

                ampPhase += (freq * getCourseFactor(ampCorseIdx) + (static_cast<float>(ampFine) * 0.1f)) / fs;
                if (ampPhase >= 1.0f) ampPhase -= floorf(ampPhase);

                offPhase += (offFreq) / fs;
                if (offPhase >= 1.0f) offPhase -= floorf(offPhase);
            }
            inst->phase = phase;
            inst->ampPhase = ampPhase;
            inst->offPhase = offPhase;
            return;
        }
        default: // fallback to cube
            verts = &rawCubeVerts[0];
            numVerts = 8;
            segments = cubeSegments;
            numSegs = sizeof(cubeSegments)/sizeof(cubeSegments[0]);
            break;
    }

    int numFrames = numFramesBy4 * 4;
    float fs        = static_cast<float>(NT_globals.sampleRate);
    float baseFreq  = quantiseFrequency(freq_Hz, inst->quantiseMode);
    int   eLen      = numSegs;

    // Adjust frequency to maintain consistent individual line drawing speed
    float freq;
    if (solidType == 0) { // Horizontal Lines - scale frequency inversely by number of lines
        freq = baseFreq / static_cast<float>(eLen);
    } else { // Other solids - use normal frequency
        freq = baseFreq;
    }

    // Output buses
    int busXidx = inst->xOutBus;
    int busYidx = inst->yOutBus;
    int busIidx = inst->iOutBus;
    float* busX = busFrames + busXidx * numFrames;
    float* busY = busFrames + busYidx * numFrames;
    float* busI = busFrames + busIidx * numFrames;

    // Blanking - corrected to use baseFreq for horizontal lines to maintain consistent blanking
    const float freqRef = 50.0f;
    float blankFrac, shiftFrac;

    if (solidType == 0) { // Horizontal Lines - use baseFreq for blanking calculations
        blankFrac = blankWindow_us * 1e-6f * baseFreq;
        if (blankFrac > 0.5f) blankFrac = 0.5f;
        shiftFrac = blankPhase_us * 1e-6f * baseFreq;
    } else { // Other solids - use normal blanking
        blankFrac = blankWindow_us * 1e-6f * freqRef * static_cast<float>(eLen);
        if (blankFrac > 0.5f) blankFrac = 0.5f;
        shiftFrac = blankPhase_us * 1e-6f * freqRef * static_cast<float>(eLen);
    }

    float ampCourseFac = getCourseFactor(ampCorseIdx);
    float ampFreqBase  = freq * ampCourseFac;
    float ampFreq      = ampFreqBase + (static_cast<float>(ampFine) * 0.1f);
    if (ampFreq < 0.0f) ampFreq = 0.0f;
    float ampPhase     = inst->ampPhase;

    float phase = inst->phase;
    float offPhase = inst->offPhase;

    for (int i = 0; i < numFrames; ++i) {
        float ampVal = oscWave(ampWave, ampPhase + ampPhaseOffset);
        float ampMul = 1.0f + ampModAmt * ampVal;

        float ePos = phase * static_cast<float>(eLen);
        int   idx  = static_cast<int>(floorf(ePos));
        if (idx >= eLen) idx = eLen - 1;
        float frac = ePos - static_cast<float>(idx);

        // Nonlinear Phase Mapping (NPM)
        float effectiveNpm, effectiveNpmCurve;
        if (solidType == 0) { // Horizontal Lines - use fixed NPM settings
            effectiveNpm = 1.0f;      // 100%
            effectiveNpmCurve = 0.5f; // 50%
        } else { // Other solids - use parameter values
            effectiveNpm = inst->npm;
            effectiveNpmCurve = inst->npmCurve;
        }

        if (effectiveNpm > 0.0f) {
            float x = frac;
            float curve = effectiveNpmCurve;
            float shaped;
            if (curve < 0.5f) {
                float k = 1.0f - 2.0f * curve;
                shaped = powf(x, 1.0f + k * 30.0f);
            } else if (curve > 0.5f) {
                float k = 2.0f * (curve - 0.5f);
                shaped = 1.0f - powf(1.0f - x, 1.0f + k * 4.0f);
            } else {
                shaped = x;
            }
            frac = (1.0f - effectiveNpm) * x + effectiveNpm * shaped;
        }

        float fShift = frac + shiftFrac;
        if (fShift <  0.0f) fShift += 1.0f;
        if (fShift >= 1.0f) fShift -= 1.0f;

        const Segment& seg = segments[idx];
        uint8_t vA = seg.a;
        uint8_t vB = seg.b;

        // Offset Modulation Oscillator
        float offCourseFac = getCourseFactor(offCorseIdx);
        float offFreqBase  = freq * offCourseFac;
        float offFreq      = offFreqBase + (static_cast<float>(offFine) * 0.1f);
        if (offFreq < 0.0f) offFreq = 0.0f;
        float offVal = oscWave(offWave, offPhase + offPhaseOffset);

        // Interpolate endpoints:
        float Ax = verts[3*vA+0];
        float Ay = verts[3*vA+1];
        float Az = verts[3*vA+2];
        float Bx = verts[3*vB+0];
        float By = verts[3*vB+1];
        float Bz = verts[3*vB+2];
        float Px = (1.0f - frac)*Ax + frac*Bx;
        float Py = (1.0f - frac)*Ay + frac*By;
        float Pz = (1.0f - frac)*Az + frac*Bz;

        // Rotate around X:
        float Y1 = cosX * Py - sinX * Pz;
        float Z1 = sinX * Py + cosX * Pz;
        float X1 = Px;
        // Rotate around Y:
        float X2 = cosY * X1 + sinY * Z1;
        float Z2 = -sinY * X1 + cosY * Z1;
        float Y2 = Y1;
        // Rotate around Z:
        float Xr = cosZ * X2 - sinZ * Y2;
        float Yr = sinZ * X2 + cosZ * Y2;
        float Zr = Z2;
        // Offset
        Xr += offsetX + xModAmt * offVal;
        Yr += offsetY + yModAmt * offVal;
        Zr += offsetZ + zModAmt * offVal;

        // Size scaling
        Xr *= size;
        Yr *= size;
        Zr *= size;

        if (resolution > 0) {
            float invRes = 101.0f - resolution;
            float scaleQ = invRes * 0.5f;
            Xr = roundf((Xr + 1.0f) * scaleQ) / scaleQ - 1.0f;
            Yr = roundf((Yr + 1.0f) * scaleQ) / scaleQ - 1.0f;
            Zr = roundf((Zr + 1.0f) * scaleQ) / scaleQ - 1.0f;
        }

        // Project:
        float Xv, Yv;
        if (inst->projectionMode == 1) {
            if (inst->polarity == 0) {
                float dcam = Zr + cameraDist;
                if (dcam == 0.0f) dcam = 0.0001f;
                float scale = cameraDist / dcam;
                Xv = 5.0f * (Xr * scale);
                Yv = 5.0f * (Yr * scale);
            } else {
                float dcam = Zr + cameraDist;
                float scale = dcam / cameraDist;
                Xv = 5.0f * (Xr * scale);
                Yv = 5.0f * (Yr * scale);
            }
        } else {
            Xv = 5.0f * Xr;
            Yv = 5.0f * Yr;
        }

        float outX = Xv * ampMul;
        float outY = Yv * ampMul;

        float Iout = (seg.draw ? 5.0f : 0.0f);

        // Standard blanking
        if (seg.draw && ((fShift < blankFrac) || (fShift > (1.0f - blankFrac)))) {
            Iout = 0.0f;
        }

        busX[i] = outX;
        busY[i] = outY;
        busI[i] = Iout;

        // Advance phase and ampPhase once per output sample
        phase += (freq / fs);
        if (phase >= 1.0f) phase -= 1.0f;

        ampPhase += (ampFreq / fs);
        if (ampPhase >= 1.0f) ampPhase -= floorf(ampPhase);

        offPhase += (getCourseFactor(offCorseIdx) * freq + (static_cast<float>(offFine) * 0.1f)) / fs;
        if (offPhase >= 1.0f) offPhase -= floorf(offPhase);
    }
    inst->phase     = phase;
    inst->ampPhase  = ampPhase;
    inst->offPhase = offPhase;
}

//—-----------------------------------------------------------------------------------------------
// 13) Factory Definition & pluginEntry
//—-----------------------------------------------------------------------------------------------

static const _NT_factory polyFactory = {
    .guid                        = NT_MULTICHAR('P','O','L','Y'),
    .name                        = "CubeWireNoCull",
    .description                 = "Wireframe cube (no culling)",
    .numSpecifications           = 0,
    .specifications              = nullptr,
    .calculateStaticRequirements = calculateStaticRequirements,
    .initialise                  = initialise,
    .calculateRequirements       = calculateRequirements,
    .construct                   = constructAlgorithm,
    .parameterChanged            = parameterChanged,
    .step                        = step,
    .draw                        = nullptr,
    .midiRealtime                = nullptr,
    .midiMessage                 = nullptr,
    .tags                        = kNT_tagUtility,
    .hasCustomUi                 = nullptr,
    .customUi                    = nullptr,
    .setupUi                     = nullptr
};

extern "C" uintptr_t pluginEntry(_NT_selector selector, uint32_t data) {
    switch (selector) {
        case kNT_selector_version:
            return kNT_apiVersion5;
        case kNT_selector_numFactories:
            return 1;
        case kNT_selector_factoryInfo:
            if (data == 0) return reinterpret_cast<uintptr_t>(&polyFactory);
            return 0;
        default:
            return 0;
    }
}

