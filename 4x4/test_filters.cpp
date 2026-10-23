// test_filters.cpp — desktop validation for filters.h (Implementation
// Plan Step 11). Confirms HPF/LPF cutoff placement via a frequency sweep,
// and that the Reg chain's AGC settles output level into the locked
// target band over a few hundred ms, per the plan's stated criterion.
// Build: g++ -std=c++11 -O2 -o test_filters test_filters.cpp
#include "filters.h"
#include <cstdio>
#include <cmath>

static int g_failures = 0;
static void check( bool cond, const char* what ) { printf( "  %s: %s\n", cond ? "ok" : "FAIL", what ); if ( !cond ) ++g_failures; }

// Measures the steady-state output/input amplitude ratio for a sine tone
// at freqHz through a fresh filter instance.
template <typename FilterFn>
static float measureGainAt( FilterFn filterCall, float freqHz, float sampleRate )
{
    const float kTwoPi = 6.283185307f;
    const int N = (int)( sampleRate * 0.2f );   // 200ms, plenty of settle time
    float maxOut = 0.0f;
    for ( int i = 0; i < N; ++i )
    {
        float x = sinf( kTwoPi * freqHz * i / sampleRate );
        float y = filterCall( x );
        if ( i > N / 2 ) { float a = fabsf( y ); if ( a > maxOut ) maxOut = a; }   // steady-state half
    }
    return maxOut;   // input amplitude is 1.0, so this IS the gain
}

int main()
{
    const float sr = 48000.0f;
    const float cutoff = 1000.0f;

    // --- LPF: should pass well below cutoff near-unity, attenuate well above it. ---
    {
        filters::OnePoleLowpass lp;
        float gLow = measureGainAt( [&]( float x ) { return lp.process( x, cutoff, sr ); }, 100.0f, sr );
        filters::OnePoleLowpass lp2;
        float gAtCutoff = measureGainAt( [&]( float x ) { return lp2.process( x, cutoff, sr ); }, cutoff, sr );
        filters::OnePoleLowpass lp3;
        float gHigh = measureGainAt( [&]( float x ) { return lp3.process( x, cutoff, sr ); }, 10000.0f, sr );
        printf( "LPF (cutoff=1000Hz): gain@100Hz=%.4f, gain@1000Hz=%.4f (want ~0.707), gain@10000Hz=%.4f\n", gLow, gAtCutoff, gHigh );
        check( gLow > 0.95f, "LPF passes 100Hz (well below cutoff) near-unity" );
        check( fabsf( gAtCutoff - 0.7071f ) < 0.03f, "LPF attenuates to ~-3dB (0.707) at the cutoff frequency" );
        check( gHigh < 0.2f, "LPF attenuates 10kHz (well above cutoff) substantially" );
    }

    // --- HPF: should attenuate well below cutoff, pass well above it near-unity. ---
    {
        filters::OnePoleHighpass hp;
        float gLow = measureGainAt( [&]( float x ) { return hp.process( x, cutoff, sr ); }, 100.0f, sr );
        filters::OnePoleHighpass hp2;
        float gAtCutoff = measureGainAt( [&]( float x ) { return hp2.process( x, cutoff, sr ); }, cutoff, sr );
        filters::OnePoleHighpass hp3;
        float gHigh = measureGainAt( [&]( float x ) { return hp3.process( x, cutoff, sr ); }, 10000.0f, sr );
        printf( "\nHPF (cutoff=1000Hz): gain@100Hz=%.4f, gain@1000Hz=%.4f (want ~0.707), gain@10000Hz=%.4f\n", gLow, gAtCutoff, gHigh );
        check( gLow < 0.2f, "HPF attenuates 100Hz (well below cutoff) substantially" );
        check( fabsf( gAtCutoff - 0.7071f ) < 0.03f, "HPF attenuates to ~-3dB (0.707) at the cutoff frequency" );
        check( gHigh > 0.95f, "HPF passes 10kHz (well above cutoff) near-unity" );
    }

    // --- DC blocker: should remove a constant offset, pass audio-rate content. ---
    {
        filters::DcBlocker dc;
        float y = 0.0f;
        for ( int i = 0; i < (int)( sr * 0.5f ); ++i ) y = dc.process( 3.0f );   // constant 3V DC
        printf( "\nDC blocker: output after 0.5s of constant 3V input = %.6f (should be ~0)\n", y );
        check( fabsf( y ) < 0.01f, "DC blocker removes a sustained DC offset" );
    }

    // --- Soft clip: bounded to +-11V for any input, near-identity for small input. ---
    {
        bool bounded = true;
        for ( float x = -1000.0f; x <= 1000.0f; x += 7.3f )
            if ( fabsf( filters::softClip( x ) ) > 11.0f + 1e-4f ) bounded = false;
        check( bounded, "softClip never exceeds +-11V for any input" );
        check( fabsf( filters::softClip( 0.5f ) - 0.5f ) < 0.01f, "softClip is near-identity for small input (0.5V)" );
    }

    // --- Reg chain (DC block -> AGC -> soft clip): feed a test signal with
    //     a DC offset and an amplitude far outside the target band, and
    //     confirm the AGC settles output peak into the locked -18..-6dB
    //     window (~1.39V-5.51V) within a few hundred ms. ---
    {
        filters::Reg reg;
        const float kTwoPi = 6.283185307f;
        const int N = (int)( sr * 1.0f );   // 1 second
        float maxInWindow = 0.0f;
        for ( int i = 0; i < N; ++i )
        {
            float x = 2.0f + 20.0f * sinf( kTwoPi * 220.0f * i / sr );   // DC offset + way-oversized amplitude
            float y = reg.process( x, sr );
            if ( i > (int)( sr * 0.5f ) )   // measure over the second half, well past settle time
            {
                float a = fabsf( y );
                if ( a > maxInWindow ) maxInWindow = a;
            }
        }
        printf( "\nReg chain: 20V-amplitude + 2V DC input, measured peak output over [0.5s,1.0s] = %.4f\n", maxInWindow );
        check( maxInWindow >= 1.39f - 0.3f && maxInWindow <= 5.51f + 0.3f,
               "Reg settles output peak into (near) the locked -18..-6dB target band (~1.39-5.51V)" );
    }

    // --- AntiAliasLowpass (used before the 32kHz kit decimation, §7):
    //     a tone above the 16kHz kit Nyquist is strongly attenuated; a tone
    //     well below passes near-unity. Tested at a 192kHz internal rate (4x). ---
    {
        const float internal = 192000.0f;
        const float kTwoPi = 6.283185307f;
        auto aaGainAt = [&]( float fHz ) {
            filters::AntiAliasLowpass aa; aa.reset();
            int N = (int)( internal * 0.05f );
            float mx = 0.0f;
            for ( int i = 0; i < N; ++i ) { float y = aa.process( sinf( kTwoPi * fHz * i / internal ), internal ); if ( i > N / 2 && fabsf( y ) > mx ) mx = fabsf( y ); }
            return mx;
        };
        float gLow = aaGainAt( 3000.0f );
        float gHigh = aaGainAt( 20000.0f );
        printf( "\nAntiAliasLowpass @192kHz: gain@3kHz=%.3f, gain@20kHz=%.4f\n", gLow, gHigh );
        check( gLow > 0.9f, "AA lowpass passes 3kHz (in-band) near-unity" );
        check( gHigh < 0.25f, "AA lowpass strongly attenuates 20kHz (above 16kHz kit Nyquist)" );
    }

    // --- Resonant bandpass (per-channel pre-processor filter, 2026-10-20):
    //     0dB at the center freq, attenuated away from it; narrower bandwidth
    //     attenuates out-of-band content more. ---
    {
        const float kTwoPi = 6.283185307f;
        const float f0 = 1000.0f;
        auto bpGainAt = [&]( float bwSemis, float fHz ) {
            filters::Bandpass bp; bp.reset(); bp.setParams( f0, bwSemis, sr );
            int N = (int)( sr * 0.2f ); float mx = 0.0f;
            for ( int i = 0; i < N; ++i ) { float y = bp.process( sinf( kTwoPi * fHz * i / sr ) ); if ( i > N / 2 && fabsf( y ) > mx ) mx = fabsf( y ); }
            return mx;
        };
        float gCenter = bpGainAt( 24.0f, f0 );            // at center, 2-octave BW
        float gWideOut = bpGainAt( 24.0f, 4000.0f );      // 2 octaves above, wide BW
        float gNarrowOut = bpGainAt( 4.0f, 4000.0f );     // 2 octaves above, narrow BW
        printf( "\nBandpass @1kHz: gain@center=%.3f, gain@4kHz(BW=24st)=%.3f, gain@4kHz(BW=4st)=%.4f\n",
                gCenter, gWideOut, gNarrowOut );
        check( fabsf( gCenter - 1.0f ) < 0.05f, "bandpass has ~0dB (unity) gain at the center frequency" );
        check( gWideOut < gCenter, "bandpass attenuates content 2 octaves above center" );
        check( gNarrowOut < gWideOut, "narrower bandwidth attenuates out-of-band content more than wider" );
        check( gNarrowOut < 0.2f, "a narrow (4-semitone) bandpass strongly rejects content 2 octaves out" );

        // Finite/stable output for a hot input at the resonant center, narrow BW.
        filters::Bandpass bp; bp.reset(); bp.setParams( f0, 2.0f, sr );
        bool finite = true;
        for ( int i = 0; i < (int)( sr * 0.5f ); ++i )
        {
            float y = bp.process( 5.0f * sinf( kTwoPi * f0 * i / sr ) );
            if ( std::isnan( y ) || fabsf( y ) > 1e4f ) finite = false;
        }
        check( finite, "narrow bandpass stays finite driven hard at its center freq" );
    }

    printf( "\n%s (%d failures)\n", g_failures == 0 ? "FILTERS/REG: TESTS PASS" : "FILTERS/REG: FAILURES", g_failures );
    return g_failures == 0 ? 0 : 1;
}
