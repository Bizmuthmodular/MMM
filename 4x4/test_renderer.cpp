// test_renderer.cpp — desktop validation for renderer.h (Implementation
// Plan Step 14). No explicit validation criterion was spelled out in the
// plan beyond "implement," but per this project's established practice,
// everything testable gets validated before moving on: a full render
// actually completes in the expected number of chunks, produces exactly
// kNumSamples of bounded, non-silent audio, the render-to-kit-rate
// fractional decimator holds at a non-integer ratio (44.1kHz), and a
// second render after the first behaves cleanly (confirms cold-reset
// processor state doesn't leak or diverge across renders).
// Build: g++ -std=c++11 -O2 -o test_renderer test_renderer.cpp
#include "fft.h"
#include "renderer.h"
#include <cstdio>
#include <cmath>

static int g_failures = 0;
static void check( bool cond, const char* what ) { printf( "  %s: %s\n", cond ? "ok" : "FAIL", what ); if ( !cond ) ++g_failures; }

static bool runFullRender( renderer::Renderer& r, kitbuffer::DoubleBufferedKit& kit, int& outChunks )
{
    r.startNewRender();
    int chunks = 0;
    bool done = false;
    const int kSafetyCap = kitbuffer::kNumSamples * 2 + 1000;   // generous upper bound on chunk count
    while ( !done && chunks < kSafetyCap )
    {
        done = r.advanceChunk( kit );
        ++chunks;
    }
    outChunks = chunks;
    return done;
}

int main()
{
    fft::initTwiddles();

    // --- Full render at 48kHz: completes, produces bounded, non-silent audio. ---
    {
        renderer::Renderer r;
        kitbuffer::DoubleBufferedKit kit;
        r.initialise( /*baseSeed=*/123, /*renderSampleRate=*/48000.0f );

        check( !r.rendering(), "renderer starts idle" );
        int chunks = 0;
        bool completed = runFullRender( r, kit, chunks );
        printf( "48kHz render completed in %d advanceChunk() calls (expected ~%d)\n",
                chunks, (int)( kitbuffer::kNumSamples * 1.5f / renderer::kChunkSize ) );
        check( completed, "a full render completes (reaches kNumSamples and swaps)" );
        check( !r.rendering(), "renderer returns to idle after completion" );

        float maxAbs = 0.0f, sumAbs = 0.0f;
        bool bounded = true;
        for ( int i = 0; i < kitbuffer::kNumSamples; ++i )
        {
            float v = kit.current().read( i );
            float a = fabsf( v );
            if ( a > maxAbs ) maxAbs = a;
            sumAbs += a;
            if ( a > 11.0f + 1e-3f ) bounded = false;
        }
        printf( "Kit audio: max|v|=%.4f, mean|v|=%.4f\n", maxAbs, sumAbs / kitbuffer::kNumSamples );
        check( bounded, "every kit sample stays within +-11V" );
        check( maxAbs > 0.05f, "the rendered kit is not silent (starter wave + feedback produced real signal)" );
    }

    // --- Loop-seam fade + anti-clip (2026-10-21): the baked kit must start
    //     AND end at ~0 so the player's loop seam (kit[N-1]->kit[0]) is
    //     click-free, and no sample may sit on the +-11V hard-clip rail (the
    //     mix is soft-clipped before the kit write, so it only ever asymptotes
    //     toward 11V, never pins there). ---
    {
        renderer::Renderer r;
        kitbuffer::DoubleBufferedKit kit;
        r.initialise( /*baseSeed=*/321, /*renderSampleRate=*/48000.0f );
        int chunks = 0;
        runFullRender( r, kit, chunks );
        const int N = kitbuffer::kNumSamples;
        float first = kit.current().read( 0 );
        float last  = kit.current().read( N - 1 );
        // A mid-kit reference level, to show the edges really were faded (not
        // just a coincidentally-silent render).
        float mid = 0.0f;
        for ( int i = N / 4; i < N / 4 + 2000; ++i ) mid += fabsf( kit.current().read( i ) );
        mid /= 2000.0f;
        printf( "\nSeam: kit[0]=%.5f, kit[N-1]=%.5f, mid mean|v|=%.4f\n", first, last, mid );
        check( fabsf( first ) < 0.05f, "kit starts at ~0 (fade-in): no seam step from the head" );
        check( fabsf( last ) < 0.05f, "kit ends at ~0 (fade-out): the ringing tail is faded so the loop seam is silent" );
        check( fabsf( last - first ) < 0.1f, "loop seam kit[N-1]->kit[0] is continuous (no click)" );
        check( mid > 0.05f, "the kit is genuinely non-silent away from the faded edges (fade didn't just zero everything)" );

        float maxAbs = 0.0f; int railHits = 0;
        for ( int i = 0; i < N; ++i )
        {
            float a = fabsf( kit.current().read( i ) );
            if ( a > maxAbs ) maxAbs = a;
            if ( a >= 11.0f - 1e-3f ) ++railHits;   // pinned at the hard-clip rail
        }
        printf( "Clip: max|v|=%.4f, rail hits=%d\n", maxAbs, railHits );
        check( railHits == 0, "no sample sits on the +-11V hard-clip rail (soft-clip prevents brick-wall clipping pops)" );
        check( maxAbs < 11.0f, "kit stays strictly below the +-11V ceiling (soft-clip asymptote)" );
    }

    // --- Peak normalization (2026-10-21): the kit, scaled by playbackGain(),
    //     must peak at exactly the set Norm level, and never exceed it — the
    //     clipping-headroom guarantee. Also checked: changing the level
    //     rescales live (no re-bake), and level 0 -> silence. ---
    {
        renderer::Renderer r;
        kitbuffer::DoubleBufferedKit kit;
        r.initialise( /*baseSeed=*/4040, /*renderSampleRate=*/48000.0f );
        r.setNormalizeLevel( 6.0f );
        int chunks = 0;
        runFullRender( r, kit, chunks );
        float peak = r.currentKitPeak();
        check( peak > 0.1f, "baked kit has a real (tracked) peak" );

        auto normalizedPeak = [&]() {
            float g = r.playbackGain();
            float mx = 0.0f;
            for ( int i = 0; i < kitbuffer::kNumSamples; ++i )
            {
                float v = fabsf( kit.current().read( i ) ) * g;
                if ( v > mx ) mx = v;
            }
            return mx;
        };
        float p6 = normalizedPeak();
        printf( "\nNormalize: raw peak=%.4f, normalized peak @6V=%.4f\n", peak, p6 );
        check( fabsf( p6 - 6.0f ) < 0.05f, "kit normalized to 6V peaks at 6V" );
        check( p6 <= 6.0f + 1e-3f, "normalized kit never exceeds the set level (headroom guarantee)" );

        // Live rescale: change the level, same baked kit -> new peak, no re-bake.
        r.setNormalizeLevel( 10.0f );
        float p10 = normalizedPeak();
        check( fabsf( p10 - 10.0f ) < 0.05f, "changing Norm level rescales the SAME kit live (no re-bake needed)" );

        r.setNormalizeLevel( 0.0f );
        check( normalizedPeak() < 1e-4f, "Norm level 0 -> silence" );
    }

    // --- Non-integer decimation ratio (44.1kHz): still produces exactly
    //     kNumSamples kit samples via the fractional accumulator. ---
    {
        renderer::Renderer r;
        kitbuffer::DoubleBufferedKit kit;
        r.initialise( /*baseSeed=*/7, /*renderSampleRate=*/44100.0f );
        int chunks = 0;
        bool completed = runFullRender( r, kit, chunks );
        printf( "\n44.1kHz render completed in %d advanceChunk() calls\n", chunks );
        check( completed, "render completes at a non-integer render/kit rate ratio (44100/32000=1.378125)" );
        float maxAbs = 0.0f;
        for ( int i = 0; i < kitbuffer::kNumSamples; ++i )
        {
            float a = fabsf( kit.current().read( i ) );
            if ( a > maxAbs ) maxAbs = a;
        }
        check( maxAbs > 0.05f, "44.1kHz render is not silent" );
    }

    // --- Second render after the first: confirms cold-reset processor
    //     state doesn't leak/diverge (also completes cleanly, bounded). ---
    {
        renderer::Renderer r;
        kitbuffer::DoubleBufferedKit kit;
        r.initialise( /*baseSeed=*/55, /*renderSampleRate=*/48000.0f );
        int chunks1 = 0, chunks2 = 0;
        bool c1 = runFullRender( r, kit, chunks1 );
        bool c2 = runFullRender( r, kit, chunks2 );
        printf( "\nTwo consecutive renders: chunks1=%d, chunks2=%d\n", chunks1, chunks2 );
        check( c1 && c2, "two consecutive renders both complete cleanly" );
        check( r.renderIndex() == 2, "renderIndex increments once per startNewRender() call (now at 2)" );

        bool bounded = true;
        for ( int i = 0; i < kitbuffer::kNumSamples; ++i )
            if ( fabsf( kit.current().read( i ) ) > 11.0f + 1e-3f ) bounded = false;
        check( bounded, "the second render's audio also stays within +-11V (no divergence carried from the first)" );
    }

    // --- advanceChunk() is a no-op when idle (doesn't crash, doesn't
    //     advance anything, returns false). ---
    {
        renderer::Renderer r;
        kitbuffer::DoubleBufferedKit kit;
        r.initialise( 1, 48000.0f );
        bool result = r.advanceChunk( kit );
        check( !result, "advanceChunk() on an idle renderer returns false (no-op)" );
    }

    printf( "\n%s (%d failures)\n", g_failures == 0 ? "RENDERER: TESTS PASS" : "RENDERER: FAILURES", g_failures );
    return g_failures == 0 ? 0 : 1;
}
