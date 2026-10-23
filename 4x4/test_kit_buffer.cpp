// test_kit_buffer.cpp — desktop validation for kit_buffer.h
// (Implementation Plan Step 13). Confirms 12-bit pack/unpack round-trip
// accuracy against float32 test signals, write-order independence, and
// the double-buffer swap mechanics.
// Build: g++ -std=c++11 -O2 -o test_kit_buffer test_kit_buffer.cpp
#include "kit_buffer.h"
#include <cstdio>
#include <cmath>

static int g_failures = 0;
static void check( bool cond, const char* what ) { printf( "  %s: %s\n", cond ? "ok" : "FAIL", what ); if ( !cond ) ++g_failures; }

int main()
{
    using namespace kitbuffer;

    printf( "kNumSamples=%d, kBytesPerBuffer=%d, kDurationSec=%.6f\n\n", kNumSamples, kBytesPerBuffer, kDurationSec );
    check( kNumSamples % 2 == 0, "kNumSamples is even (clean pairing, no incomplete trailing pair)" );
    check( kBytesPerBuffer <= 226784, "kBytesPerBuffer fits within the documented 221.46875KB-per-buffer budget" );
    check( fabsf( kDurationSec - 4.40625f ) < 1e-4f, "kDurationSec matches the derived figure (4.40625s, trimmed 2026-10-20 for DRAM margin)" );

    // --- 12-bit pack/unpack round-trip, raw level values across the full
    //     signed 12-bit range. ---
    {
        bool allExact = true;
        for ( int a = -2048; a <= 2047; a += 37 )
        {
            for ( int b = -2048; b <= 2047; b += 211 )
            {
                uint8_t group[3];
                pack12( (int16_t)a, (int16_t)b, group );
                int16_t ra, rb;
                unpack12( group, ra, rb );
                if ( ra != a || rb != b ) allExact = false;
            }
        }
        check( allExact, "pack12/unpack12 round-trips every sampled (a,b) pair exactly" );
    }

    // --- Quantization error on a float32 sine sweep: confirm the error is
    //     in the expected ~12-bit range (step size = fullScale/2047). ---
    {
        const float kTwoPi = 6.283185307f;
        float maxErr = 0.0f;
        const int N = 10000;
        for ( int i = 0; i < N; ++i )
        {
            float x = kFullScaleVolts * sinf( kTwoPi * 3.7f * i / N );   // sweep the full range
            int16_t level = KitBuffer::voltsToLevel( x );
            float y = KitBuffer::levelToVolts( level );
            float err = fabsf( y - x );
            if ( err > maxErr ) maxErr = err;
        }
        float expectedStep = kFullScaleVolts / 2047.0f;
        printf( "Quantization: max error = %.6f V, one 12-bit step = %.6f V\n", maxErr, expectedStep );
        check( maxErr <= expectedStep * 0.51f, "max quantization error stays within one 12-bit step (round-to-nearest)" );
        check( maxErr > expectedStep * 0.01f, "quantization error is nonzero (confirms real quantization is happening, not a silent passthrough)" );
    }

    // --- KitBuffer write/read round-trip through the full buffer, and
    //     write-order independence (write odd indices first, then even). ---
    {
        KitBuffer kb;
        kb.clear();
        bool roundTripOk = true;
        for ( int i = 0; i < kNumSamples; ++i )
        {
            float x = kFullScaleVolts * sinf( 0.0017f * i );
            kb.write( i, x );
        }
        float maxErr = 0.0f;
        for ( int i = 0; i < kNumSamples; ++i )
        {
            float x = kFullScaleVolts * sinf( 0.0017f * i );
            float y = kb.read( i );
            float err = fabsf( y - x );
            if ( err > maxErr ) maxErr = err;
            if ( err > kFullScaleVolts / 2047.0f * 0.51f ) roundTripOk = false;
        }
        printf( "\nFull-buffer round trip: max error = %.6f V\n", maxErr );
        check( roundTripOk, "every sample in the full buffer round-trips within one quantization step" );

        // Now write in reverse order (odd before even within each pair) and
        // confirm identical results.
        KitBuffer kb2;
        kb2.clear();
        for ( int i = 1; i < kNumSamples; i += 2 )
        {
            float x = kFullScaleVolts * sinf( 0.0017f * i );
            kb2.write( i, x );
        }
        for ( int i = 0; i < kNumSamples; i += 2 )
        {
            float x = kFullScaleVolts * sinf( 0.0017f * i );
            kb2.write( i, x );
        }
        bool orderIndependent = true;
        for ( int i = 0; i < kNumSamples; ++i )
            if ( fabsf( kb.read( i ) - kb2.read( i ) ) > 1e-6f ) orderIndependent = false;
        check( orderIndependent, "writing odd-before-even within each pair gives identical results to even-before-odd" );
    }

    // --- Double buffer: renderer writes the inactive buffer, swap() makes
    //     it current atomically; the player's view never changes until
    //     swap() is called, and the other buffer is untouched by writes
    //     to the first. ---
    {
        DoubleBufferedKit db;
        db.inactive().write( 0, 3.0f );
        check( fabsf( db.current().read( 0 ) ) < 1e-6f, "writes to the inactive buffer are invisible to current() before swap()" );
        db.swap();
        check( fabsf( db.current().read( 0 ) - 3.0f ) < 0.01f, "current() reflects the written value immediately after swap()" );

        db.inactive().write( 0, -5.0f );   // now writing what was buffer A (now inactive again)
        check( fabsf( db.current().read( 0 ) - 3.0f ) < 0.01f, "current() is unaffected by writes to the newly-inactive buffer" );
        db.swap();
        check( fabsf( db.current().read( 0 ) - ( -5.0f ) ) < 0.01f, "second swap() reveals the second render's value" );
    }

    printf( "\n%s (%d failures)\n", g_failures == 0 ? "KIT BUFFER: TESTS PASS" : "KIT BUFFER: FAILURES", g_failures );
    return g_failures == 0 ? 0 : 1;
}
