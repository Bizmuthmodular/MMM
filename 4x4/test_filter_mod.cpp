// test_filter_mod.cpp — desktop validation for the VCFRND / QCONT filter
// modulation features (added 2026-10-21).
//
// VCFRND: when on, each render draws fresh seed-derived bandpass CENTER
//         frequencies, overriding the static Freq params.
// QCONT:  when on, a single shared Contour curve drives all 4 bandpass
//         BANDWIDTHS ("Q") per internal sample, overriding the static BW
//         params (biquad coeffs recomputed every sample).
//
// Both must (a) be DETERMINISTIC — identical seed+render => identical kit,
// preserving this project's reproducibility guarantee; (b) actually CHANGE
// the baked sound vs the static-filter path (isolated: same seed, same
// render index, only the feature toggled); and (c) stay bounded / NaN-free
// / non-silent — QCONT in particular sweeps the biquad coeffs every sample
// inside the resonant feedback loop, which must remain stable.
//
// Build: g++ -std=c++11 -O2 -I. -o test_filter_mod test_filter_mod.cpp
#include "fft.h"
#include "renderer.h"
#include "kit_buffer.h"
#include <cstdio>
#include <cmath>

static int g_failures = 0;
static void check( bool cond, const char* what ) { printf( "  %s: %s\n", cond ? "ok" : "FAIL", what ); if ( !cond ) ++g_failures; }

// Four scratch kits (each ~141k samples) + analysis buffers, kept as file
// statics to avoid big stack frames.
static float g_a[kitbuffer::kNumSamples];
static float g_b[kitbuffer::kNumSamples];

struct KitStats { float rms; float maxAbs; bool anyNaN; };

// Render one full kit (render index advances by one) and copy it out to dst,
// returning summary stats.
static KitStats renderKitTo( renderer::Renderer& r, float* dst )
{
    kitbuffer::DoubleBufferedKit kit;
    r.startNewRender();
    while ( !r.advanceChunk( kit ) ) { /* drain */ }
    KitStats st; st.rms = 0.0f; st.maxAbs = 0.0f; st.anyNaN = false;
    double acc = 0.0;
    for ( int i = 0; i < kitbuffer::kNumSamples; ++i )
    {
        float v = kit.current().read( i );
        dst[i] = v;
        if ( std::isnan( v ) ) st.anyNaN = true;
        float a = fabsf( v );
        if ( a > st.maxAbs ) st.maxAbs = a;
        acc += (double)v * (double)v;
    }
    st.rms = (float)sqrt( acc / (double)kitbuffer::kNumSamples );
    return st;
}

static float rmsDiff( const float* x, const float* y )
{
    double acc = 0.0;
    for ( int i = 0; i < kitbuffer::kNumSamples; ++i )
    {
        double d = (double)x[i] - (double)y[i];
        acc += d * d;
    }
    return (float)sqrt( acc / (double)kitbuffer::kNumSamples );
}

static float maxAbsDiff( const float* x, const float* y )
{
    float m = 0.0f;
    for ( int i = 0; i < kitbuffer::kNumSamples; ++i )
    {
        float d = fabsf( x[i] - y[i] );
        if ( d > m ) m = d;
    }
    return m;
}

static void configureStaticFilters( renderer::Renderer& r, float freqHz, float bwSemis )
{
    for ( int c = 0; c < 4; ++c ) { r.setBandpassFreq( c, freqHz ); r.setBandpassBw( c, bwSemis ); }
}

int main()
{
    fft::initTwiddles();
    const float sr = 48000.0f;

    // ---- VCFRND: reproducible, audibly distinct from the static-Freq path,
    //      and safe. Same seed + render#0, only VCFRND toggled. ----
    {
        renderer::Renderer rOff; rOff.initialise( 777, sr );
        configureStaticFilters( rOff, 400.0f, 12.0f );   // all 4 at 400Hz, 1-octave band
        rOff.setVcfRnd( false ); rOff.setQCont( false );
        KitStats sOff = renderKitTo( rOff, g_a );         // render#0, static cutoffs

        renderer::Renderer rOn1; rOn1.initialise( 777, sr );
        configureStaticFilters( rOn1, 400.0f, 12.0f );
        rOn1.setVcfRnd( true ); rOn1.setQCont( false );
        KitStats sOn1 = renderKitTo( rOn1, g_b );         // render#0, random cutoffs
        float vcfDiff = rmsDiff( g_a, g_b );
        float offRms = sOff.rms;

        check( !sOff.anyNaN && !sOn1.anyNaN, "VCFRND: neither static nor randomized render produces NaN" );
        check( sOn1.maxAbs <= 11.0f + 1e-3f, "VCFRND on: baked kit stays within +-11V" );
        check( sOn1.rms > 0.01f, "VCFRND on: kit is non-silent" );
        check( vcfDiff > 0.05f * offRms, "VCFRND on vs off (same seed, render#0) materially changes the kit" );
        printf( "  VCFRND: offRMS=%.4f  rmsDiff(on,off)=%.4f  (%.0f%% of signal)\n",
                offRms, vcfDiff, offRms > 1e-9f ? 100.0f * vcfDiff / offRms : 0.0f );

        // Reproducibility: a second identically-seeded VCFRND-on render#0 must
        // be bit-for-bit (within float eps) identical — random cutoffs are
        // seed-derived, not wall-clock.
        renderer::Renderer rOn2; rOn2.initialise( 777, sr );
        configureStaticFilters( rOn2, 400.0f, 12.0f );
        rOn2.setVcfRnd( true ); rOn2.setQCont( false );
        KitStats sOn2 = renderKitTo( rOn2, g_a );
        (void)sOn2;
        check( maxAbsDiff( g_a, g_b ) < 1e-5f, "VCFRND on: same seed+render reproduces an identical kit (deterministic)" );
    }

    // ---- QCONT: reproducible, audibly distinct from the static-BW path, and
    //      — crucially — STABLE despite per-sample biquad coeff sweeping
    //      inside the resonant feedback loop. Same seed + render#0. ----
    {
        renderer::Renderer rOff; rOff.initialise( 555, sr );
        configureStaticFilters( rOff, 800.0f, 6.0f );    // fairly high-Q static band
        rOff.setVcfRnd( false ); rOff.setQCont( false );
        KitStats sOff = renderKitTo( rOff, g_a );

        renderer::Renderer rOn1; rOn1.initialise( 555, sr );
        configureStaticFilters( rOn1, 800.0f, 6.0f );
        rOn1.setVcfRnd( false ); rOn1.setQCont( true );
        KitStats sOn1 = renderKitTo( rOn1, g_b );
        float qDiff = rmsDiff( g_a, g_b );
        float offRms = sOff.rms;

        check( !sOn1.anyNaN, "QCONT on: per-sample coeff sweep produces no NaN" );
        check( sOn1.maxAbs <= 11.0f + 1e-3f, "QCONT on: resonant loop with swept Q stays within +-11V (stable)" );
        check( sOn1.rms > 0.01f, "QCONT on: kit is non-silent" );
        check( qDiff > 0.05f * offRms, "QCONT on vs off (same seed, render#0) materially changes the kit" );
        printf( "  QCONT:  offRMS=%.4f  rmsDiff(on,off)=%.4f  (%.0f%% of signal)\n",
                offRms, qDiff, offRms > 1e-9f ? 100.0f * qDiff / offRms : 0.0f );

        renderer::Renderer rOn2; rOn2.initialise( 555, sr );
        configureStaticFilters( rOn2, 800.0f, 6.0f );
        rOn2.setVcfRnd( false ); rOn2.setQCont( true );
        renderKitTo( rOn2, g_a );
        check( maxAbsDiff( g_a, g_b ) < 1e-5f, "QCONT on: same seed+render reproduces an identical kit (deterministic)" );
    }

    // ---- Both on together: still bounded / finite / non-silent (the combined
    //      path — random cutoffs + per-sample Q sweep — must not destabilize). ----
    {
        renderer::Renderer r; r.initialise( 999, sr );
        configureStaticFilters( r, 1000.0f, 4.0f );
        r.setVcfRnd( true ); r.setQCont( true );
        KitStats s = renderKitTo( r, g_a );
        check( !s.anyNaN, "VCFRND+QCONT both on: no NaN" );
        check( s.maxAbs <= 11.0f + 1e-3f, "VCFRND+QCONT both on: stays within +-11V" );
        check( s.rms > 0.01f, "VCFRND+QCONT both on: kit is non-silent" );
    }

    printf( "\n%s (%d failures)\n", g_failures == 0 ? "FILTER MOD (VCFRND/QCONT): TESTS PASS" : "FILTER MOD (VCFRND/QCONT): FAILURES", g_failures );
    return g_failures == 0 ? 0 : 1;
}
