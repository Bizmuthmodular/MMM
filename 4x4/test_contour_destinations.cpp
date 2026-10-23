// test_contour_destinations.cpp — desktop validation for
// contour_destinations.h (Implementation Plan Step 12).
// Build: g++ -std=c++11 -O2 -o test_contour_destinations test_contour_destinations.cpp
#include "fft.h"
#include "contour_destinations.h"
#include "kit_buffer.h"
#include <cstdio>
#include <cmath>

static int g_failures = 0;
static void check( bool cond, const char* what ) { printf( "  %s: %s\n", cond ? "ok" : "FAIL", what ); if ( !cond ) ++g_failures; }

int main()
{
    fft::initTwiddles();
    using namespace contourdest;
    const float sr = 48000.0f;
    const float kitLengthSec = kitbuffer::kDurationSec;   // single source of truth (§8)

    ContourDestinations cd;
    SharedSettings shared;   // defaults
    cd.configure( shared, kitLengthSec, /*baseSeed=*/1000, sr );
    cd.onNewRender( /*renderSeed=*/1000 );

    // --- Single-contour playheads (2026-10-21): the 16 crosspoint heads read
    //     ONE shared curve at distinct phase offsets, so at any instant they
    //     should NOT all be identical (that would mean the offsets collapsed
    //     or the curve is flat — i.e. destinations moving in lockstep). ---
    {
        cd.advance();
        bool allIdentical = true;
        float first = cd.crosspointGain( 0, 0 );
        for ( int row = 0; row < 4 && allIdentical; ++row )
            for ( int col = 0; col < 4; ++col )
                if ( fabsf( cd.crosspointGain( row, col ) - first ) > 1e-6f ) { allIdentical = false; break; }
        check( !allIdentical, "16 crosspoint playheads produce distinct values (distinct phase offsets, not lockstep)" );
    }

    // --- Long-run mapping-range bounds check across many advance() calls. ---
    {
        bool crosspointBounded = true;
        bool p1Bounded = true, p2Bounded = true, p3Bounded = true;
        bool p4FreqBounded = true;

        const int N = (int)( sr * kitLengthSec );
        for ( int i = 0; i < N; ++i )
        {
            cd.advance();
            for ( int row = 0; row < 4; ++row )
                for ( int col = 0; col < 4; ++col )
                    if ( fabsf( cd.crosspointGain( row, col ) ) > 1.0f + 1e-4f ) crosspointBounded = false;
            if ( fabsf( cd.p1DepthSemitones() ) > 12.0f + 1e-3f ) p1Bounded = false;
            if ( fabsf( cd.p2DepthHz() ) > 500.0f + 1e-2f ) p2Bounded = false;
            if ( fabsf( cd.p3DelayMsOffset() ) > 50.0f + 1e-2f ) p3Bounded = false;
            if ( fabsf( cd.p4FreqOffset() ) > 2000.0f + 1e-1f ) p4FreqBounded = false;
        }
        printf( "Ran %d samples (%.2fs) across all %d destinations.\n", N, kitLengthSec, contourdest::kNumDestinations );
        check( contourdest::kNumDestinations == 21, "destination count is 21 (16 crosspoints + P1/P2/P3 depth + P4 phaser sweep + shared filter-Q mod)" );
        check( crosspointBounded, "all 16 crosspoint gains stay within [-1,1] for the whole kit length" );
        check( p1Bounded, "P1 depth offset stays within +-12 semitones" );
        check( p2Bounded, "P2 depth offset stays within +-500Hz" );
        check( p3Bounded, "P3 delay offset stays within +-50ms" );
        check( p4FreqBounded, "P4 phaser center-freq offset stays within +-2000Hz" );
    }

    // --- Re-trigger (onNewRender) with a FRESH per-render seed should not
    //     crash and should keep output bounded (new gesture each render). ---
    {
        cd.onNewRender( /*renderSeed=*/1001 );
        bool stillBounded = true;
        for ( int i = 0; i < 1000; ++i )
        {
            cd.advance();
            if ( fabsf( cd.crosspointGain( 0, 0 ) ) > 1.0f + 1e-4f ) stillBounded = false;
        }
        check( stillBounded, "output stays bounded immediately after a re-trigger (onNewRender)" );
    }

    // --- Wiring glue: fillChannelControls/fillMatrix should populate a
    //     real ChannelControls/FeedbackNetwork without crashing, and the
    //     resulting network should still run stably. ---
    {
        feedbacknet::FeedbackNetwork net;
        net.onNewRender( 99 );
        feedbacknet::ChannelControls ctrl;
        bool anyNaN = false;
        for ( int i = 0; i < 48000; ++i )
        {
            cd.advance();
            cd.fillChannelControls( ctrl );
            cd.fillMatrix( net );
            float out[4];
            net.processSample( sr, ctrl, out );
            for ( int ch = 0; ch < 4; ++ch ) if ( std::isnan( out[ch] ) ) anyNaN = true;
        }
        check( !anyNaN, "fillChannelControls/fillMatrix wiring drives a real FeedbackNetwork for 1s with no NaN" );
    }

    // --- Shared-curve property: every playhead reads ONE curve at a phase
    //     offset, and since the base phase sweeps a full [0,1) each head's
    //     (base+offset) also covers [0,1) fully — so over a complete pass all
    //     16 crosspoint heads must see the SAME value range (same min & max).
    //     This simultaneously proves "one shared slope" and "full 360 pass". ---
    {
        ContourDestinations cd2;
        SharedSettings sh2;
        cd2.configure( sh2, kitLengthSec, /*baseSeed=*/2000, sr );
        cd2.onNewRender( /*renderSeed=*/2000 );
        float mn[16], mx[16];
        for ( int h = 0; h < 16; ++h ) { mn[h] = 1e9f; mx[h] = -1e9f; }
        const int N = (int)( sr * kitLengthSec );
        for ( int i = 0; i < N; ++i )
        {
            cd2.advance();
            for ( int row = 0; row < 4; ++row )
                for ( int col = 0; col < 4; ++col )
                {
                    float raw = cd2.crosspointGain( row, col ) * 5.0f;   // back to +-5V curve units
                    int h = row * 4 + col;
                    if ( raw < mn[h] ) mn[h] = raw;
                    if ( raw > mx[h] ) mx[h] = raw;
                }
        }
        bool sameRange = true;
        for ( int h = 1; h < 16; ++h )
            if ( fabsf( mn[h] - mn[0] ) > 0.05f || fabsf( mx[h] - mx[0] ) > 0.05f ) sameRange = false;
        check( sameRange, "all 16 playheads traverse the same shared curve (identical value range over a full pass)" );
        check( mx[0] - mn[0] > 0.1f, "the shared curve actually varies over the pass (non-degenerate modulation)" );
    }

    // --- Determinism: same render seed reproduces identical playhead values. ---
    {
        ContourDestinations a, b;
        SharedSettings sh;
        a.configure( sh, kitLengthSec, /*baseSeed=*/4242, sr ); a.onNewRender( /*renderSeed=*/4242 );
        b.configure( sh, kitLengthSec, /*baseSeed=*/4242, sr ); b.onNewRender( /*renderSeed=*/4242 );
        bool identical = true;
        for ( int i = 0; i < 20000 && identical; ++i )
        {
            a.advance(); b.advance();
            for ( int row = 0; row < 4 && identical; ++row )
                for ( int col = 0; col < 4; ++col )
                    if ( fabsf( a.crosspointGain( row, col ) - b.crosspointGain( row, col ) ) > 1e-6f ) { identical = false; break; }
        }
        check( identical, "same render seed reproduces identical playhead modulation (deterministic)" );
    }

    // --- Per-render randomisation (2026-10-21): a different render seed gives
    //     a DIFFERENT contour gesture (new curve + offsets), and re-seeding
    //     with the same render seed reproduces it exactly. ---
    {
        ContourDestinations cd3;
        SharedSettings sh3;
        cd3.configure( sh3, kitLengthSec, /*baseSeed=*/9000, sr );

        float seqA[2000];
        cd3.onNewRender( /*renderSeed=*/9000 );                       // "render 0"
        for ( int i = 0; i < 2000; ++i ) { cd3.advance(); seqA[i] = cd3.crosspointGain( 0, 0 ); }

        bool differs = false;
        cd3.onNewRender( /*renderSeed=*/9001 );                       // "render 1" (baseSeed+1)
        for ( int i = 0; i < 2000; ++i )
        {
            cd3.advance();
            if ( fabsf( cd3.crosspointGain( 0, 0 ) - seqA[i] ) > 0.05f ) differs = true;
        }
        check( differs, "a new per-render seed yields a different contour gesture (curve + offsets re-randomised each render)" );

        bool reproduces = true;
        cd3.onNewRender( /*renderSeed=*/9000 );                       // re-seed render 0
        for ( int i = 0; i < 2000; ++i )
        {
            cd3.advance();
            if ( fabsf( cd3.crosspointGain( 0, 0 ) - seqA[i] ) > 1e-6f ) reproduces = false;
        }
        check( reproduces, "re-seeding with the same render seed reproduces the identical gesture (deterministic across renders)" );
    }

    printf( "\nsizeof(ContourDestinations) on this platform (x86/ARM alignment may differ) = %zu bytes\n", sizeof( ContourDestinations ) );

    printf( "\n%s (%d failures)\n", g_failures == 0 ? "CONTOUR DESTINATIONS: TESTS PASS" : "CONTOUR DESTINATIONS: FAILURES", g_failures );
    return g_failures == 0 ? 0 : 1;
}
