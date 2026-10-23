// test_p4_phaser.cpp — desktop validation for p4_phaser.h (the 12-stage
// allpass phaser that replaced the bitcrusher, 2026-10-21).
//
// The phaser is a cascade of 12 identical first-order allpass sections plus a
// dry+wet mix, so its mix magnitude response is provably
//     |H(f)| = |cos( 6 * phi1(f) )|
// where phi1(f) is the single-section allpass phase (0 -> -pi over 0 -> fs/2).
// That "6" is 12/2 — it is the fingerprint of the stage count. This test
// verifies the implementation against that analytic response rather than via
// brittle notch-peak-picking on a grid:
//   1. The analytic response has EXACTLY 6 notches across 0..fs/2 (=> 12
//      stages). [pure math — no DSP, robust]
//   2. The MEASURED steady-state magnitude matches |cos(6*phi1)| closely at a
//      spread of probe frequencies (=> the code really is 12 allpass stages +
//      dry/wet mix, not merely "something that changes the sound").
//   3. Sweep: raising the center frequency moves the analytic notches up, and
//      a measured spot-check confirms the comb shifts.
//   4. Stability: static mix obeys |out| <= sum of input partial amplitudes
//      (|H|<=1 per component); a smoothly swept center frequency stays finite
//      and bounded (no runaway) across 48k..384k oversampled rates.
//   5. The randomized per-render base is deterministic (seed-reproducible).
//
// Build: g++ -std=c++11 -O2 -o test_p4_phaser test_p4_phaser.cpp
#include "p4_phaser.h"
#include <cstdio>
#include <cmath>

static int g_failures = 0;
static void check( bool cond, const char* what ) { printf( "  %s: %s\n", cond ? "ok" : "FAIL", what ); if ( !cond ) ++g_failures; }

static const float kTwoPi = 6.283185307179586f;
static const float kPi    = 3.14159265358979324f;

// Pin the randomized base so fc is a known value. r in [0,1] -> base in
// [kBaseFreqMin, kBaseFreqMax].
static float g_baseR = 0.4667f;
static float pinnedBase( uint32_t, int ) { return g_baseR; }
static float rForFreq( float hz ) { return ( hz - p4phaser::kBaseFreqMin ) / ( p4phaser::kBaseFreqMax - p4phaser::kBaseFreqMin ); }

// The coefficient the Processor uses for a given fc/fs (mirrors p4_phaser.h).
static float coeffFor( float fc, float sr )
{
    float ceil = 0.45f * sr; if ( ceil > p4phaser::kFreqCeil ) ceil = p4phaser::kFreqCeil;
    if ( fc < p4phaser::kFreqFloor ) fc = p4phaser::kFreqFloor;
    if ( fc > ceil ) fc = ceil;
    float t = tanf( kPi * fc / sr );
    return ( t - 1.0f ) / ( t + 1.0f );   // -90deg at fc (matches p4_phaser.h)
}

// Analytic single-section allpass phase: angle of (a+e^-jw)/(1+a e^-jw).
static float allpassPhase( float a, float w )
{
    float nr = a + cosf( w ), ni = -sinf( w );
    float dr = 1.0f + a * cosf( w ), di = -a * sinf( w );
    return atan2f( ni, nr ) - atan2f( di, dr );
}
// Analytic mix magnitude = |cos(6*phi1)|.
static float analyticMag( float a, float f, float sr ) { return fabsf( cosf( 6.0f * allpassPhase( a, kTwoPi * f / sr ) ) ); }

// Measured steady-state magnitude |H(f)| of the dry+wet mix (unit sine in,
// peak out). Allpass poles at |a|<1 decay fast, so a few thousand settle
// samples suffice at any input frequency.
static float measuredMag( float fc, float f, float sr )
{
    p4phaser::Processor p; p.randomBaseFn = pinnedBase;
    g_baseR = rForFreq( fc );
    p.reset(); p.randomBaseFn = pinnedBase; p.onNewRender( 1, 0 );
    const int settle = 6000;
    for ( int i = 0; i < settle; ++i ) p.processSample( sinf( kTwoPi * f * i / sr ), 0.0f, sr );
    float peak = 0.0f;
    const int win = (int)( sr / f * 8.0f ) + 128;
    for ( int i = settle; i < settle + win; ++i )
    {
        float y = p.processSample( sinf( kTwoPi * f * i / sr ), 0.0f, sr );
        float a = fabsf( y ); if ( a > peak ) peak = a;
    }
    return peak;
}

// Count sign changes (zeros) of the analytic cos(6*phi1) over a dense grid =>
// the notch count, which must equal 6 for a 12-stage cascade.
static int analyticNotchCount( float fc, float sr, float* firstNotchHz )
{
    float a = coeffFor( fc, sr );
    float fLo = 1.0f, fHi = 0.4999f * sr;
    int grid = 20000;
    float prev = cosf( 6.0f * allpassPhase( a, kTwoPi * fLo / sr ) );
    int zeros = 0; *firstNotchHz = -1.0f;
    for ( int g = 1; g < grid; ++g )
    {
        float f = fLo + ( fHi - fLo ) * (float)g / (float)( grid - 1 );
        float c = cosf( 6.0f * allpassPhase( a, kTwoPi * f / sr ) );
        if ( ( prev < 0.0f ) != ( c < 0.0f ) ) { ++zeros; if ( *firstNotchHz < 0.0f ) *firstNotchHz = f; }
        prev = c;
    }
    return zeros;
}

int main()
{
    const float sr = 48000.0f;

    // --- 1. Analytic structure: exactly 6 notches => 12 stages. ---
    {
        float firstNotch;
        int n = analyticNotchCount( 1000.0f, sr, &firstNotch );
        printf( "  analytic notches (fc=1000Hz) = %d, first at %.0fHz\n", n, firstNotch );
        check( n == 6, "12-stage cascade's dry+wet mix has exactly 6 notches across 0..fs/2" );
    }

    // --- 2. Measured response matches the analytic |cos(6*phi1)| closely. ---
    {
        float fc = 1000.0f, a = coeffFor( fc, sr );
        float probes[] = { 120.0f, 300.0f, 700.0f, 1500.0f, 2500.0f, 4000.0f, 6000.0f, 9000.0f, 14000.0f };
        float worst = 0.0f; int nprobe = (int)( sizeof(probes)/sizeof(probes[0]) );
        for ( int i = 0; i < nprobe; ++i )
        {
            float meas = measuredMag( fc, probes[i], sr );
            float pred = analyticMag( a, probes[i], sr );
            float e = fabsf( meas - pred );
            if ( e > worst ) worst = e;
        }
        printf( "  worst |measured-analytic| magnitude error over %d probes = %.4f\n", nprobe, worst );
        check( worst < 0.06f, "measured magnitude matches the analytic 12-stage |cos(6*phi1)| response" );
    }

    // --- 2b. The comb is a real phaser: deep notches AND near-unity peaks. ---
    {
        float fc = 1000.0f, a = coeffFor( fc, sr );
        // Find an analytic notch freq and a peak freq, measure both.
        float notchF = -1.0f, peakF = -1.0f;
        float prevC = 1.0f, prevF = 20.0f;
        for ( float f = 20.0f; f < 0.49f * sr; f += 2.0f )
        {
            float c = cosf( 6.0f * allpassPhase( a, kTwoPi * f / sr ) );
            if ( notchF < 0.0f && ( prevC < 0.0f ) != ( c < 0.0f ) ) notchF = f;      // zero crossing
            if ( peakF < 0.0f && fabsf( c ) > 0.999f && f > 50.0f ) peakF = f;         // |cos|~1
            prevC = c; prevF = f; (void)prevF;
        }
        float mNotch = measuredMag( fc, notchF, sr );
        float mPeak  = measuredMag( fc, peakF, sr );
        printf( "  measured: notch@%.0fHz=%.3f  peak@%.0fHz=%.3f\n", notchF, mNotch, peakF, mPeak );
        check( mNotch < 0.12f, "a notch frequency is deeply attenuated (true cancellation)" );
        check( mPeak > 0.9f, "a peak frequency passes at ~unity (dry+wet in phase)" );
    }

    // --- 3. Sweep: raising fc moves the notches up (analytic + measured). ---
    {
        float f0, f1;
        analyticNotchCount( 600.0f, sr, &f0 );
        analyticNotchCount( 2100.0f, sr, &f1 );   // base 600 + a +1500 Hz offset
        printf( "  first notch: fc=600 -> %.0fHz, fc=2100 -> %.0fHz\n", f0, f1 );
        check( f1 > f0 * 1.1f, "raising the center frequency sweeps the notch comb upward (analytic)" );

        // Measured spot-check: at f0 (a notch when fc=600) the magnitude should
        // rise once fc moves to 2100 (that notch has moved away).
        float atNotch_fc600  = measuredMag( 600.0f, f0, sr );
        float atNotch_fc2100 = measuredMag( 2100.0f, f0, sr );
        printf( "  mag at %.0fHz: fc=600 -> %.3f, fc=2100 -> %.3f\n", f0, atNotch_fc600, atNotch_fc2100 );
        check( atNotch_fc2100 > atNotch_fc600 + 0.2f, "sweeping fc up lifts the old notch frequency out of the null (measured)" );
    }

    // --- 4a. Static LTI bound: |out| <= sum of input partial amplitudes. ---
    {
        float amp = 1.6f; float inputBound = 3.0f * amp;   // 3 partials, |H|<=1 each
        bool bounded = true;
        p4phaser::Processor p; p.randomBaseFn = pinnedBase; g_baseR = rForFreq( 900.0f );
        p.reset(); p.randomBaseFn = pinnedBase; p.onNewRender( 1, 0 );
        for ( int i = 0; i < 20000; ++i )
        {
            float x = amp * ( sinf( kTwoPi * 110.0f * i / sr ) + sinf( kTwoPi * 650.0f * i / sr ) + sinf( kTwoPi * 3300.0f * i / sr ) );
            float y = p.processSample( x, 0.0f, sr );
            if ( fabsf( y ) > inputBound + 1e-3f ) bounded = false;
        }
        check( bounded, "static phaser mix stays within the sum-of-partials bound (|H|<=1)" );
    }

    // --- 4b. Smoothly swept center freq: finite & bounded (no runaway) across
    //         48k..384k oversampled rates. The real Contour sweep is smooth, so
    //         coefficient changes are gradual — no energy blow-up. ---
    {
        float rates[] = { 48000.0f, 96000.0f, 192000.0f, 384000.0f };
        bool finite = true, bounded = true;
        for ( int ri = 0; ri < 4; ++ri )
        {
            float srr = rates[ri];
            p4phaser::Processor p; p.randomBaseFn = pinnedBase; g_baseR = 0.4f;
            p.reset(); p.randomBaseFn = pinnedBase; p.onNewRender( 7, ri );
            for ( int i = 0; i < 20000; ++i )
            {
                float off = 1800.0f * sinf( kTwoPi * 0.5f * i / srr );   // slow +-1800Hz LFO sweep
                float x = 1.6f * ( sinf( kTwoPi * 110.0f * i / srr ) + sinf( kTwoPi * 650.0f * i / srr ) + sinf( kTwoPi * 3300.0f * i / srr ) );
                float y = p.processSample( x, off, srr );
                if ( std::isnan( y ) || std::isinf( y ) ) finite = false;
                if ( fabsf( y ) > 6.0f ) bounded = false;   // generous ceiling; blow-up would be huge
            }
        }
        check( finite, "smoothly swept phaser: no NaN/Inf across 48k..384k rates" );
        check( bounded, "smoothly swept phaser: output stays bounded (no instability)" );
    }

    // --- 5. Randomized base is deterministic (seed/render reproducible). ---
    {
        p4phaser::Processor a, b;
        a.reset(); b.reset();
        a.onNewRender( 12345, 3 ); b.onNewRender( 12345, 3 );
        bool identical = true, differs = false;
        for ( int i = 0; i < 500; ++i )
        {
            float x = sinf( kTwoPi * 300.0f * i / sr );
            if ( fabsf( a.processSample( x, 0.0f, sr ) - b.processSample( x, 0.0f, sr ) ) > 1e-7f ) identical = false;
        }
        check( identical, "same seed+render draws the same base center freq (reproducible)" );

        p4phaser::Processor c, d;
        c.reset(); d.reset();
        c.onNewRender( 12345, 3 ); d.onNewRender( 12345, 4 );   // different render index
        for ( int i = 0; i < 500; ++i )
        {
            float x = sinf( kTwoPi * 300.0f * i / sr );
            if ( fabsf( c.processSample( x, 0.0f, sr ) - d.processSample( x, 0.0f, sr ) ) > 1e-4f ) differs = true;
        }
        check( differs, "a different render index draws a different base center freq" );
    }

    printf( "\n%s (%d failures)\n", g_failures == 0 ? "P4 PHASER: TESTS PASS" : "P4 PHASER: FAILURES", g_failures );
    return g_failures == 0 ? 0 : 1;
}
