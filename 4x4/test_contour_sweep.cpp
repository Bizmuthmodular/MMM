// test_contour_sweep.cpp — broad randomized stress sweep: lazy vs reference
// engine, many seeds/configs, dense sample-by-sample comparison.
// Build: g++ -std=c++11 -O2 -o test_contour_sweep test_contour_sweep.cpp
#include "contour_lazy.h"
#include "contour_engine.h"
#include <cstdio>
#include <cmath>
#include <cstdlib>

int main()
{
    int failures = 0;
    int peakResidentOverall = 0;
    const int kRuns = 200;
    for ( int run = 0; run < kRuns; ++run )
    {
        srand( run * 7919 + 13 );
        float len   = 1.0f + ( rand() % 901 ) / 100.0f;     // 1.0 - 10.0
        int   win   = 1 + rand() % 32;                       // 1 - 32
        float chaos = ( rand() % 101 ) / 100.0f;
        float rad   = ( rand() % 501 ) / 1000.0f;             // 0 - 0.5
        float rp    = ( rand() % 101 ) / 100.0f;
        float hp    = ( rand() % 101 ) / 100.0f;
        float lv    = 0.5f + ( rand() % 100 ) / 10.0f;
        float off   = -5.0f + ( rand() % 101 ) / 10.0f;
        bool  bip   = ( rand() % 2 ) == 0;
        uint32_t seed = (uint32_t)( rand() % 32767 ) + 1;
        float sr    = ( rand() % 2 ) ? 32000.0f : 48000.0f;

        contour::Params rpP;
        rpP.lengthSec=len; rpP.windows=win; rpP.chaos=chaos; rpP.radiusSec=rad;
        rpP.roundProb=rp; rpP.holdProb=hp; rpP.levelV=lv; rpP.offsetV=off; rpP.bipolar=bip; rpP.loop=false; rpP.seed=seed;
        contourlazy::Params lpP;
        lpP.lengthSec=len; lpP.windows=win; lpP.chaos=chaos; lpP.radiusSec=rad;
        lpP.roundProb=rp; lpP.holdProb=hp; lpP.levelV=lv; lpP.offsetV=off; lpP.bipolar=bip; lpP.loop=false; lpP.seed=seed;

        contour::Engine ref( rpP ); ref.setSampleRate( sr ); ref.rebuild(); ref.trigger();
        contourlazy::Engine lazy( lpP ); lazy.setSampleRate( sr ); lazy.rebuild(); lazy.trigger();

        int samples = (int)( len * sr ) + 20;
        float maxErr = 0.0f;
        int peakResident = 0;
        for ( int i = 0; i < samples; ++i )
        {
            float rv = ref.process();
            float lv2 = lazy.process();
            float err = fabsf( rv - lv2 );
            if ( err > maxErr ) maxErr = err;
            int r = lazy.breakpointCountResident();
            if ( r > peakResident ) peakResident = r;
        }
        if ( peakResident > peakResidentOverall ) peakResidentOverall = peakResident;
        bool ok = maxErr < 0.002f;
        if ( !ok )
        {
            ++failures;
            printf( "RUN %d FAIL: len=%.2f win=%d chaos=%.2f rad=%.3f rp=%.2f hp=%.2f lv=%.2f off=%.2f bip=%d seed=%u sr=%.0f maxErr=%g\n",
                    run, len, win, chaos, rad, rp, hp, lv, off, bip, seed, sr, maxErr );
        }
    }
    printf( "%d/%d runs passed. Peak resident breakpoints across all runs: %d\n", kRuns - failures, kRuns, peakResidentOverall );
    printf( "sizeof(contourlazy::Engine) = %zu bytes\n", sizeof( contourlazy::Engine ) );
    return failures == 0 ? 0 : 1;
}
