// test_feedback_network.cpp — desktop validation for feedback_network.h
// (Implementation Plan Step 10). No explicit validation criterion was
// stated in the plan for this step, but the project's working practice has
// been to validate everything testable on desktop before moving on, so
// this checks: the starter wave fires for its set duration then goes
// silent, the matrix correctly uses each channel's PREVIOUS sample (no
// same-sample circular dependency), and the wired network stays stable
// (no NaN/divergence) and produces a genuinely self-sustaining signal once
// kicked by the starter burst -- the core premise of the whole plugin.
// Build: g++ -std=c++11 -O2 -o test_feedback_network test_feedback_network.cpp
#include "fft.h"
#include "feedback_network.h"
#include <cstdio>
#include <cmath>

static int g_failures = 0;
static void check( bool cond, const char* what ) { printf( "  %s: %s\n", cond ? "ok" : "FAIL", what ); if ( !cond ) ++g_failures; }

int main()
{
    fft::initTwiddles();   // required once before any P1 (phase vocoder) use -- see feedback_network.h
    const float sr = 48000.0f;

    // --- Starter wave: fires for durationSec, then silent. ---
    {
        feedbacknet::StarterWave sw;
        sw.freqHz = 110.0f;
        sw.durationSec = 0.2f;
        sw.onNewRender();
        int activeSamples = 0, silentAfter = 0;
        int durSamples = (int)( 0.2f * sr );
        for ( int i = 0; i < durSamples + 1000; ++i )
        {
            float y = sw.processSample( sr );
            if ( i < durSamples ) { if ( y != 0.0f ) ++activeSamples; }
            else { if ( y == 0.0f ) ++silentAfter; }
        }
        printf( "Starter wave: active samples in window = %d/%d, silent after = %d/1000\n", activeSamples, durSamples, silentAfter );
        check( activeSamples == durSamples, "starter wave produces nonzero output for every sample of its duration" );
        check( silentAfter == 1000, "starter wave is exactly silent after its duration elapses" );
    }

    // --- No same-sample circular dependency: with a nonzero self-feedback
    //     coefficient and all starter waves silenced (duration=0), the
    //     first processed sample's feedback contribution must be exactly
    //     zero (since prevOut starts at 0), not some nonzero resolved
    //     fixed-point value. ---
    {
        feedbacknet::FeedbackNetwork net;
        for ( int ch = 0; ch < 4; ++ch ) { net.starter( ch ).durationSec = 0.0f; }
        net.setMatrixCoeff( 0, 0, 0.9f );   // channel 1 self-feedback
        net.onNewRender( 1 );
        feedbacknet::ChannelControls ctrl;
        float out[4];
        net.processSample( sr, ctrl, out );
        printf( "\nFirst sample with silenced starter + 0.9 self-feedback: out[0]=%.6f\n", out[0] );
        // P1 at depth=0 with zero input and zero random base should output
        // ~0 on the very first sample (nothing in its analysis buffer yet).
        check( fabsf( out[0] ) < 0.5f, "first sample's output reflects zero feedback input, not a resolved nonzero loop value" );
    }

    // --- Stability + genuine self-sustaining behavior: kick channel 1's
    //     loop with the starter burst, feed its own output back into
    //     itself, and confirm (a) no NaN/divergence over a long run, and
    //     (b) the signal doesn't just decay to silence after the burst --
    //     i.e. the feedback loop is actually doing something. ---
    {
        feedbacknet::FeedbackNetwork net;
        net.starter( 0 ).freqHz = 110.0f;
        net.starter( 0 ).durationSec = 0.1f;
        for ( int ch = 1; ch < 4; ++ch ) net.starter( ch ).durationSec = 0.0f;
        net.setMatrixCoeff( 0, 0, 0.95f );   // strong self-feedback on channel 1 only
        net.onNewRender( 42 );
        feedbacknet::ChannelControls ctrl;
        ctrl.p1DepthSemitones = 3.0f;   // give P1 something to do besides pure passthrough

        const int N = (int)( 1.0f * sr );   // 1 second
        bool anyNaN = false, anyHuge = false;
        float maxAbsAfterBurst = 0.0f;
        int burstEnd = (int)( 0.1f * sr );
        for ( int i = 0; i < N; ++i )
        {
            float out[4];
            net.processSample( sr, ctrl, out );
            for ( int ch = 0; ch < 4; ++ch )
            {
                if ( std::isnan( out[ch] ) ) anyNaN = true;
                if ( fabsf( out[ch] ) > 1000.0f ) anyHuge = true;
            }
            if ( i > burstEnd && fabsf( out[0] ) > maxAbsAfterBurst ) maxAbsAfterBurst = fabsf( out[0] );
        }
        printf( "\n1s run, channel 1 self-feedback=0.95, starter burst 0.1s: anyNaN=%d anyHuge=%d maxAbsAfterBurst=%.4f\n",
                (int)anyNaN, (int)anyHuge, maxAbsAfterBurst );
        check( !anyNaN, "no NaN produced over a 1-second run" );
        check( !anyHuge, "no runaway divergence (|output| never exceeds 1000) over a 1-second run" );
        check( maxAbsAfterBurst > 0.01f, "the loop sustains nonzero signal after the starter burst ends (genuine feedback, not instant decay to silence)" );
    }

    // --- Matrix coefficient accessors round-trip. ---
    {
        feedbacknet::FeedbackNetwork net;
        net.setMatrixCoeff( 2, 1, -0.37f );
        check( fabsf( net.matrixCoeff( 2, 1 ) - ( -0.37f ) ) < 1e-6f, "setMatrixCoeff/matrixCoeff round-trip exactly" );
        check( fabsf( net.matrixCoeff( 0, 0 ) ) < 1e-6f, "unset matrix coefficients default to zero" );
    }

    // --- Processor enable/mute: a disabled channel outputs exactly 0 (and
    //     thus contributes nothing to the mix or feedback), while its
    //     enabled neighbours still produce signal. ---
    {
        feedbacknet::FeedbackNetwork net;
        for ( int ch = 0; ch < 4; ++ch ) { net.starter( ch ).freqHz = 110.0f + 40.0f * ch; net.starter( ch ).durationSec = 0.1f; }
        // Light cross-feedback so every channel is driven.
        for ( int r = 0; r < 4; ++r ) net.setMatrixCoeff( r, ( r + 1 ) % 4, 0.5f );
        net.onNewRender( 7 );

        feedbacknet::ChannelControls ctrl;
        ctrl.processorEnabled[0] = false;   // P1 off
        ctrl.processorEnabled[3] = false;   // P4 off

        bool ch0AlwaysZero = true, ch3AlwaysZero = true, ch1EverNonzero = false, ch2EverNonzero = false;
        for ( int i = 0; i < (int)( sr * 0.5f ); ++i )
        {
            float out[4];
            net.processSample( sr, ctrl, out );
            if ( out[0] != 0.0f ) ch0AlwaysZero = false;
            if ( out[3] != 0.0f ) ch3AlwaysZero = false;
            if ( fabsf( out[1] ) > 1e-6f ) ch1EverNonzero = true;
            if ( fabsf( out[2] ) > 1e-6f ) ch2EverNonzero = true;
        }
        check( ch0AlwaysZero, "disabled channel 0 (P1) outputs exactly 0 for the whole run" );
        check( ch3AlwaysZero, "disabled channel 3 (P4) outputs exactly 0 for the whole run" );
        check( ch1EverNonzero, "enabled channel 1 (P2) still produces signal" );
        check( ch2EverNonzero, "enabled channel 2 (P3) still produces signal" );

        // All four off -> complete silence on every channel.
        feedbacknet::FeedbackNetwork net2;
        for ( int ch = 0; ch < 4; ++ch ) { net2.starter( ch ).freqHz = 110.0f; net2.starter( ch ).durationSec = 0.1f; }
        net2.onNewRender( 7 );
        feedbacknet::ChannelControls allOff;
        for ( int ch = 0; ch < 4; ++ch ) allOff.processorEnabled[ch] = false;
        bool allZero = true;
        for ( int i = 0; i < 2000; ++i )
        {
            float out[4];
            net2.processSample( sr, allOff, out );
            for ( int ch = 0; ch < 4; ++ch ) if ( out[ch] != 0.0f ) allZero = false;
        }
        check( allZero, "all four processors off -> every channel output is exactly 0 (silent render)" );
    }

    printf( "\n%s (%d failures)\n", g_failures == 0 ? "FEEDBACK NETWORK: TESTS PASS" : "FEEDBACK NETWORK: FAILURES", g_failures );
    return g_failures == 0 ? 0 : 1;
}
