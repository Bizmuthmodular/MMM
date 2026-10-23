// test_contour_reference.cpp — confirms the REFERENCE contour_engine.h passes
// its own golden tests (CONTOUR_NT_SPEC.md §8.1), establishing a trustworthy
// baseline before the lazy reimplementation (Step 3) is compared against it.
// Build: g++ -std=c++11 -O2 -o test_contour_reference test_contour_reference.cpp
#include "contour_engine.h"
#include <cstdio>
#include <cmath>

static int g_failures = 0;

static void runCase( const char* name, contour::Params p, const float* expected, int n )
{
    printf( "%s\n", name );
    contour::Engine eng( p );
    eng.setSampleRate( 100.0f );
    eng.rebuild();
    eng.trigger();

    int idx = 0;
    float maxErr = 0.0f;
    for ( int i = 0; i < n * 50; ++i )   // 0.5s steps @ 100Hz = every 50 samples
    {
        float v = eng.process();
        if ( i % 50 == 0 )
        {
            float err = fabsf( v - expected[idx] );
            if ( err > maxErr ) maxErr = err;
            printf( "  t=%.1f  got=%.4f  expected=%.4f  %s\n",
                    idx * 0.5f, v, expected[idx], err < 0.001f ? "ok" : "FAIL" );
            if ( err >= 0.001f ) ++g_failures;
            ++idx;
        }
    }
    printf( "  max error: %g\n\n", maxErr );
}

int main()
{
    {
        contour::Params p;
        p.lengthSec = 6.0f; p.windows = 6; p.chaos = 0.6f; p.radiusSec = 0.12f;
        p.roundProb = 1.0f; p.holdProb = 0.0f; p.levelV = 8.0f; p.offsetV = 0.0f;
        p.bipolar = false; p.loop = false; p.seed = 7;
        float expected[] = { 5.3333f,4.8929f,5.6162f,5.0606f,7.2034f,4.3570f,3.8648f,5.5147f,5.3221f,5.1143f,5.4567f,5.0208f,4.1759f };
        runCase( "Case 1", p, expected, 13 );
    }
    {
        contour::Params p;
        p.lengthSec = 6.0f; p.windows = 6; p.chaos = 0.6f; p.radiusSec = 0.22f;
        p.roundProb = 0.6f; p.holdProb = 0.35f; p.levelV = 8.0f; p.offsetV = 0.0f;
        p.bipolar = false; p.loop = false; p.seed = 7;
        float expected[] = { 5.3333f,4.8929f,5.6162f,5.1717f,7.2753f,6.1624f,3.8393f,5.9604f,5.5207f,5.1865f,5.4950f,5.2327f,4.1759f };
        runCase( "Case 2", p, expected, 13 );
    }
    {
        contour::Params p;
        p.lengthSec = 6.0f; p.windows = 6; p.chaos = 0.6f; p.radiusSec = 0.22f;
        p.roundProb = 0.6f; p.holdProb = 0.35f; p.levelV = 9.5f; p.offsetV = 4.5f;
        p.bipolar = false; p.loop = false; p.seed = 7;
        float expected[] = { 10.0000f,10.0000f,10.0000f,10.0000f,10.0000f,10.0000f,9.0592f,10.0000f,10.0000f,10.0000f,10.0000f,10.0000f,9.4588f };
        runCase( "Case 3", p, expected, 13 );
    }
    {
        contour::Params p;
        p.lengthSec = 7.0f; p.windows = 5; p.chaos = 0.5f; p.radiusSec = 0.25f;
        p.roundProb = 0.5f; p.holdProb = 0.0f; p.levelV = 5.0f; p.offsetV = 0.0f;
        p.bipolar = true; p.loop = false; p.seed = 9;
        float expected[] = { 0.3535f,0.0237f,0.4917f,1.0917f,0.0716f,-1.2661f,0.1253f,0.4924f,1.3469f,0.8309f,0.9297f,0.4802f,0.5790f,0.3968f,0.3535f };
        runCase( "Case 4", p, expected, 15 );
    }

    printf( "%s (%d failures)\n", g_failures == 0 ? "REFERENCE ENGINE: ALL GOLDEN TESTS PASS" : "REFERENCE ENGINE: FAILURES", g_failures );
    return g_failures == 0 ? 0 : 1;
}
