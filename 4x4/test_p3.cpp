// test_p3.cpp — desktop validation for p3_wavefolder.h (Implementation
// Plan Step 9). The plan's prompt asks to check the transfer curve against
// "§4's Fig. 4" — no such figure actually exists in
// 4x4-System-Overview.md (checked directly; only a Fig.1/STARTER marker is
// present), so this instead validates against the formula Step 8 already
// derived and numerically verified (verify_fold.cpp), confirming the real
// Processor wrapper reduces to exactly that formula and behaves correctly
// as fold/bias vary.
// Build: g++ -std=c++11 -O2 -o test_p3 test_p3.cpp
#include "p3_wavefolder.h"
#include <cstdio>
#include <cmath>

static int g_failures = 0;
static void check( bool cond, const char* what ) { printf( "  %s: %s\n", cond ? "ok" : "FAIL", what ); if ( !cond ) ++g_failures; }

int main()
{
    using namespace p3wavefolder;

    // --- fold=1, bias=0 should reduce exactly to the Step 8 formula
    //     (identity on [-1,1], reflecting at +-1, +-3, ...). ---
    {
        printf( "fold=1.0, bias=0.0 transfer curve (should match Step 8's verified formula exactly):\n" );
        bool matches = true;
        for ( float x = -5.0f; x <= 5.0001f; x += 0.5f )
        {
            float y = process( x, 1.0f, 0.0f );
            float expected = foldShape( x );
            printf( "  x=%+5.2f -> y=%+.4f\n", x, y );
            if ( fabsf( y - expected ) > 1e-6f ) matches = false;
        }
        check( matches, "process(x, 1.0, 0.0) == foldShape(x) exactly" );
    }

    // --- Identity region: small-signal fold should pass through mostly
    //     unchanged (fold small enough to stay within [-1,1]). ---
    {
        printf( "\nfold=0.1, bias=0.0 (small-signal, should stay near-linear):\n" );
        bool nearLinear = true;
        for ( float x = -5.0f; x <= 5.0001f; x += 1.0f )
        {
            float y = process( x, 0.1f, 0.0f );
            printf( "  x=%+5.2f -> y=%+.4f (x*fold=%+.4f)\n", x, y, x * 0.1f );
            if ( fabsf( y - x * 0.1f ) > 1e-4f ) nearLinear = false;
        }
        check( nearLinear, "fold=0.1 stays within the identity region for |x|<=5" );
    }

    // --- Higher fold produces more corners (more sign changes) over the
    //     same input sweep -- the "more drive = more folds" behavior. ---
    {
        auto countSignChanges = [&]( float fold ) {
            int changes = 0;
            float prev = process( -5.0f, fold, 0.0f );
            for ( float x = -4.99f; x <= 5.0001f; x += 0.01f )
            {
                float y = process( x, fold, 0.0f );
                if ( ( y > 0.0f ) != ( prev > 0.0f ) ) ++changes;
                prev = y;
            }
            return changes;
        };
        int c1 = countSignChanges( 1.0f );
        int c5 = countSignChanges( 5.0f );
        int c10 = countSignChanges( 10.0f );
        printf( "\nSign-change count over x in [-5,5]: fold=1 -> %d, fold=5 -> %d, fold=10 -> %d\n", c1, c5, c10 );
        check( c5 > c1 && c10 > c5, "higher fold produces more sign changes (more folds) over the same input range" );
    }

    // --- Bias asymmetrizes the fold: with bias != 0, process(x) should NOT
    //     be an odd function anymore (process(-x) != -process(x)). ---
    {
        bool asymmetric = false;
        for ( float x = 0.3f; x <= 3.0f; x += 0.3f )
        {
            float yPos = process( x, 2.0f, 1.3f );
            float yNeg = process( -x, 2.0f, 1.3f );
            if ( fabsf( yPos + yNeg ) > 1e-3f ) asymmetric = true;
        }
        printf( "\nBias asymmetry check (fold=2.0, bias=1.3): %s\n", asymmetric ? "asymmetric (process(-x) != -process(x))" : "still symmetric" );
        check( asymmetric, "nonzero bias breaks odd symmetry, as the doc's 'introduces even harmonics' claim requires" );

        bool symmetric = true;
        for ( float x = 0.3f; x <= 3.0f; x += 0.3f )
        {
            float yPos = process( x, 2.0f, 0.0f );
            float yNeg = process( -x, 2.0f, 0.0f );
            if ( fabsf( yPos + yNeg ) > 1e-4f ) symmetric = false;
        }
        check( symmetric, "bias=0 keeps the fold an odd function (no asymmetry introduced)" );
    }

    // --- Self-bounding: output must stay within [-1,1] for any fold/bias/x. ---
    {
        bool bounded = true;
        for ( float fold = 0.0f; fold <= 20.0f; fold += 2.5f )
            for ( float bias = -4.0f; bias <= 4.0f; bias += 1.0f )
                for ( float x = -50.0f; x <= 50.0f; x += 1.3f )
                {
                    float y = process( x, fold, bias );
                    if ( y > 1.0f + 1e-4f || y < -1.0f - 1e-4f ) bounded = false;
                }
        check( bounded, "output stays within [-1,1] across a wide fold/bias/x sweep" );
    }

    // --- fold=0: degenerates to a constant (foldShape(bias) alone) --
    //     document/confirm this explicitly, since it's the stated
    //     rationale for treating fold as curve-only starting from 0. ---
    {
        bool allSame = true;
        float first = process( -10.0f, 0.0f, 1.7f );
        for ( float x = -10.0f; x <= 10.0001f; x += 1.0f )
            if ( fabsf( process( x, 0.0f, 1.7f ) - first ) > 1e-6f ) allSame = false;
        check( allSame, "fold=0 degenerates to a constant regardless of x, matching the documented rationale" );
    }

    // --- Curve-mapping helpers: endpoints should hit the documented range exactly. ---
    {
        check( fabsf( foldFromCurve( -5.0f ) - kFoldMin ) < 1e-5f, "foldFromCurve(-5V) == kFoldMin (0.0)" );
        check( fabsf( foldFromCurve( 5.0f ) - kFoldMax ) < 1e-5f, "foldFromCurve(+5V) == kFoldMax (20.0)" );
        check( fabsf( biasFromCurve( -5.0f ) - kBiasMin ) < 1e-5f, "biasFromCurve(-5V) == kBiasMin (-4.0)" );
        check( fabsf( biasFromCurve( 5.0f ) - kBiasMax ) < 1e-5f, "biasFromCurve(+5V) == kBiasMax (+4.0)" );
    }

    printf( "\n%s (%d failures)\n", g_failures == 0 ? "P3 WAVEFOLDER: TESTS PASS" : "P3 WAVEFOLDER: FAILURES", g_failures );
    return g_failures == 0 ? 0 : 1;
}
