// test_fft.cpp — desktop validation for fft.h (Implementation Plan Step 2).
// Build: g++ -std=c++11 -O2 -o test_fft test_fft.cpp && ./test_fft
#include "fft.h"
#include <cstdio>
#include <cmath>

static int g_failures = 0;

static void check( bool cond, const char* what )
{
    if ( !cond ) { printf( "  FAIL: %s\n", what ); ++g_failures; }
    else         printf( "  ok:   %s\n", what );
}

static bool approx( float a, float b, float tol = 1e-3f )
{
    return fabsf( a - b ) < tol;
}

int main()
{
    fft::initTwiddles();
    const int N = 1024;
    fft::Complex buf[2048];

    // --- Test 1: DC signal -> bin 0 = N, all other bins ~0 ---
    printf( "Test 1: DC signal (N=%d)\n", N );
    for ( int i = 0; i < N; ++i ) { buf[i].re = 1.0f; buf[i].im = 0.0f; }
    fft::transform( buf, N, false );
    check( approx( buf[0].re, (float)N, 1.0f ) && approx( buf[0].im, 0.0f ), "bin 0 == N" );
    bool restZero = true;
    for ( int i = 1; i < N; ++i )
        if ( !approx( buf[i].re, 0.0f, 0.5f ) || !approx( buf[i].im, 0.0f, 0.5f ) ) restZero = false;
    check( restZero, "all other bins ~0" );

    // --- Test 2: impulse -> flat spectrum, all bins magnitude 1 ---
    printf( "Test 2: impulse (N=%d)\n", N );
    for ( int i = 0; i < N; ++i ) { buf[i].re = 0.0f; buf[i].im = 0.0f; }
    buf[0].re = 1.0f;
    fft::transform( buf, N, false );
    bool flat = true;
    for ( int i = 0; i < N; ++i )
    {
        float mag = sqrtf( buf[i].re * buf[i].re + buf[i].im * buf[i].im );
        if ( !approx( mag, 1.0f, 0.01f ) ) flat = false;
    }
    check( flat, "flat unit-magnitude spectrum" );

    // --- Test 3: single sine at bin k=10 -> energy concentrated at bins 10 and N-10 ---
    printf( "Test 3: single sine, bin 10 (N=%d)\n", N );
    const int k = 10;
    for ( int i = 0; i < N; ++i )
    {
        buf[i].re = sinf( 2.0f * 3.14159265f * k * i / N );
        buf[i].im = 0.0f;
    }
    fft::transform( buf, N, false );
    float peakMag = 0.0f; int peakBin = -1;
    for ( int i = 0; i < N / 2; ++i )
    {
        float mag = sqrtf( buf[i].re * buf[i].re + buf[i].im * buf[i].im );
        if ( mag > peakMag ) { peakMag = mag; peakBin = i; }
    }
    check( peakBin == k, "peak energy at expected bin" );
    float otherEnergy = 0.0f;
    for ( int i = 0; i < N / 2; ++i )
    {
        if ( i == k ) continue;
        float mag = sqrtf( buf[i].re * buf[i].re + buf[i].im * buf[i].im );
        otherEnergy += mag;
    }
    check( otherEnergy < peakMag * 0.05f, "negligible energy elsewhere (spectral leakage small)" );

    // --- Test 4: round-trip forward+inverse recovers the original signal ---
    printf( "Test 4: round-trip forward+inverse (N=%d)\n", N );
    float original[1024];
    for ( int i = 0; i < N; ++i )
    {
        original[i] = sinf( 2.0f * 3.14159265f * 37 * i / N ) + 0.3f * cosf( 2.0f * 3.14159265f * 5 * i / N );
        buf[i].re = original[i];
        buf[i].im = 0.0f;
    }
    fft::transform( buf, N, false );
    fft::transform( buf, N, true );
    bool roundTrip = true;
    float maxErr = 0.0f;
    for ( int i = 0; i < N; ++i )
    {
        float err = fabsf( buf[i].re - original[i] );
        if ( err > maxErr ) maxErr = err;
        if ( err > 1e-3f ) roundTrip = false;
    }
    printf( "  max round-trip error: %g\n", maxErr );
    check( roundTrip, "round-trip matches original within 1e-3" );

    // --- Test 5: 2048-point transform works too (zero-padded spectrum size) ---
    printf( "Test 5: 2048-point DC signal\n" );
    const int N2 = 2048;
    for ( int i = 0; i < N2; ++i ) { buf[i].re = 1.0f; buf[i].im = 0.0f; }
    fft::transform( buf, N2, false );
    check( approx( buf[0].re, (float)N2, 1.0f ), "2048-point bin 0 == N2" );

    printf( "\n%s (%d failure%s)\n", g_failures == 0 ? "ALL TESTS PASSED" : "SOME TESTS FAILED",
            g_failures, g_failures == 1 ? "" : "s" );
    return g_failures == 0 ? 0 : 1;
}
