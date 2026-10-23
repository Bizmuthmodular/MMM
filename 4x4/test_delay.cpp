// test_delay.cpp — desktop validation for delay.h (P3, replaces the former
// wavefolder). Confirms the delay actually delays by the expected number of
// samples, that the base+offset model lands the time in range, and that the
// buffer-length clamp (max delay scales down with the internal/oversampled
// rate) behaves as documented.
// Build: g++ -std=c++11 -O2 -o test_delay test_delay.cpp
#include "delay.h"
#include <cstdio>
#include <cmath>

static int g_failures = 0;
static void check( bool cond, const char* what ) { printf( "  %s: %s\n", cond ? "ok" : "FAIL", what ); if ( !cond ) ++g_failures; }

// Pin the base delay to a chosen ms value.
static float g_baseR = 0.0f;
static float pinnedBase( uint32_t, int ) { return g_baseR; }
static float rForMs( float ms ) { return ( ms - p3delay::kMinMs ) / ( p3delay::kMaxMs - p3delay::kMinMs ); }

int main()
{
    const float sr = 48000.0f;

    // --- An impulse comes out delayed by exactly base-delay samples (offset 0). ---
    {
        p3delay::Processor p; p.randomBaseFn = pinnedBase;
        g_baseR = rForMs( 10.0f );   // 10ms base
        p.reset(); p.randomBaseFn = pinnedBase; p.onNewRender( 1, 0 );
        int expected = (int)( 10.0f * sr / 1000.0f + 0.5f );   // 480 samples
        int impulseAt = 5, seenAt = -1;
        for ( int i = 0; i < 2000; ++i )
        {
            float x = ( i == impulseAt ) ? 1.0f : 0.0f;
            float y = p.processSample( x, 0.0f, sr );
            if ( y > 0.5f && seenAt < 0 ) seenAt = i;
        }
        printf( "  impulse in at %d, out at %d (expected delay %d samples)\n", impulseAt, seenAt, expected );
        check( seenAt == impulseAt + expected, "10ms base delay outputs the impulse delayed by exactly 480 samples @48kHz" );
    }

    // --- Depth offset shifts the delay time: base 50ms + offset +30ms = 80ms. ---
    {
        p3delay::Processor p; p.randomBaseFn = pinnedBase;
        g_baseR = rForMs( 50.0f );
        p.reset(); p.randomBaseFn = pinnedBase; p.onNewRender( 1, 0 );
        int expected = (int)( 80.0f * sr / 1000.0f + 0.5f );   // 3840
        int seenAt = -1;
        for ( int i = 0; i < 6000; ++i )
        {
            float x = ( i == 0 ) ? 1.0f : 0.0f;
            float y = p.processSample( x, 30.0f, sr );   // +30ms offset
            if ( y > 0.5f && seenAt < 0 ) seenAt = i;
        }
        check( seenAt == expected, "base 50ms + 30ms offset delays by 80ms worth of samples" );
    }

    // --- Clamp to 1-100ms: a huge positive offset clamps at 100ms, not beyond. ---
    {
        p3delay::Processor p; p.randomBaseFn = pinnedBase;
        g_baseR = rForMs( 50.0f );
        p.reset(); p.randomBaseFn = pinnedBase; p.onNewRender( 1, 0 );
        int expected = (int)( 100.0f * sr / 1000.0f + 0.5f );   // 4800 -> clamped to buffer 4799
        if ( expected > p3delay::kMaxSamples - 1 ) expected = p3delay::kMaxSamples - 1;
        int seenAt = -1;
        for ( int i = 0; i < 8000; ++i )
        {
            float x = ( i == 0 ) ? 1.0f : 0.0f;
            float y = p.processSample( x, 999.0f, sr );   // offset clamps delay to 100ms
            if ( y > 0.5f && seenAt < 0 ) seenAt = i;
        }
        printf( "  huge offset -> delay %d samples (100ms clamp, buffer max %d)\n", seenAt, p3delay::kMaxSamples - 1 );
        check( seenAt == expected, "delay time clamps at 100ms regardless of a larger offset" );
    }

    // --- Max delay scales with the internal rate (oversampling): at 4x
    //     (192kHz) a 100ms request can't fit the base-rate buffer, so it's
    //     clamped to buffer length = 4799 samples = ~25ms @192kHz. ---
    {
        p3delay::Processor p; p.randomBaseFn = pinnedBase;
        g_baseR = rForMs( 100.0f );
        p.reset(); p.randomBaseFn = pinnedBase; p.onNewRender( 1, 0 );
        float internal = 192000.0f;   // 4x oversample
        int seenAt = -1;
        for ( int i = 0; i < 8000; ++i )
        {
            float x = ( i == 0 ) ? 1.0f : 0.0f;
            float y = p.processSample( x, 0.0f, internal );
            if ( y > 0.5f && seenAt < 0 ) seenAt = i;
        }
        float ms = seenAt * 1000.0f / internal;
        printf( "  100ms request @192kHz (4x) -> %d samples = %.2fms (buffer-limited)\n", seenAt, ms );
        check( seenAt == p3delay::kMaxSamples - 1, "at 4x the 100ms request is buffer-clamped (~25ms), as documented" );
    }

    // --- Base randomization lands in 1-100ms for various seeds (default fn). ---
    {
        bool inRange = true;
        for ( uint32_t seed = 1; seed <= 50; ++seed )
        {
            p3delay::Processor p;   // default randomBaseFn
            p.onNewRender( seed, 0 );
            // Measure base by sending an impulse with 0 offset.
            int seenAt = -1;
            for ( int i = 0; i < 6000; ++i )
            {
                float x = ( i == 0 ) ? 1.0f : 0.0f;
                float y = p.processSample( x, 0.0f, sr );
                if ( y > 0.5f && seenAt < 0 ) seenAt = i;
            }
            float ms = seenAt * 1000.0f / sr;
            if ( ms < p3delay::kMinMs - 0.5f || ms > p3delay::kMaxMs + 0.5f ) inRange = false;
        }
        check( inRange, "randomized base delay lands within 1-100ms for 50 seeds" );
    }

    printf( "\n%s (%d failures)\n", g_failures == 0 ? "DELAY: TESTS PASS" : "DELAY: FAILURES", g_failures );
    return g_failures == 0 ? 0 : 1;
}
