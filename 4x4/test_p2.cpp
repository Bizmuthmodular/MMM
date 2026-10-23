// test_p2.cpp — desktop validation for p2_freqshift.h (Implementation Plan
// Step 7). Confirms a test tone's spectral peak moves by the expected
// FIXED Hz amount (not a ratio, unlike P1) at a few shift settings.
// Build: g++ -std=c++11 -O2 -o test_p2 test_p2.cpp
#include "fft.h"
#include "p2_freqshift.h"
#include <cstdio>
#include <cmath>

static int g_failures = 0;
static void check( bool cond, const char* what ) { printf( "  %s: %s\n", cond ? "ok" : "FAIL", what ); if ( !cond ) ++g_failures; }

// p2freqshift::Processor draws a random per-render base shift in
// onNewRender() (±2000Hz, by design — see p2_freqshift.h). These tests
// validate the depthHz parameter's accuracy in isolation, so the random
// base is pinned to zero via the processor's pluggable randomBaseFn hook.
static float zeroBase( uint32_t, int ) { return 0.5f; }   // (0.5*2-1)*2000 = 0

static float estimateFreqHz( const float* buf, int n, float sr )
{
    const int kN = 2048;   // must not exceed fft::kFFTMaxSize
    fft::Complex a[kN];
    int offset = ( n - kN ) / 2;
    if ( offset < 0 ) offset = 0;
    for ( int i = 0; i < kN; ++i )
    {
        float w = 0.5f - 0.5f * cosf( 6.283185307f * i / ( kN - 1 ) );
        float s = ( offset + i < n ) ? buf[offset + i] : 0.0f;
        a[i].re = s * w; a[i].im = 0.0f;
    }
    fft::transform( a, kN, false );
    int peak = 1; float peakMag = 0.0f;
    for ( int i = 1; i < kN / 2; ++i )
    {
        float m = a[i].re * a[i].re + a[i].im * a[i].im;
        if ( m > peakMag ) { peakMag = m; peak = i; }
    }
    auto magAt = [&]( int i ) { return sqrtf( a[i].re * a[i].re + a[i].im * a[i].im ); };
    float m0 = magAt( peak - 1 ), m1 = magAt( peak ), m2 = magAt( peak + 1 );
    float denom = ( m0 - 2.0f * m1 + m2 );
    float delta = ( denom != 0.0f ) ? 0.5f * ( m0 - m2 ) / denom : 0.0f;
    return ( peak + delta ) * sr / kN;
}

int main()
{
    fft::initTwiddles();
    const float sr = 48000.0f;
    const float kTwoPi = 6.283185307f;

    auto testShift = [&]( float shiftHz, float inFreq )
    {
        p2freqshift::Processor p;
        p.randomBaseFn = zeroBase;
        p.onNewRender( 7, 0 );
        const int N = 48000;
        static float outBuf[48000];
        for ( int i = 0; i < N; ++i )
        {
            float x = sinf( kTwoPi * inFreq * i / sr );
            outBuf[i] = p.processSample( x, shiftHz, sr );
        }
        // Settle time: allpass IIR state settles in well under a ms; use
        // the back half to be safe.
        float measured = estimateFreqHz( outBuf + N / 2, N / 2, sr );
        float expected = inFreq + shiftHz;   // fixed Hz offset, not a ratio
        float errHz = fabsf( measured - expected );
        printf( "shift=%+7.1fHz, in=%7.1fHz: expected out=%7.2fHz, measured=%7.2fHz (err %.2fHz)\n",
                shiftHz, inFreq, expected, measured, errHz );
        check( errHz < 10.0f, "measured frequency within 10Hz of expected fixed-Hz shift" );
    };

    printf( "Frequency shift tests (fixed Hz offset, not a ratio):\n" );
    testShift( 0.0f, 440.0f );
    testShift( 200.0f, 440.0f );
    testShift( -200.0f, 440.0f );
    testShift( 1000.0f, 1000.0f );
    testShift( -500.0f, 2000.0f );

    // --- Random base offset, same pattern as P1: confirm the default
    //     randomBaseFn actually perturbs the shift away from depthHz alone. ---
    {
        p2freqshift::Processor p;
        p.onNewRender( 11, 2 );   // default randomBaseFn, not overridden
        const int N = 48000;
        static float outBuf[48000];
        for ( int i = 0; i < N; ++i )
        {
            float x = sinf( kTwoPi * 440.0f * i / sr );
            outBuf[i] = p.processSample( x, 0.0f, sr );
        }
        float measured = estimateFreqHz( outBuf + N / 2, N / 2, sr );
        printf( "\nRandom base (seed=11, renderIndex=2), depth=0, in=440Hz: measured out=%.2fHz\n", measured );
        check( fabsf( measured - 440.0f ) > 10.0f, "default random base perturbs output away from the unshifted input frequency" );
    }

    printf( "\n%s (%d failures)\n", g_failures == 0 ? "P2 FREQUENCY SHIFTER: TESTS PASS" : "P2 FREQUENCY SHIFTER: FAILURES", g_failures );
    return g_failures == 0 ? 0 : 1;
}
