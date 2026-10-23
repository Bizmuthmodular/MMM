// test_oversampling.cpp — proves the oversampling principle deterministically
// on the worst alias offender (P3 wavefolder), independent of the full
// generative kit. Drives a sine through the folder at a fold high enough
// that its harmonics land well above the 32kHz kit's 16kHz Nyquist, then
// produces a 32kHz output two ways:
//   (a) process AT 32kHz directly  -> harmonics alias (fold) into 0..16kHz
//   (b) process at 128kHz (4x), AA-lowpass, decimate to 32kHz -> cleaned
// and measures in-band alias energy via FFT. (b) must have substantially
// less energy at the aliased (inharmonic) frequencies.
//
// This mirrors exactly what renderer.h does (run the nonlinearity at the
// oversampled rate, then filters::AntiAliasLowpass before the decimator),
// but isolated to one memoryless stage so the comparison is apples-to-apples
// (the full network's output genuinely differs per rate, so it can't be
// compared this way — see §7/§10).
// Build: g++ -std=c++11 -O2 -o test_oversampling test_oversampling.cpp
#include "fft.h"
#include "p3_wavefolder.h"
#include "filters.h"
#include <cstdio>
#include <cmath>

static int g_failures = 0;
static void check( bool cond, const char* what ) { printf( "  %s: %s\n", cond ? "ok" : "FAIL", what ); if ( !cond ) ++g_failures; }

static const int kKitRate = 32000;
static const float kTwoPi = 6.283185307f;

// Produce N kit-rate (32kHz) samples of foldShape(sine*fold) at `osFactor`
// oversampling. osFactor=1 processes directly at 32kHz; >1 processes at
// osFactor*32kHz through an AntiAliasLowpass then integer-decimates.
static void renderFolded( float* kitOut, int nKit, float inHz, float fold, int osFactor )
{
    float internalRate = (float)( kKitRate * osFactor );
    filters::AntiAliasLowpass aa;
    aa.reset();
    double phase = 0.0;
    int kitIdx = 0;
    for ( int k = 0; k < nKit; ++k )
    {
        float acc = 0.0f;   // last filtered internal sample within this kit step
        for ( int s = 0; s < osFactor; ++s )
        {
            float x = sinf( (float)( kTwoPi * phase ) );
            float folded = p3wavefolder::process( x, fold, 0.0f );
            acc = aa.process( folded, internalRate );   // at 1x this still runs (gentle AA)
            phase += inHz / internalRate;
            if ( phase >= 1.0 ) phase -= 1.0;
        }
        kitOut[kitIdx++] = acc;   // take the latest filtered sample = decimate by osFactor
    }
}

// Sum of spectral magnitude at bins NOT near a harmonic of f0 (i.e. alias
// energy), over a 2048-pt FFT of a 32kHz signal.
static float aliasEnergy( const float* buf, int n, float f0Hz )
{
    const int kN = 2048;
    fft::Complex a[kN];
    int off = ( n - kN ) / 2; if ( off < 0 ) off = 0;
    for ( int i = 0; i < kN; ++i )
    {
        float w = 0.5f - 0.5f * cosf( kTwoPi * i / ( kN - 1 ) );
        a[i].re = ( off + i < n ? buf[off + i] : 0.0f ) * w;
        a[i].im = 0.0f;
    }
    fft::transform( a, kN, false );
    float binHz = (float)kKitRate / kN;
    float total = 0.0f;
    for ( int i = 1; i < kN / 2; ++i )
    {
        float f = i * binHz;
        // distance to nearest harmonic of f0
        float h = f / f0Hz;
        float nearest = roundf( h );
        bool isHarmonic = ( nearest >= 1.0f ) && ( fabsf( h - nearest ) < 0.08f );
        if ( !isHarmonic )
            total += sqrtf( a[i].re * a[i].re + a[i].im * a[i].im );
    }
    return total;
}

int main()
{
    fft::initTwiddles();

    // --- AA filter unit test: a tone above the 16kHz kit Nyquist is strongly
    //     attenuated; a tone well below passes near-unity. (Run at 128kHz,
    //     the kind of internal rate used at 4x.) ---
    {
        auto gainAt = [&]( float fHz ) {
            filters::AntiAliasLowpass aa; aa.reset();
            float sr = 128000.0f; float mx = 0.0f;
            int N = (int)( sr * 0.05f );
            for ( int i = 0; i < N; ++i ) { float y = aa.process( sinf( kTwoPi * fHz * i / sr ), sr ); if ( i > N / 2 && fabsf( y ) > mx ) mx = fabsf( y ); }
            return mx;
        };
        float gLow = gainAt( 2000.0f );
        float gHigh = gainAt( 20000.0f );
        printf( "AA filter @128kHz: gain@2kHz=%.3f, gain@20kHz=%.4f\n", gLow, gHigh );
        check( gLow > 0.9f, "AA filter passes 2kHz (well below cutoff) near-unity" );
        check( gHigh < 0.25f, "AA filter strongly attenuates 20kHz (above the 16kHz kit Nyquist)" );
    }

    // --- Oversampling reduces alias energy from the wavefolder. ---
    {
        const int N = 20000;
        static float k1[20000], k4[20000];
        const float inHz = 5000.0f;   // harmonics at 10k,15k,20k,25k... -> many above 16kHz
        const float fold = 6.0f;      // enough drive to generate strong high harmonics

        renderFolded( k1, N, inHz, fold, 1 );   // process at 32kHz -> aliasing
        renderFolded( k4, N, inHz, fold, 4 );   // 128kHz + AA -> cleaned

        float alias1 = aliasEnergy( k1 + 2000, N - 4000, inHz );
        float alias4 = aliasEnergy( k4 + 2000, N - 4000, inHz );
        printf( "Wavefolder alias energy: 1x=%.3f, 4x+AA=%.3f (ratio %.2fx less)\n",
                alias1, alias4, alias1 / ( alias4 > 1e-9f ? alias4 : 1e-9f ) );
        check( alias4 < alias1 * 0.5f, "4x oversampling cuts wavefolder alias energy by at least half" );
        check( alias4 < alias1, "4x oversampling has strictly less alias energy than 1x" );
    }

    printf( "\n%s (%d failures)\n", g_failures == 0 ? "OVERSAMPLING: TESTS PASS" : "OVERSAMPLING: FAILURES", g_failures );
    return g_failures == 0 ? 0 : 1;
}
