// p3_wavefolder.h — P3 wavefolder processor.
// Implementation Plan Step 9. Standalone processor unit, not yet wired into
// the full plugin. Uses the exact fold formula derived and verified in
// Step 8 (see verify_fold.cpp and 4x4-System-Overview.md §4).
//
// Unlike P1/P2/P4, this processor is a pure memoryless nonlinearity — no
// persistent state, so no class/struct instance is needed at all. Closed
// form, a handful of multiply-adds per sample, as specified.
//
// Parameter spec, settled here per the plan's Step 9 prompt (§4/§10 left
// this open): **fold and bias are both curve-only Contour destinations,
// no base value** — the same treatment as P4 (§5), not P1/P2's base+depth.
// Rationale: fold/bias are "amount of effect" controls (how much drive,
// how much asymmetry), the same category as P4's bit-depth/rate-divisor,
// not "where in pitch/frequency space" controls like P1/P2's base. A
// randomized per-render base wouldn't add anything meaningful here — at
// fold=0 the output degenerates to a constant (foldShape(bias) alone, no
// signal dependence at all), so "zero" is already the natural, musically
// neutral starting point for a curve to animate away from, same as P4's
// curve-only controls starting from no effect.
//
// Curve-to-destination mapping (documented here since this is the first
// processor where it needed spelling out numerically): each Contour
// instance's driving curve is bipolar +-5V (established convention, see
// §2's P1 driving curve). Both fold and bias map linearly from that +-5V
// curve range onto their native float range:
//   fold:  curve -5V -> 0.0,  curve +5V -> 20.0   (unitless gain/multiplier)
//   bias:  curve -5V -> -4.0, curve +5V -> +4.0   (unitless, matches the
//                                                   fold formula's period-4
//                                                   domain exactly)
//
// No dynamic allocation, no STL, no exceptions/RTTI, C++11.
#pragma once
#include <cmath>

namespace p3wavefolder {

static const float kFoldMin = 0.0f;
static const float kFoldMax = 20.0f;
static const float kBiasMin = -4.0f;
static const float kBiasMax = 4.0f;

// wrap4(a) = a - 4*floor(a/4), the always-nonnegative mod into [0,4).
inline float wrap4( float a )
{
    return a - 4.0f * floorf( a / 4.0f );
}

// Period-4 geometric mirror fold: identity on [-1,1], reflects at every
// odd-integer boundary (+-1, +-3, +-5, ...), bounded to [-1,1] for any
// input. Derived and verified in Step 8 (verify_fold.cpp).
inline float foldShape( float u )
{
    return 1.0f - fabsf( wrap4( u + 1.0f ) - 2.0f );
}

// fold: pre-scale (gain into the folding stage). bias: pre-offset (DC
// shift before folding, asymmetrizes the fold). See file header for the
// curve-to-range mapping that produces these two values upstream.
inline float process( float x, float fold, float bias )
{
    return foldShape( x * fold + bias );
}

// Convenience: map a +-5V bipolar Contour curve sample onto fold's native
// range. Linear, per the file header's documented mapping.
inline float foldFromCurve( float curveVolts )
{
    float t = ( curveVolts + 5.0f ) / 10.0f;   // [0,1]
    if ( t < 0.0f ) t = 0.0f;
    if ( t > 1.0f ) t = 1.0f;
    return kFoldMin + t * ( kFoldMax - kFoldMin );
}

// Convenience: map a +-5V bipolar Contour curve sample onto bias's native
// range. Linear, per the file header's documented mapping.
inline float biasFromCurve( float curveVolts )
{
    float t = ( curveVolts + 5.0f ) / 10.0f;   // [0,1]
    if ( t < 0.0f ) t = 0.0f;
    if ( t > 1.0f ) t = 1.0f;
    return kBiasMin + t * ( kBiasMax - kBiasMin );
}

} // namespace p3wavefolder
