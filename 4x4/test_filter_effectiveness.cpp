// test_filter_effectiveness.cpp — answers the user's doubt ("are the filters
// really effective?") for the NEW per-channel resonant bandpass. Renders a
// full kit via renderer::Renderer at a fixed seed, twice: once with all four
// bandpasses NARROW around a center frequency, once WIDE (near-wideband),
// everything else identical. FFTs the rendered 32kHz kit and shows the narrow
// setting concentrates the kit's energy in the band (much higher in-band
// energy fraction, much lower spectral centroid) — i.e. the bandpass audibly
// and strongly shapes the output, unlike the gentle one-poles it replaced.
// Build: g++ -std=c++11 -O2 -o test_filter_effectiveness test_filter_effectiveness.cpp
#include "fft.h"
#include "renderer.h"
#include <cstdio>
#include <cmath>

static int g_failures = 0;
static void check( bool cond, const char* what ) { printf( "  %s: %s\n", cond ? "ok" : "FAIL", what ); if ( !cond ) ++g_failures; }

static const float kCenter = 500.0f;

// Render a full kit with all 4 bandpasses at (center, bwSemis), fixed seed.
static void renderKit( kitbuffer::DoubleBufferedKit& kit, float bwSemis )
{
    renderer::Renderer r;
    r.initialise( 2024, 48000.0f );
    for ( int c = 0; c < 4; ++c ) { r.setBandpassFreq( c, kCenter ); r.setBandpassBw( c, bwSemis ); }
    r.startNewRender();
    int guard = 0;
    while ( !r.advanceChunk( kit ) && guard++ < 400000 ) {}
}

// Averaged magnitude spectrum stats over several windows of the kit:
// in-band energy fraction (|f-center| within +-1 octave) and spectral centroid.
static void spectralStats( const kitbuffer::KitBuffer& kb, float& inBandFrac, float& centroidHz )
{
    const int kN = 2048;
    const float sr = (float)kitbuffer::kSampleRateHz;
    const float loHz = kCenter / 2.0f, hiHz = kCenter * 2.0f;   // +-1 octave band
    double inBand = 0.0, total = 0.0, cWeighted = 0.0;
    fft::Complex a[kN];
    // Average over windows spanning the body of the kit.
    for ( int w = 0; w < 24; ++w )
    {
        int off = ( kitbuffer::kNumSamples / 26 ) * ( w + 1 );
        for ( int i = 0; i < kN; ++i )
        {
            float win = 0.5f - 0.5f * cosf( 6.283185307f * i / ( kN - 1 ) );
            float s = ( off + i < kitbuffer::kNumSamples ) ? kb.read( off + i ) : 0.0f;
            a[i].re = s * win; a[i].im = 0.0f;
        }
        fft::transform( a, kN, false );
        for ( int i = 1; i < kN / 2; ++i )
        {
            float f = i * sr / kN;
            float mag = sqrtf( a[i].re * a[i].re + a[i].im * a[i].im );
            total += mag; cWeighted += (double)f * mag;
            if ( f >= loHz && f <= hiHz ) inBand += mag;
        }
    }
    inBandFrac = ( total > 0.0 ) ? (float)( inBand / total ) : 0.0f;
    centroidHz = ( total > 0.0 ) ? (float)( cWeighted / total ) : 0.0f;
}

int main()
{
    fft::initTwiddles();
    static kitbuffer::DoubleBufferedKit kitNarrow, kitWide;

    renderKit( kitNarrow, 6.0f );    // narrow: half-octave-ish band around 500Hz
    renderKit( kitWide, 96.0f );     // wide: 8 octaves ~ near-wideband

    float nFrac, nCentroid, wFrac, wCentroid;
    spectralStats( kitNarrow.current(), nFrac, nCentroid );
    spectralStats( kitWide.current(), wFrac, wCentroid );

    printf( "Narrow BP (6 st @500Hz): in-band[250-1000Hz] energy frac=%.3f, centroid=%.0fHz\n", nFrac, nCentroid );
    printf( "Wide   BP (96 st)      : in-band[250-1000Hz] energy frac=%.3f, centroid=%.0fHz\n", wFrac, wCentroid );
    printf( "-> narrow concentrates %.1fx more of its energy in the band, centroid %.0fHz lower\n",
            wFrac > 1e-6f ? nFrac / wFrac : 0.0f, wCentroid - nCentroid );

    check( nFrac > wFrac * 1.5f, "narrow bandpass puts substantially more kit energy in the target band than wide" );
    check( nCentroid < wCentroid, "narrow bandpass pulls the kit's spectral centroid down toward the band" );

    // Both kits must be bounded + non-silent (loop stable through the filter).
    bool ok = true;
    for ( int i = 0; i < kitbuffer::kNumSamples; i += 37 )
    {
        float a = fabsf( kitNarrow.current().read( i ) ), b = fabsf( kitWide.current().read( i ) );
        if ( std::isnan( a ) || std::isnan( b ) || a > 11.0f + 1e-3f || b > 11.0f + 1e-3f ) ok = false;
    }
    check( ok, "both narrow and wide renders stay bounded (<=11V) and NaN-free" );

    // Narrow, highly-resonant band inside the feedback loop stays stable.
    {
        static kitbuffer::DoubleBufferedKit kitRes;
        renderKit( kitRes, 1.0f );   // 1 semitone -> extreme resonance
        bool resOk = true; float mx = 0.0f;
        for ( int i = 0; i < kitbuffer::kNumSamples; i += 11 )
        {
            float v = kitRes.current().read( i );
            if ( std::isnan( v ) || fabsf( v ) > 11.0f + 1e-3f ) resOk = false;
            if ( fabsf( v ) > mx ) mx = fabsf( v );
        }
        printf( "1-semitone (extreme resonance) render: max|v|=%.3f\n", mx );
        check( resOk, "a 1-semitone resonant bandpass in the loop stays bounded (Reg contains it)" );
    }

    printf( "\n%s (%d failures)\n", g_failures == 0 ? "FILTER EFFECTIVENESS: TESTS PASS" : "FILTER EFFECTIVENESS: FAILURES", g_failures );
    return g_failures == 0 ? 0 : 1;
}
