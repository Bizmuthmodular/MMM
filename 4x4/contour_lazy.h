// contour_lazy.h — lazy, on-demand reimplementation of contour::Engine.
// Implementation Plan Step 3. Deliberate deviation from CONTOUR_NT_SPEC.md's
// "use the reference engine as given," per 4x4-System-Overview.md §6/§8:
// trades the reference engine's eager whole-curve breakpoint precompute for
// an event-driven generator that only ever holds a small, bounded window of
// breakpoints, independent of how many windows/envelopes the render spans.
//
// Must produce equivalent output to contour_engine.h — validated against
// CONTOUR_NT_SPEC.md §8.1's golden test cases in test_contour_lazy.cpp.
//
// No dynamic allocation, no STL, no exceptions/RTTI, C++11, <cmath>/<cstdint>
// only — same constraints as the reference engine.
#pragma once
#include <cmath>
#include <cstdint>

namespace contourlazy {

struct Params {
    float    lengthSec = 6.0f;
    int      windows   = 6;
    float    chaos     = 0.6f;
    float    radiusSec = 0.12f;
    float    roundProb = 1.0f;
    float    holdProb  = 0.0f;
    float    levelV    = 8.0f;
    float    offsetV   = 0.0f;
    bool     bipolar   = false;
    bool     loop      = false;
    uint32_t seed      = 7u;
};

class Engine {
public:
    Params params;

    explicit Engine( const Params& p = Params() ) : params( p ) { rebuild(); }

    void   setSampleRate( float sr ) { dt_ = 1.0 / sr; }
    void   reseed( uint32_t s )      { params.seed = s; rebuild(); }
    bool   running() const           { return running_; }
    float  durationSec() const       { return (float)T_; }
    double positionSec() const       { return t_; }
    int    breakpointCountResident() const { return nOut_; }   // window size in use — for measurement

    // Start or restart. Level carries on from wherever it is (no-reset
    // behaviour, same as the reference engine).
    void trigger()
    {
        double startLevel = primed_ ? currentLevelEstimate() : lastEnvelopeL2();
        resetGeneration( startLevel );
        primed_ = true;
        t_ = 0.0;
        running_ = true;
    }

    // Re-plan after a params change. Keeps playback position (passStart_).
    void rebuild()
    {
        T_ = clampd( params.lengthSec, 1.0, 10.0 );
        W_ = imax( 1, imin( params.windows, kMaxWindowsParam ) );
        nEnv_ = W_ * kEnvsPerWindow;
        nRound_ = probWindows( params.roundProb, 500000, round_ );
        nHold_  = probWindows( params.holdProb,  600000, hold_ );
        double startLevel = passStart_ >= 0.0 ? passStart_ : lastEnvelopeL2();
        resetGeneration( startLevel );
    }

    // One output sample in volts. Holds last value when stopped.
    float process()
    {
        if ( !running_ ) return out_;
        ensureGeneratedPast( t_ );
        out_ = (float)evalResident( t_ );
        t_ += dt_;
        if ( t_ >= T_ ) { if ( params.loop ) trigger(); else running_ = false; }
        return out_;
    }

    // Fills n evenly-spaced samples of the current curve, for display. Does
    // not touch playback state — runs its own throwaway generation pass.
    void renderCurve( float* out, int n ) const
    {
        Engine tmp( params );
        tmp.setSampleRate( (float)( 1.0 / dt_ ) );
        tmp.passStart_ = passStart_ >= 0.0 ? passStart_ : tmp.lastEnvelopeL2();
        tmp.resetGeneration( tmp.passStart_ );
        for ( int i = 0; i < n; ++i )
        {
            double t = T_ * i / ( n - 1 );
            tmp.ensureGeneratedPast( t );
            out[i] = (float)tmp.evalResident( t );
        }
    }

private:
    enum { kMaxWindowsParam = 32, kEnvsPerWindow = 2, kPieces = 24 };
    enum { kWindowCap = 12 };   // resident breakpoint window — bounded, independent of W

    struct Env  { double a, b; int r1, l1, r2, l2; };
    struct Span { double a, b; };
    struct Pt   { double t, vl, vr, r, s1, s2; };

    static double clampd( double v, double lo, double hi ) { return v < lo ? lo : ( v > hi ? hi : v ); }
    static double dmin( double a, double b ) { return a < b ? a : b; }
    static double dmax( double a, double b ) { return a > b ? a : b; }
    static int    imin( int a, int b ) { return a < b ? a : b; }
    static int    imax( int a, int b ) { return a > b ? a : b; }
    static double sweepSec( int rate ) { return 20.0 * pow( 2.0, -rate / 7.5 ); }

    // Bit-identical to contour_engine.h's rnd() — same keyed mulberry32.
    double rnd( uint32_t i, uint32_t j ) const
    {
        uint32_t a = (uint32_t)( (uint64_t)params.seed * 100003ull + (uint64_t)i * 7919ull + (uint64_t)j * 104729ull );
        a += 0x6D2B79F5u;
        uint32_t t = ( a ^ ( a >> 15 ) ) * ( 1u | a );
        t = ( t + ( ( t ^ ( t >> 7 ) ) * ( 61u | t ) ) ) ^ t;
        return (double)( t ^ ( t >> 14 ) ) / 4294967296.0;
    }

    // Same probWindows() region algorithm as the reference engine — this is
    // the one part that genuinely needs whole-timeline knowledge, so it's
    // still computed eagerly here. kPieces=24 (native default): cheap at
    // 32 bytes/piece regardless.
    int probWindows( float prob, uint32_t base, Span* out ) const
    {
        if ( prob >= 1.0f ) { out[0] = { 0.0, T_ }; return 1; }
        if ( prob <= 0.0f ) return 0;
        double w[kPieces], a[kPieces], len[kPieces], sum = 0.0; int order[kPieces];
        for ( int k = 0; k < kPieces; ++k ) { w[k] = 0.25 + rnd( base + k, 1 ); sum += w[k]; order[k] = k; }
        double acc = 0.0;
        for ( int k = 0; k < kPieces; ++k ) { a[k] = acc; len[k] = w[k] / sum * T_; acc += len[k]; }
        double key[kPieces];
        for ( int k = 0; k < kPieces; ++k ) key[k] = rnd( base + k, 2 );
        for ( int i = 1; i < kPieces; ++i ) {
            const int o = order[i]; int j = i - 1;
            while ( j >= 0 && key[order[j]] > key[o] ) { order[j + 1] = order[j]; --j; }
            order[j + 1] = o;
        }
        Span picked[kPieces]; int np = 0; double need = prob * T_;
        for ( int oi = 0; oi < kPieces && need > 1e-9; ++oi ) {
            const int k = order[oi];
            if ( len[k] <= need ) { picked[np++] = { a[k], a[k] + len[k] }; need -= len[k]; }
            else { const double off = rnd( base + k, 3 ) * ( len[k] - need ); picked[np++] = { a[k] + off, a[k] + off + need }; need = 0.0; }
        }
        for ( int i = 1; i < np; ++i ) {
            const Span sp = picked[i]; int j = i - 1;
            while ( j >= 0 && picked[j].a > sp.a ) { picked[j + 1] = picked[j]; --j; }
            picked[j + 1] = sp;
        }
        int n = 0;
        for ( int i = 0; i < np; ++i ) {
            if ( n && picked[i].a <= out[n - 1].b + 1e-9 ) out[n - 1].b = dmax( out[n - 1].b, picked[i].b );
            else out[n++] = picked[i];
        }
        return n;
    }

    // Envelope parameters for ANY index, computed directly (O(1), no
    // dependency on other envelopes) — the key property that makes lazy
    // generation possible: window/envelope boundaries are deterministic
    // linear divisions of T_, and rnd() is a keyed lookup, not a sequence.
    void envelopeAt( int idx, Env& e, double& outA, double& outB ) const
    {
        int wi  = idx / kEnvsPerWindow;
        int sub = idx % kEnvsPerWindow;
        double a = wi * T_ / W_;
        double b = ( wi == W_ - 1 ) ? T_ : ( wi + 1 ) * T_ / W_;
        double base = ( b - a ) / kEnvsPerWindow;
        double ea = a + sub * base;
        double eb = ( sub == kEnvsPerWindow - 1 ) ? b : a + base;
        auto pull = [&]( double v, uint32_t j ) { return (int)floor( v + params.chaos * ( 99.0 * rnd( 3000 + idx, j ) - v ) + 0.5 ); };
        e.r1 = pull( 0.0, 1 );
        e.l1 = pull( 99.0 * rnd( 2000 + idx, 2 ), 2 );
        e.r2 = pull( 0.0, 3 );
        e.l2 = pull( 99.0 * rnd( 2000 + idx, 4 ), 4 );
        outA = ea; outB = eb;
    }

    double lastEnvelopeL2() const
    {
        Env e; double a, b;
        envelopeAt( nEnv_ - 1, e, a, b );
        return (double)e.l2;
    }

    // --- Generation cursor: the envelope/stage currently being traversed ---
    int    curEnvIdx_, curStage_;      // which envelope + stage (0=toward L1, 1=toward L2)
    double curSegT0_, curSegV0_;       // segment start (already finalized)
    double curSegT1_, curSegV1_;       // segment end (target or cutoff)
    bool   curExhausted_;              // true once past the last envelope

    // curStage_: 0 = moving toward L1, 1 = moving toward L2, 2 = flat tail
    // (stage 1 reached its target before the envelope's own time slot ran
    // out — the reference engine implicitly holds flat here too, since its
    // per-envelope loop only emits breakpoints at stage transitions, not at
    // arbitrary elapsed time; the next real breakpoint is the next
    // envelope's own start, which only arrives once this envelope's full
    // [a,b) slot has elapsed).
    void beginSegmentFromCursor()
    {
        for ( ;; )
        {
            if ( curEnvIdx_ >= nEnv_ ) { curExhausted_ = true; curSegT1_ = T_; curSegV1_ = curSegV0_; return; }
            Env e; double ea, eb;
            envelopeAt( curEnvIdx_, e, ea, eb );
            if ( curSegT0_ >= eb - 1e-12 ) { ++curEnvIdx_; curStage_ = 0; continue; }   // envelope already fully behind us
            int tgt  = curStage_ ? e.l2 : e.l1;
            int rate = curStage_ ? e.r2 : e.r1;
            double sp = 99.0 / sweepSec( rate );
            double need = fabs( (double)tgt - curSegV0_ ) / sp;
            if ( curSegT0_ + need <= eb )
            {
                curSegT1_ = curSegT0_ + need;
                curSegV1_ = tgt;
            }
            else
            {
                curSegV1_ = curSegV0_ + ( tgt > curSegV0_ ? 1.0 : -1.0 ) * sp * ( eb - curSegT0_ );
                curSegT1_ = eb;
            }
            curExhausted_ = false;
            return;
        }
    }

    // Advance exactly one stage-step past the current segment end.
    void stepToNextSegment()
    {
        curSegT0_ = curSegT1_;
        curSegV0_ = curSegV1_;

        if ( curStage_ == 2 )   // flat tail just finished -> genuinely next envelope
        {
            ++curEnvIdx_; curStage_ = 0; beginSegmentFromCursor(); return;
        }

        Env e; double ea, eb; envelopeAt( curEnvIdx_, e, ea, eb );

        if ( curStage_ == 0 )
        {
            if ( curSegT0_ < eb - 1e-12 ) { curStage_ = 1; beginSegmentFromCursor(); return; }
            ++curEnvIdx_; curStage_ = 0; beginSegmentFromCursor(); return;   // cutoff exactly at eb, stage1 never runs
        }

        // curStage_ == 1 just finished
        if ( curSegT0_ < eb - 1e-12 )
        {
            // reached L2 before this envelope's time slot elapsed -> flat
            // tail holds the level until eb, matching the reference engine.
            curSegT1_ = eb; curSegV1_ = curSegV0_; curStage_ = 2; curExhausted_ = false;
            return;
        }
        ++curEnvIdx_; curStage_ = 0; beginSegmentFromCursor();
    }

    // Advance the cursor (silently, no emission) until targetT falls within
    // the active segment (or we're exhausted), returning the level there.
    // This is what makes fast-forwarding through a hold region cheap: no
    // breakpoints are generated for the skipped interior, only the cursor
    // state advances.
    double levelAtArbitraryTime( double targetT )
    {
        while ( !curExhausted_ && targetT > curSegT1_ + 1e-12 )
            stepToNextSegment();
        if ( curExhausted_ ) return curSegV0_;
        double frac = ( curSegT1_ > curSegT0_ ) ? ( targetT - curSegT0_ ) / ( curSegT1_ - curSegT0_ ) : 0.0;
        return curSegV0_ + ( curSegV1_ - curSegV0_ ) * frac;
    }

    double rawToV( double level ) const
    {
        double y = level / 99.0;
        return ( params.bipolar ? params.levelV * ( 2.0 * y - 1.0 ) : params.levelV * y ) + params.offsetV;
    }

    double voltsAtArbitraryTime( double targetT )
    {
        return clampd( rawToV( levelAtArbitraryTime( targetT ) ), -10.0, 10.0 );
    }

    bool inRoundSpan( double t ) const
    {
        for ( int w = 0; w < nRound_; ++w )
            if ( t >= round_[w].a && t <= round_[w].b ) return true;
        return false;
    }

    // --- Resident output window: the only breakpoints kept in memory ---
    Pt  out_win_[kWindowCap];
    int nOut_;        // count currently resident
    int evalK_;        // index into out_win_[] of the segment being evaluated (k, k+1)
    int nextHoldIdx_;  // next hold span not yet processed

    void pushOut( double t, double vl, double vr )
    {
        if ( nOut_ >= kWindowCap )
        {
            // Compact: discard everything at/behind evalK_-1 (no longer
            // needed — eval only ever touches evalK_/evalK_+1, and radius
            // finalization only ever looks one point back).
            int keepFrom = imax( 0, evalK_ - 1 );
            int moved = nOut_ - keepFrom;
            for ( int i = 0; i < moved; ++i ) out_win_[i] = out_win_[keepFrom + i];
            nOut_ = moved;
            evalK_ -= keepFrom;
        }
        Pt& p = out_win_[nOut_];
        p.t = t; p.vl = vl; p.vr = vr; p.r = 0.0; p.s1 = 0.0; p.s2 = 0.0;
        if ( nOut_ > 0 )
        {
            Pt& prev = out_win_[nOut_ - 1];
            double d1 = p.t - prev.t;
            p.s1 = d1 > 1e-12 ? ( p.vl - prev.vr ) / d1 : 0.0;
            // finalize prev's outgoing slope + radius now that p exists
            prev.s2 = d1 > 1e-12 ? ( p.vl - prev.vr ) / d1 : 0.0;
            if ( nOut_ >= 2 )
            {
                double d0 = prev.t - out_win_[nOut_ - 2].t;
                bool roundHere = inRoundSpan( prev.t );
                prev.r = ( roundHere && params.radiusSec > 0.0f )
                             ? dmin( (double)params.radiusSec, dmin( d0 * 0.5, d1 * 0.5 ) )
                             : 0.0;
            }
        }
        ++nOut_;
    }

    void resetGeneration( double startLevel )
    {
        curEnvIdx_ = 0; curStage_ = 0;
        curSegT0_ = 0.0; curSegV0_ = startLevel;
        curExhausted_ = false;
        beginSegmentFromCursor();
        nOut_ = 0; evalK_ = 0; nextHoldIdx_ = 0;
        pushOut( 0.0, clampd( rawToV( startLevel ), -10.0, 10.0 ), clampd( rawToV( startLevel ), -10.0, 10.0 ) );
    }

    double currentLevelEstimate() const
    {
        // Used only by trigger() when retriggering mid-pass: approximate
        // "level right now" from the most recently resident point. Close
        // enough for the no-reset carry-over — exact to within the last
        // generated point's value, same spirit as the reference engine's
        // levelAt(t_) at retrigger time.
        return nOut_ > 0 ? ( params.bipolar
                                 ? ( ( out_win_[nOut_ - 1].vr - params.offsetV ) / params.levelV + 1.0 ) * 49.5
                                 : ( out_win_[nOut_ - 1].vr - params.offsetV ) / params.levelV * 99.0 )
                          : lastEnvelopeL2();
    }

    // Generate forward (emitting into out_win_) until we have at least one
    // finalized breakpoint pair spanning time t, plus one more point ahead
    // for radius lookahead.
    void ensureGeneratedPast( double t )
    {
        double need = t + 1e-9;
        while ( nOut_ < 3 || out_win_[nOut_ - 2].t < need )
        {
            if ( curExhausted_ && curSegT0_ >= T_ - 1e-9 )
            {
                // Nothing left to generate — pad with a flat final point.
                Pt& last = out_win_[nOut_ - 1];
                if ( last.t < T_ - 1e-9 ) pushOut( T_, last.vr, last.vr );
                else break;
                continue;
            }

            // Hold check: does the next upcoming hold start at/before the
            // current segment's natural end?
            if ( nextHoldIdx_ < nHold_ && hold_[nextHoldIdx_].a <= curSegT1_ + 1e-9
                 && hold_[nextHoldIdx_].a >= out_win_[nOut_ - 1].t - 1e-9 )
            {
                double a = hold_[nextHoldIdx_].a, b = hold_[nextHoldIdx_].b;

                // Emit any rail crossing strictly before `a` within the
                // current segment first (matches reference: normal cT_/cV_
                // points before a hold are emitted as ordinary points).
                emitCrossingsUpTo( a );

                double va = voltsAtArbitraryTime( a );
                pushOut( a, va, va );
                double vb = voltsAtArbitraryTime( b );
                pushOut( b, va, b >= T_ - 1e-9 ? va : vb );
                ++nextHoldIdx_;
                continue;
            }

            // Normal step: emit any crossings up to the segment end, then
            // the segment end point itself, then advance. A zero-distance
            // segment (stage reaches a target that equals where it already
            // was — e.g. l1==l2 by chance) must NOT emit a duplicate point
            // at the same time: the reference engine's pushLv() silently
            // dedupes this case (overwrites in place rather than adding a
            // new breakpoint), so skip emission here too and just advance.
            bool zeroDistance = nOut_ > 0 && fabs( curSegT1_ - out_win_[nOut_ - 1].t ) < 1e-9;
            if ( !zeroDistance )
            {
                emitCrossingsUpTo( curSegT1_ );
                pushOut( curSegT1_, clampd( rawToV( curSegV1_ ), -10.0, 10.0 ), clampd( rawToV( curSegV1_ ), -10.0, 10.0 ) );
            }
            if ( curSegT1_ >= T_ - 1e-9 ) { curExhausted_ = true; curSegT0_ = T_; }
            else stepToNextSegment();
        }
    }

    // Emits rail-crossing points (if any) between the last emitted point and
    // targetT, within the CURRENT segment. toV() is affine in level, so
    // clamp(interp(raw volts)) reproduces the reference's crossing-insertion
    // construction exactly (clamping commutes with linear interpolation).
    void emitCrossingsUpTo( double targetT )
    {
        if ( nOut_ == 0 ) return;
        double t0 = out_win_[nOut_ - 1].t;
        if ( targetT <= t0 + 1e-12 ) return;
        double v0level = levelAtArbitraryTimeNoAdvance( t0 );
        double v1level = levelAtArbitraryTimeNoAdvance( targetT );
        double rv0 = rawToV( v0level ), rv1 = rawToV( v1level );
        const double rails[2] = { -10.0, 10.0 };
        double u[2], rv[2]; int nc = 0;
        for ( int q = 0; q < 2; ++q )
            if ( ( rv0 - rails[q] ) * ( rv1 - rails[q] ) < 0.0 )
            { u[nc] = ( rails[q] - rv0 ) / ( rv1 - rv0 ); rv[nc++] = rails[q]; }
        if ( nc == 2 && u[0] > u[1] ) { double x = u[0]; u[0] = u[1]; u[1] = x; x = rv[0]; rv[0] = rv[1]; rv[1] = x; }
        for ( int c = 0; c < nc; ++c )
        {
            double tc = t0 + u[c] * ( targetT - t0 );
            pushOut( tc, rv[c], rv[c] );
        }
    }

    // Same as levelAtArbitraryTime but assumes t is within the segment
    // ending at curSegT1_ that's already active (used by emitCrossingsUpTo,
    // which only ever queries within the current un-stepped segment).
    double levelAtArbitraryTimeNoAdvance( double t ) const
    {
        double frac = ( curSegT1_ > curSegT0_ ) ? ( t - curSegT0_ ) / ( curSegT1_ - curSegT0_ ) : 0.0;
        return curSegV0_ + ( curSegV1_ - curSegV0_ ) * frac;
    }

    double evalResident( double t )
    {
        while ( evalK_ < nOut_ - 2 && t >= out_win_[evalK_ + 1].t ) ++evalK_;
        const Pt& A = out_win_[evalK_];
        const Pt& B = out_win_[evalK_ + 1];
        double lin = A.vr + B.s1 * ( t - A.t );
        if ( B.r > 0.0 && t > B.t - B.r )
        {
            double u = ( t - ( B.t - B.r ) ) / ( 2.0 * B.r );
            return lin + ( B.s2 - B.s1 ) * B.r * u * u + ( B.vr - B.vl ) * u * u * ( 3.0 - 2.0 * u );
        }
        if ( A.r > 0.0 && t < A.t + A.r )
        {
            double u = ( t - ( A.t - A.r ) ) / ( 2.0 * A.r );
            return A.vl + A.s1 * ( t - A.t ) + ( A.s2 - A.s1 ) * A.r * u * u + ( A.vr - A.vl ) * u * u * ( 3.0 - 2.0 * u );
        }
        return lin;
    }

    Span   round_[kPieces], hold_[kPieces];
    int    nRound_ = 0, nHold_ = 0;
    int    W_ = 6, nEnv_ = 12;
    double T_ = 6.0, dt_ = 1.0 / 48000.0, t_ = 0.0, passStart_ = -1.0;
    bool   running_ = false, primed_ = false;
    float  out_ = 0.0f;
};

} // namespace contourlazy
