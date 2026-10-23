// test_contour_lazy.cpp — validates contour_lazy.h against (a) the reference
// engine's own golden tests (CONTOUR_NT_SPEC.md §8.1) and (b) a dense
// sample-by-sample diff against the reference engine itself, which is a much
// stricter check than the 13-15 checkpoint golden values alone.
// Build: g++ -std=c++11 -O0 -Wall -Wextra -o test_contour_lazy test_contour_lazy.cpp
#include "contour_lazy.h"
#include "contour_engine.h"
#include <cstdio>
#include <cmath>
#include <utility>

static int g_failures = 0;

static void checkGolden( const char* name, contourlazy::Params p, const float* expected, int n )
{
    printf( "%s (golden)\n", name );
    contourlazy::Engine eng( p );
    eng.setSampleRate( 100.0f );
    eng.rebuild();
    eng.trigger();
    int idx = 0;
    float maxErr = 0.0f;
    for ( int i = 0; i < n * 50; ++i )
    {
        float v = eng.process();
        if ( i % 50 == 0 )
        {
            float err = fabsf( v - expected[idx] );
            if ( err > maxErr ) maxErr = err;
            printf( "  t=%.1f  got=%.4f  expected=%.4f  %s\n", idx * 0.5f, v, expected[idx], err < 0.001f ? "ok" : "FAIL" );
            if ( err >= 0.001f ) ++g_failures;
            ++idx;
        }
    }
    printf( "  max error: %g  (resident window peak: %d)\n\n", maxErr, eng.breakpointCountResident() );
}

static void checkDenseDiff( const char* name, float sr,
                             contour::Params refP, contourlazy::Params lazyP, bool loopFor25s = false )
{
    printf( "%s (dense sample-by-sample diff vs reference, sr=%.0f)\n", name, sr );
    contour::Engine ref( refP );
    ref.setSampleRate( sr );
    ref.rebuild();
    ref.trigger();

    contourlazy::Engine lazy( lazyP );
    lazy.setSampleRate( sr );
    lazy.rebuild();
    lazy.trigger();

    int samples = loopFor25s ? (int)( 25.0 * sr ) : (int)( refP.lengthSec * sr ) + 10;
    float maxErr = 0.0f;
    int maxErrAt = -1;
    int peakResident = 0;
    for ( int i = 0; i < samples; ++i )
    {
        float rv = ref.process();
        float lv = lazy.process();
        float err = fabsf( rv - lv );
        if ( err > maxErr ) { maxErr = err; maxErrAt = i; }
        int res = lazy.breakpointCountResident();
        if ( res > peakResident ) peakResident = res;
    }
    printf( "  %d samples compared, max error %g at sample %d (t=%.4fs), peak resident breakpoints %d\n",
            samples, maxErr, maxErrAt, maxErrAt / sr, peakResident );
    bool ok = maxErr < 0.002f;   // slightly looser than golden-value tolerance, dense comparison
    printf( "  %s\n\n", ok ? "ok" : "FAIL" );
    if ( !ok ) ++g_failures;
}

int main()
{
    // --- Golden-value checks (same 4 cases as the reference engine) ---
    {
        contourlazy::Params p;
        p.lengthSec = 6.0f; p.windows = 6; p.chaos = 0.6f; p.radiusSec = 0.12f;
        p.roundProb = 1.0f; p.holdProb = 0.0f; p.levelV = 8.0f; p.offsetV = 0.0f;
        p.bipolar = false; p.loop = false; p.seed = 7;
        float expected[] = { 5.3333f,4.8929f,5.6162f,5.0606f,7.2034f,4.3570f,3.8648f,5.5147f,5.3221f,5.1143f,5.4567f,5.0208f,4.1759f };
        checkGolden( "Case 1", p, expected, 13 );
    }
    {
        contourlazy::Params p;
        p.lengthSec = 6.0f; p.windows = 6; p.chaos = 0.6f; p.radiusSec = 0.22f;
        p.roundProb = 0.6f; p.holdProb = 0.35f; p.levelV = 8.0f; p.offsetV = 0.0f;
        p.bipolar = false; p.loop = false; p.seed = 7;
        float expected[] = { 5.3333f,4.8929f,5.6162f,5.1717f,7.2753f,6.1624f,3.8393f,5.9604f,5.5207f,5.1865f,5.4950f,5.2327f,4.1759f };
        checkGolden( "Case 2", p, expected, 13 );
    }
    {
        contourlazy::Params p;
        p.lengthSec = 6.0f; p.windows = 6; p.chaos = 0.6f; p.radiusSec = 0.22f;
        p.roundProb = 0.6f; p.holdProb = 0.35f; p.levelV = 9.5f; p.offsetV = 4.5f;
        p.bipolar = false; p.loop = false; p.seed = 7;
        float expected[] = { 10.0000f,10.0000f,10.0000f,10.0000f,10.0000f,10.0000f,9.0592f,10.0000f,10.0000f,10.0000f,10.0000f,10.0000f,9.4588f };
        checkGolden( "Case 3", p, expected, 13 );
    }
    {
        contourlazy::Params p;
        p.lengthSec = 7.0f; p.windows = 5; p.chaos = 0.5f; p.radiusSec = 0.25f;
        p.roundProb = 0.5f; p.holdProb = 0.0f; p.levelV = 5.0f; p.offsetV = 0.0f;
        p.bipolar = true; p.loop = false; p.seed = 9;
        float expected[] = { 0.3535f,0.0237f,0.4917f,1.0917f,0.0716f,-1.2661f,0.1253f,0.4924f,1.3469f,0.8309f,0.9297f,0.4802f,0.5790f,0.3968f,0.3535f };
        checkGolden( "Case 4", p, expected, 15 );
    }

    // --- Dense sample-by-sample diffs at audio rate against the reference
    //     engine directly (far stricter than 13-15 checkpoints) ---
    auto mk = [&]( float len, int win, float chaos, float rad, float rp, float hp, float lv, float off, bool bip, uint32_t seed )
    {
        contour::Params rp_; rp_.lengthSec=len; rp_.windows=win; rp_.chaos=chaos; rp_.radiusSec=rad;
        rp_.roundProb=rp; rp_.holdProb=hp; rp_.levelV=lv; rp_.offsetV=off; rp_.bipolar=bip; rp_.loop=false; rp_.seed=seed;
        contourlazy::Params lp_; lp_.lengthSec=len; lp_.windows=win; lp_.chaos=chaos; lp_.radiusSec=rad;
        lp_.roundProb=rp; lp_.holdProb=hp; lp_.levelV=lv; lp_.offsetV=off; lp_.bipolar=bip; lp_.loop=false; lp_.seed=seed;
        return std::pair<contour::Params, contourlazy::Params>( rp_, lp_ );
    };

    { auto pr = mk( 6.0f, 6, 0.6f, 0.12f, 1.0f, 0.0f, 8.0f, 0.0f, false, 7 );
      checkDenseDiff( "Dense: Case 1 config, 48kHz", 48000.0f, pr.first, pr.second ); }
    { auto pr = mk( 6.0f, 6, 0.6f, 0.22f, 0.6f, 0.35f, 8.0f, 0.0f, false, 7 );
      checkDenseDiff( "Dense: Case 2 config (holds+rounding), 48kHz", 48000.0f, pr.first, pr.second ); }
    { auto pr = mk( 6.0f, 6, 0.6f, 0.22f, 0.6f, 0.35f, 9.5f, 4.5f, false, 7 );
      checkDenseDiff( "Dense: Case 3 config (rail clipping), 48kHz", 48000.0f, pr.first, pr.second ); }
    { auto pr = mk( 7.0f, 5, 0.5f, 0.25f, 0.5f, 0.0f, 5.0f, 0.0f, true, 9 );
      checkDenseDiff( "Dense: Case 4 config (bipolar), 48kHz", 48000.0f, pr.first, pr.second ); }
    // Native (untrimmed) settings at the plugin's locked config: W up to 32, full kPieces=24, wide seed sweep.
    { auto pr = mk( 4.9f, 32, 0.8f, 0.05f, 0.8f, 0.5f, 10.0f, 0.0f, true, 101 );
      checkDenseDiff( "Dense: native W=32, heavy holds+rounding, seed 101, 32kHz", 32000.0f, pr.first, pr.second ); }
    { auto pr = mk( 4.9f, 1, 0.1f, 0.3f, 0.3f, 0.2f, 7.0f, 0.0f, false, 202 );
      checkDenseDiff( "Dense: W=1 edge case, seed 202, 32kHz", 32000.0f, pr.first, pr.second ); }
    { auto pr = mk( 1.0f, 32, 1.0f, 0.02f, 1.0f, 0.8f, 10.0f, 0.0f, true, 303 );
      checkDenseDiff( "Dense: short length + max chaos/hold stress, seed 303, 32kHz", 32000.0f, pr.first, pr.second ); }

    // --- Loop stability: 25s @ 48kHz without stopping, output stays in range ---
    {
        printf( "Loop stability: 25s @ 48kHz\n" );
        contourlazy::Params p; p.lengthSec=6.0f; p.windows=6; p.chaos=0.6f; p.radiusSec=0.12f;
        p.roundProb=1.0f; p.holdProb=0.0f; p.levelV=8.0f; p.offsetV=0.0f; p.bipolar=false; p.loop=true; p.seed=7;
        contourlazy::Engine eng( p );
        eng.setSampleRate( 48000.0f );
        eng.rebuild();
        eng.trigger();
        bool inRange = true;
        int peakResident = 0;
        for ( int i = 0; i < (int)( 25.0 * 48000 ); ++i )
        {
            float v = eng.process();
            if ( v < -10.0f || v > 10.0f ) inRange = false;
            int r = eng.breakpointCountResident();
            if ( r > peakResident ) peakResident = r;
        }
        printf( "  stayed within +/-10V: %s, peak resident breakpoints: %d\n\n", inRange ? "yes" : "NO", peakResident );
        if ( !inRange ) ++g_failures;
    }

    printf( "%s (%d failures)\n", g_failures == 0 ? "LAZY ENGINE: ALL TESTS PASS" : "LAZY ENGINE: FAILURES", g_failures );
    printf( "sizeof(contourlazy::Engine) = %zu bytes\n", sizeof( contourlazy::Engine ) );
    return g_failures == 0 ? 0 : 1;
}
