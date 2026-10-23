// test_plugin_host.cpp — desktop end-to-end host harness for the full
// plugin (Implementation Plan Step 15). Simulates what the disting NT
// firmware does: stubs the firmware-provided API symbols, reads the
// factory via pluginEntry, allocates the requested SRAM/DRAM, constructs
// the algorithm, drives step() until a kit finishes baking, and verifies
// the player produces bounded, non-silent audio on the selected bus.
//
// This cannot run the ARM .o, but it validates the INTEGRATION LOGIC
// (parameter wiring, render-advance-per-step throttling, the player's
// rate-converted loop read, Play/Output/mode handling) against the exact
// factory entry points the firmware uses.
//
// Build: g++ -std=c++11 -O2 -I. -I../include -o test_plugin_host test_plugin_host.cpp 4x4.cpp
#include "distingnt/api.h"
#include <cstdio>
#include <cmath>
#include <cstdint>
#include <cstdlib>

// ---- Firmware-side stubs the plugin links against ----
const _NT_globals NT_globals = { 48000u, 256u, nullptr, 0u, 0u, 0u };

static int16_t* g_vArray = nullptr;   // the host-owned parameter value array

int32_t NT_algorithmIndex( const _NT_algorithm* ) { return 0; }
uint32_t NT_parameterOffset( void ) { return 0; }
void NT_setParameterFromAudio( uint32_t /*idx*/, uint32_t parameter, int16_t value )
{
    if ( g_vArray ) g_vArray[parameter] = value;   // honor the Regenerate auto-reset
}
// A few other API symbols the headers may reference but this path doesn't call:
void NT_setParameterFromUi( uint32_t, uint32_t, int16_t ) {}

extern "C" uintptr_t pluginEntry( _NT_selector selector, uint32_t data );

static int g_failures = 0;
static void check( bool cond, const char* what ) { printf( "  %s: %s\n", cond ? "ok" : "FAIL", what ); if ( !cond ) ++g_failures; }

int main()
{
    const _NT_factory* f = (const _NT_factory*)pluginEntry( kNT_selector_factoryInfo, 0 );
    check( f != nullptr, "pluginEntry returns a factory" );
    if ( !f ) return 1;
    printf( "Plugin: \"%s\" — %s\n", f->name, f->description );
    check( (uintptr_t)pluginEntry( kNT_selector_version, 0 ) == (uintptr_t)kNT_apiVersionCurrent, "reports current API version" );
    check( (uintptr_t)pluginEntry( kNT_selector_numFactories, 0 ) == 1, "reports one factory" );

    _NT_algorithmRequirements req = {};
    f->calculateRequirements( req, nullptr );
    printf( "Requirements: numParameters=%u sram=%u dram=%u dtc=%u itc=%u\n",
            req.numParameters, req.sram, req.dram, req.dtc, req.itc );
    check( req.dram > 0 && req.dram <= 512u * 1024u, "DRAM request is within the 512KB pool" );

    // Allocate the memory blocks the firmware would provide.
    _NT_algorithmMemoryPtrs ptrs = {};
    ptrs.sram = (uint8_t*)calloc( 1, req.sram ? req.sram : 1 );
    ptrs.dram = (uint8_t*)calloc( 1, req.dram ? req.dram : 1 );
    ptrs.dtc  = (uint8_t*)calloc( 1, req.dtc ? req.dtc : 1 );
    ptrs.itc  = (uint8_t*)calloc( 1, req.itc ? req.itc : 1 );

    _NT_algorithm* alg = f->construct( ptrs, req, nullptr );
    check( alg != nullptr, "construct() returns an algorithm" );
    check( alg->parameters != nullptr, "construct() populated parameters" );
    check( alg->parameterPages != nullptr, "construct() populated parameterPages" );

    // Build the v[] value array from parameter defaults, as the firmware does.
    g_vArray = (int16_t*)calloc( req.numParameters, sizeof( int16_t ) );
    for ( uint32_t i = 0; i < req.numParameters; ++i ) g_vArray[i] = alg->parameters[i].def;
    alg->v = g_vArray;

    // Find the Output bus parameter (it's the one with unit kNT_unitAudioOutput).
    int outParam = -1;
    for ( uint32_t i = 0; i < req.numParameters; ++i )
        if ( alg->parameters[i].unit == kNT_unitAudioOutput ) { outParam = (int)i; break; }
    check( outParam >= 0, "found the Output bus parameter" );
    int outBus = g_vArray[outParam];
    printf( "Output bus = %d (param index %d)\n", outBus, outParam );

    // Drive step() with a 64-bus x N-frame bus buffer, as the firmware would.
    const int kNumFramesBy4 = 6;            // 24 frames, the default buffer size
    const int n = kNumFramesBy4 * 4;
    const int kNumBuses = 64;
    static float busFrames[64 * 24];

    // Run long enough to complete a full kit bake plus some playback. A kit
    // is ~147204 kit-samples -> ~220806 internal samples at 48k -> ~13800
    // chunks (one advanceChunk per step at kChunkSize=16). Run 20000 steps.
    const int kSteps = 20000;
    float maxOutDuringBake = 0.0f, maxOutAfter = 0.0f;
    bool anyNaN = false;
    for ( int s = 0; s < kSteps; ++s )
    {
        // Zero the bus each call (firmware doesn't, but output buses start
        // at zero each block for the first algorithm — §1 signal flow).
        for ( int i = 0; i < kNumBuses * n; ++i ) busFrames[i] = 0.0f;
        f->step( alg, busFrames, kNumFramesBy4 );
        float* out = busFrames + ( outBus - 1 ) * n;
        for ( int i = 0; i < n; ++i )
        {
            float v = out[i];
            if ( std::isnan( v ) ) anyNaN = true;
            float a = fabsf( v );
            if ( s < 2000 ) { if ( a > maxOutDuringBake ) maxOutDuringBake = a; }
            else            { if ( a > maxOutAfter ) maxOutAfter = a; }
        }
    }
    printf( "After %d steps: max|out| early=%.4f, later=%.4f\n", kSteps, maxOutDuringBake, maxOutAfter );
    check( !anyNaN, "no NaN on the output bus across the whole run" );
    check( maxOutAfter <= 11.0f + 1e-3f, "player output stays within +-11V" );
    check( maxOutAfter > 0.05f, "player produces non-silent audio once a kit has baked" );

    // --- Play=Off should silence the output (Replace mode). ---
    {
        int playParam = -1;
        for ( uint32_t i = 0; i < req.numParameters; ++i )
            if ( alg->parameters[i].enumStrings && alg->parameters[i].enumStrings[0] &&
                 alg->parameters[i].enumStrings[0][0] == 'O' && alg->parameters[i].enumStrings[0][1] == 'f' )
                { playParam = (int)i; break; }   // "Off"/"On" strings => Play
        check( playParam >= 0, "found the Play parameter" );
        g_vArray[playParam] = 0;                 // Play = Off
        g_vArray[outParam + 1] = 1;              // Output mode = Replace (next param after bus)
        float maxOut = 0.0f;
        for ( int s = 0; s < 10; ++s )
        {
            for ( int i = 0; i < kNumBuses * n; ++i ) busFrames[i] = 0.0f;
            f->step( alg, busFrames, kNumFramesBy4 );
            float* out = busFrames + ( outBus - 1 ) * n;
            for ( int i = 0; i < n; ++i ) if ( fabsf( out[i] ) > maxOut ) maxOut = fabsf( out[i] );
        }
        check( maxOut < 1e-6f, "Play=Off in Replace mode produces exact silence" );
    }

    // --- Regenerate trigger: now handled by step() edge-detection (not
    //     parameterChanged, which is NULL). Set it to 1, run one step(),
    //     confirm step() auto-resets it to Hold — and that it re-arms. ---
    {
        int regenParam = -1;
        for ( uint32_t i = 0; i < req.numParameters; ++i )
            if ( alg->parameters[i].enumStrings && alg->parameters[i].enumStrings[0] &&
                 alg->parameters[i].enumStrings[0][0] == 'H' )   // "Hold"
                { regenParam = (int)i; break; }
        check( regenParam >= 0, "found the Regenerate parameter" );
        check( f->parameterChanged == NULL, "parameterChanged is NULL (all params read live in step())" );

        g_vArray[regenParam] = 1;
        for ( int i = 0; i < kNumBuses * n; ++i ) busFrames[i] = 0.0f;
        f->step( alg, busFrames, kNumFramesBy4 );
        check( g_vArray[regenParam] == 0, "Regenerate auto-resets to Hold after one step() (momentary trigger fires)" );

        // Re-arm: a fresh Hold->Regenerate must trigger again.
        g_vArray[regenParam] = 1;
        for ( int i = 0; i < kNumBuses * n; ++i ) busFrames[i] = 0.0f;
        f->step( alg, busFrames, kNumFramesBy4 );
        check( g_vArray[regenParam] == 0, "Regenerate re-triggers on a second Hold->Regenerate (edge re-arms)" );
    }

    // --- Contour settings page: the 5 global settings params exist, and
    //     changing one then regenerating produces a bounded, non-silent,
    //     non-NaN kit (the reconfigure path works end-to-end). ---
    {
        int windowsParam = -1, chaosParam = -1;
        for ( uint32_t i = 0; i < req.numParameters; ++i )
        {
            if ( alg->parameters[i].name[0] == 'W' && alg->parameters[i].name[1] == 'i' ) windowsParam = (int)i;  // "Windows"
            if ( alg->parameters[i].name[0] == 'C' && alg->parameters[i].name[1] == 'h' ) chaosParam = (int)i;    // "Chaos"
        }
        check( windowsParam >= 0, "found the Windows (Contour) parameter" );
        check( chaosParam >= 0, "found the Chaos (Contour) parameter" );

        // Make sure Play is On again and change the Contour settings.
        int playParam2 = -1;
        for ( uint32_t i = 0; i < req.numParameters; ++i )
            if ( alg->parameters[i].enumStrings && alg->parameters[i].enumStrings[0] &&
                 alg->parameters[i].enumStrings[0][0] == 'O' && alg->parameters[i].enumStrings[0][1] == 'f' )
                { playParam2 = (int)i; break; }
        if ( playParam2 >= 0 ) g_vArray[playParam2] = 1;

        int regenParam2 = -1;
        for ( uint32_t i = 0; i < req.numParameters; ++i )
            if ( alg->parameters[i].enumStrings && alg->parameters[i].enumStrings[0] &&
                 alg->parameters[i].enumStrings[0][0] == 'H' )   // "Hold"
                { regenParam2 = (int)i; break; }

        if ( windowsParam >= 0 ) g_vArray[windowsParam] = 20;   // from default 6
        if ( chaosParam >= 0 )   g_vArray[chaosParam]   = 90;   // from default 60
        if ( regenParam2 >= 0 )  g_vArray[regenParam2]  = 1;    // trigger a bake with the new settings

        bool anyNaN = false; float maxOut = 0.0f;
        for ( int s = 0; s < 18000; ++s )
        {
            for ( int i = 0; i < kNumBuses * n; ++i ) busFrames[i] = 0.0f;
            f->step( alg, busFrames, kNumFramesBy4 );
            float* out = busFrames + ( outBus - 1 ) * n;
            for ( int i = 0; i < n; ++i )
            {
                if ( std::isnan( out[i] ) ) anyNaN = true;
                if ( s > 16000 && fabsf( out[i] ) > maxOut ) maxOut = fabsf( out[i] );
            }
        }
        check( !anyNaN, "changed Contour settings + regenerate: no NaN across the new bake" );
        check( maxOut <= 11.0f + 1e-3f, "changed Contour settings: output stays within +-11V" );
        check( maxOut > 0.05f, "changed Contour settings: new kit is non-silent" );
    }

    // --- Processors page: the 4 enable params exist; disabling all four and
    //     regenerating produces a silent kit (every channel muted). ---
    {
        // The 4 enable params are the Off/On enums named "P1 pitch".. — find
        // them by name prefix 'P' + digit + ' '.
        int enableParams[4] = { -1, -1, -1, -1 };
        int found = 0;
        for ( uint32_t i = 0; i < req.numParameters; ++i )
        {
            const char* nm = alg->parameters[i].name;
            if ( nm[0] == 'P' && nm[1] >= '1' && nm[1] <= '4' && nm[2] == ' ' &&
                 alg->parameters[i].enumStrings && alg->parameters[i].enumStrings[0] &&
                 alg->parameters[i].enumStrings[0][0] == 'O' )   // Off/On enum, not the % depths
            {
                int idx = nm[1] - '1';
                if ( enableParams[idx] < 0 ) { enableParams[idx] = (int)i; ++found; }
            }
        }
        check( found == 4, "found all 4 processor enable parameters" );

        int regenP = -1;
        for ( uint32_t i = 0; i < req.numParameters; ++i )
            if ( alg->parameters[i].enumStrings && alg->parameters[i].enumStrings[0] &&
                 alg->parameters[i].enumStrings[0][0] == 'H' ) { regenP = (int)i; break; }

        for ( int k = 0; k < 4; ++k ) if ( enableParams[k] >= 0 ) g_vArray[enableParams[k]] = 0;  // all off
        if ( regenP >= 0 ) g_vArray[regenP] = 1;   // bake with all processors off

        float maxOut = 0.0f;
        for ( int s = 0; s < 18000; ++s )
        {
            for ( int i = 0; i < kNumBuses * n; ++i ) busFrames[i] = 0.0f;
            f->step( alg, busFrames, kNumFramesBy4 );
            float* out = busFrames + ( outBus - 1 ) * n;
            for ( int i = 0; i < n; ++i ) if ( s > 16000 && fabsf( out[i] ) > maxOut ) maxOut = fabsf( out[i] );
        }
        check( maxOut < 1e-6f, "all processors off + regenerate -> kit is silent" );

        // Re-enable all processors for the oversampling check below.
        for ( int k = 0; k < 4; ++k ) if ( enableParams[k] >= 0 ) g_vArray[enableParams[k]] = 1;
    }

    // --- Oversample param exists; a 4x render runs end-to-end (the whole
    //     network at 192kHz + AA downsample) producing bounded, non-silent,
    //     NaN-free audio. ---
    {
        int osParam = -1, regenP = -1;
        for ( uint32_t i = 0; i < req.numParameters; ++i )
        {
            const char* const* es = alg->parameters[i].enumStrings;
            if ( es && es[0] && es[0][0] == '1' && es[0][1] == 'x' ) osParam = (int)i;   // "1x".."8x"
            if ( es && es[0] && es[0][0] == 'H' ) regenP = (int)i;                        // "Hold"
        }
        check( osParam >= 0, "found the Oversample parameter" );

        g_vArray[osParam] = 2;   // index 2 -> 4x
        if ( regenP >= 0 ) g_vArray[regenP] = 1;
        bool anyNaN = false; float maxOut = 0.0f;
        // A 4x bake needs ~4x the steps of 1x (~55k); run well past that.
        for ( int s = 0; s < 70000; ++s )
        {
            for ( int i = 0; i < kNumBuses * n; ++i ) busFrames[i] = 0.0f;
            f->step( alg, busFrames, kNumFramesBy4 );
            float* out = busFrames + ( outBus - 1 ) * n;
            for ( int i = 0; i < n; ++i )
            {
                if ( std::isnan( out[i] ) ) anyNaN = true;
                if ( s > 68000 && fabsf( out[i] ) > maxOut ) maxOut = fabsf( out[i] );
            }
        }
        check( !anyNaN, "4x oversampled render: no NaN end-to-end" );
        check( maxOut <= 11.0f + 1e-3f, "4x oversampled render: output stays within +-11V" );
        check( maxOut > 0.05f, "4x oversampled render: produces non-silent audio" );
        g_vArray[osParam] = 0;   // back to 1x for the quick Filters bake below
    }

    // --- Filters page: 4 bandpass Freq + 4 BW params exist; setting them (a
    //     narrow band) and regenerating produces a bounded, non-silent,
    //     NaN-free kit. ---
    {
        int nFreq = 0, nBw = 0, firstFreq = -1, firstBw = -1;
        for ( uint32_t i = 0; i < req.numParameters; ++i )
        {
            const char* nm = alg->parameters[i].name;
            if ( nm[0] == 'F' && nm[1] == 'r' && nm[2] == 'e' ) { if ( firstFreq < 0 ) firstFreq = (int)i; ++nFreq; }  // "Freq n"
            if ( nm[0] == 'B' && nm[1] == 'W' ) { if ( firstBw < 0 ) firstBw = (int)i; ++nBw; }                        // "BW n"
        }
        check( nFreq == 4, "found 4 bandpass Freq parameters (Filters page)" );
        check( nBw == 4, "found 4 bandpass BW parameters (Filters page)" );

        int regenP = -1;
        for ( uint32_t i = 0; i < req.numParameters; ++i )
            if ( alg->parameters[i].enumStrings && alg->parameters[i].enumStrings[0] &&
                 alg->parameters[i].enumStrings[0][0] == 'H' ) { regenP = (int)i; break; }

        if ( firstFreq >= 0 ) for ( int k = 0; k < 4; ++k ) g_vArray[firstFreq + k] = 600;   // 600Hz centers
        if ( firstBw >= 0 ) for ( int k = 0; k < 4; ++k ) g_vArray[firstBw + k] = 12;         // 1-octave bands
        if ( regenP >= 0 ) g_vArray[regenP] = 1;

        bool anyNaN = false; float maxOut = 0.0f;
        for ( int s = 0; s < 18000; ++s )
        {
            for ( int i = 0; i < kNumBuses * n; ++i ) busFrames[i] = 0.0f;
            f->step( alg, busFrames, kNumFramesBy4 );
            float* out = busFrames + ( outBus - 1 ) * n;
            for ( int i = 0; i < n; ++i ) { if ( std::isnan( out[i] ) ) anyNaN = true; if ( s > 16000 && fabsf( out[i] ) > maxOut ) maxOut = fabsf( out[i] ); }
        }
        check( !anyNaN, "custom filter cutoffs + regenerate: no NaN" );
        check( maxOut <= 11.0f + 1e-3f && maxOut > 0.02f, "custom filter cutoffs: bounded, non-silent kit" );
    }

    // --- Pitch (playback speed): +12 semitones should ~double the output's
    //     zero-crossing rate vs 0 (octave up), on the same current kit. Also
    //     confirms the param exists with the right range/unit and stays safe. ---
    {
        int pitchParam = -1;
        for ( uint32_t i = 0; i < req.numParameters; ++i )
            if ( alg->parameters[i].name[0] == 'P' && alg->parameters[i].name[1] == 'i' &&
                 alg->parameters[i].name[2] == 't' ) { pitchParam = (int)i; break; }   // "Pitch"
        check( pitchParam >= 0, "found the Pitch parameter" );
        check( pitchParam >= 0 && alg->parameters[pitchParam].unit == kNT_unitSemitones
               && alg->parameters[pitchParam].min == -48 && alg->parameters[pitchParam].max == 48,
               "Pitch is semitones, range +-48 (+-4 octaves)" );

        // Ensure Play=On (find by "Off"/"On").
        int playP = -1;
        for ( uint32_t i = 0; i < req.numParameters; ++i )
            if ( alg->parameters[i].enumStrings && alg->parameters[i].enumStrings[0] &&
                 alg->parameters[i].enumStrings[0][0] == 'O' && alg->parameters[i].enumStrings[0][1] == 'f' ) { playP = (int)i; break; }
        if ( playP >= 0 ) g_vArray[playP] = 1;

        auto zcRate = [&]( int semis ) {
            g_vArray[pitchParam] = (int16_t)semis;
            // Warm up a few blocks so pitch takes effect, then count crossings.
            for ( int s = 0; s < 50; ++s ) { for ( int i = 0; i < kNumBuses * n; ++i ) busFrames[i] = 0.0f; f->step( alg, busFrames, kNumFramesBy4 ); }
            int cross = 0, samples = 0; float prev = 0.0f; bool anyNaN = false; float mx = 0.0f;
            for ( int s = 0; s < 4000; ++s )
            {
                for ( int i = 0; i < kNumBuses * n; ++i ) busFrames[i] = 0.0f;
                f->step( alg, busFrames, kNumFramesBy4 );
                float* out = busFrames + ( outBus - 1 ) * n;
                for ( int i = 0; i < n; ++i )
                {
                    float v = out[i];
                    if ( std::isnan( v ) ) anyNaN = true;
                    if ( fabsf( v ) > mx ) mx = fabsf( v );
                    if ( prev < 0.0f && v >= 0.0f ) ++cross;
                    prev = v; ++samples;
                }
            }
            if ( anyNaN || mx > 11.0f + 1e-3f ) { printf( "  (pitch %d: NaN=%d max=%.2f)\n", semis, (int)anyNaN, mx ); }
            return (double)cross / (double)samples;   // crossings per sample
        };
        double zc0 = zcRate( 0 );
        double zcUp = zcRate( 12 );
        double ratio = ( zc0 > 1e-9 ) ? zcUp / zc0 : 0.0;
        printf( "Pitch: ZC/sample at 0 st=%.5f, at +12 st=%.5f (ratio %.2fx, expect ~2x)\n", zc0, zcUp, ratio );
        check( ratio > 1.6 && ratio < 2.4, "+12 semitones ~doubles the output zero-crossing rate (octave up via speed)" );

        // Lower pitch must still be safe/bounded.
        double zcDown = zcRate( -24 );
        check( zcDown < zc0, "-24 semitones lowers the output zero-crossing rate (slower/lower)" );
    }

    // --- VCFRND / QCONT (Filters page): both params exist as Off/On enums;
    //     turning both on and regenerating produces a bounded, non-silent,
    //     NaN-free kit (the random-cutoff + per-sample Q-sweep path works
    //     end-to-end through the real factory). ---
    {
        int vcfParam = -1, qcontParam = -1;
        for ( uint32_t i = 0; i < req.numParameters; ++i )
        {
            const char* nm = alg->parameters[i].name;
            if ( nm[0] == 'V' && nm[1] == 'C' && nm[2] == 'F' ) vcfParam = (int)i;   // "VCFRND"
            if ( nm[0] == 'Q' && nm[1] == 'C' && nm[2] == 'O' ) qcontParam = (int)i; // "QCONT"
        }
        check( vcfParam >= 0, "found the VCFRND parameter (Filters page)" );
        check( qcontParam >= 0, "found the QCONT parameter (Filters page)" );

        int regenP = -1, playP = -1;
        for ( uint32_t i = 0; i < req.numParameters; ++i )
        {
            const char* const* es = alg->parameters[i].enumStrings;
            if ( es && es[0] && es[0][0] == 'H' ) regenP = (int)i;                        // "Hold"
            if ( es && es[0] && es[0][0] == 'O' && es[0][1] == 'f' && playP < 0 ) playP = (int)i; // first Off/On = Play
        }
        if ( playP >= 0 ) g_vArray[playP] = 1;
        if ( vcfParam >= 0 ) g_vArray[vcfParam] = 1;      // VCFRND on
        if ( qcontParam >= 0 ) g_vArray[qcontParam] = 1;  // QCONT on
        if ( regenP >= 0 ) g_vArray[regenP] = 1;          // bake with both on

        bool anyNaN = false; float maxOut = 0.0f;
        for ( int s = 0; s < 18000; ++s )
        {
            for ( int i = 0; i < kNumBuses * n; ++i ) busFrames[i] = 0.0f;
            f->step( alg, busFrames, kNumFramesBy4 );
            float* out = busFrames + ( outBus - 1 ) * n;
            for ( int i = 0; i < n; ++i ) { if ( std::isnan( out[i] ) ) anyNaN = true; if ( s > 16000 && fabsf( out[i] ) > maxOut ) maxOut = fabsf( out[i] ); }
        }
        check( !anyNaN, "VCFRND+QCONT on + regenerate: no NaN end-to-end" );
        check( maxOut <= 11.0f + 1e-3f && maxOut > 0.02f, "VCFRND+QCONT on: bounded, non-silent kit" );
    }

    // --- Norm level (Render page): the played-back kit must peak at the set
    //     level and never exceed it (clipping-headroom guarantee), end-to-end
    //     through the real factory. ---
    {
        int normParam = -1, regenP = -1, playP = -1, pitchP = -1;
        for ( uint32_t i = 0; i < req.numParameters; ++i )
        {
            const char* nm = alg->parameters[i].name;
            if ( nm[0] == 'N' && nm[1] == 'o' && nm[2] == 'r' ) normParam = (int)i;   // "Norm level"
            if ( nm[0] == 'P' && nm[1] == 'i' && nm[2] == 't' ) pitchP = (int)i;      // "Pitch"
            const char* const* es = alg->parameters[i].enumStrings;
            if ( es && es[0] && es[0][0] == 'H' ) regenP = (int)i;                     // "Hold"
            if ( es && es[0] && es[0][0] == 'O' && es[0][1] == 'f' && playP < 0 ) playP = (int)i;  // Play
        }
        check( normParam >= 0, "found the Norm level parameter (Render page)" );
        check( normParam >= 0 && alg->parameters[normParam].unit == kNT_unitVolts
               && alg->parameters[normParam].min == 0 && alg->parameters[normParam].max == 11,
               "Norm level is volts, range 0..11" );

        if ( playP >= 0 ) g_vArray[playP] = 1;
        if ( pitchP >= 0 ) g_vArray[pitchP] = 0;       // normal speed so a full loop is read
        g_vArray[normParam] = 5;                        // normalize to 5V
        if ( regenP >= 0 ) g_vArray[regenP] = 1;        // bake a fresh kit

        bool anyNaN = false; float maxOut = 0.0f;
        // Need the bake (~13.2k steps at 1x, chunk 16) to finish, then a full
        // kit loop (~8.8k steps) to sweep the peak. Run 28k; measure after 15k.
        for ( int s = 0; s < 28000; ++s )
        {
            for ( int i = 0; i < kNumBuses * n; ++i ) busFrames[i] = 0.0f;
            f->step( alg, busFrames, kNumFramesBy4 );
            float* out = busFrames + ( outBus - 1 ) * n;
            for ( int i = 0; i < n; ++i ) { if ( std::isnan( out[i] ) ) anyNaN = true; if ( s > 15000 && fabsf( out[i] ) > maxOut ) maxOut = fabsf( out[i] ); }
        }
        printf( "Norm: level=5V, measured playback peak=%.4f\n", maxOut );
        check( !anyNaN, "Norm level: no NaN" );
        check( maxOut <= 5.0f + 1e-2f, "normalized playback never exceeds the 5V level (headroom guarantee)" );
        check( maxOut > 4.0f, "normalized playback reaches ~5V (peak-normalized to the set level)" );
    }

    free( ptrs.sram ); free( ptrs.dram ); free( ptrs.dtc ); free( ptrs.itc ); free( g_vArray );
    printf( "\n%s (%d failures)\n", g_failures == 0 ? "PLUGIN HOST: TESTS PASS" : "PLUGIN HOST: FAILURES", g_failures );
    return g_failures == 0 ? 0 : 1;
}
