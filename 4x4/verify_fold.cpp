// verify_fold.cpp — Step 8 design-verification tool (not a plugin file).
// Derives and checks the exact geometric "mirror" fold formula for P3 per
// 4x4-System-Overview.md §4: a mod + absolute-value construction, period 4,
// identity on [-1,1], reflecting at the boundaries +-1, +-3, +-5, ...
//
// Closed form: foldShape(u) = 1 - | wrap4(u + 1) - 2 |, where
// wrap4(a) = a - 4*floor(a/4) is the always-nonnegative mod into [0,4).
// This is the same formula implemented in p3_wavefolder.h (Step 9).
//
// Build: g++ -std=c++11 -O2 -o verify_fold verify_fold.cpp
#include <cstdio>
#include <cmath>

static float wrap4( float a )
{
    return a - 4.0f * floorf( a / 4.0f );
}

static float foldShape( float u )
{
    return 1.0f - fabsf( wrap4( u + 1.0f ) - 2.0f );
}

int main()
{
    printf( "Identity check on [-1,1] (foldShape(u) should equal u):\n" );
    bool identityOk = true;
    for ( float u = -1.0f; u <= 1.0001f; u += 0.25f )
    {
        float y = foldShape( u );
        printf( "  u=%+.2f -> %+.4f (err %.5f)\n", u, y, fabsf( y - u ) );
        if ( fabsf( y - u ) > 1e-4f ) identityOk = false;
    }
    printf( "Identity on [-1,1]: %s\n\n", identityOk ? "ok" : "FAIL" );

    printf( "Corner/reflection checks:\n" );
    struct { float u, expected; } cases[] = {
        { 1.0f, 1.0f }, { 1.5f, 0.5f }, { 2.0f, 0.0f }, { 2.5f, -0.5f }, { 3.0f, -1.0f },
        { 3.5f, -0.5f }, { 4.0f, 0.0f }, { 5.0f, 1.0f },
        { -1.0f, -1.0f }, { -2.0f, 0.0f }, { -3.0f, 1.0f }, { -4.0f, 0.0f }, { -5.0f, -1.0f },
    };
    bool cornersOk = true;
    for ( auto& c : cases )
    {
        float y = foldShape( c.u );
        bool ok = fabsf( y - c.expected ) < 1e-4f;
        printf( "  u=%+.2f -> %+.4f (expected %+.4f) %s\n", c.u, y, c.expected, ok ? "ok" : "FAIL" );
        if ( !ok ) cornersOk = false;
    }
    printf( "Corner/reflection pattern: %s\n\n", cornersOk ? "ok" : "FAIL" );

    printf( "Boundedness check over a wide sweep (must stay within [-1,1]):\n" );
    bool bounded = true;
    float maxAbs = 0.0f;
    for ( float u = -1000.0f; u <= 1000.0f; u += 0.0137f )
    {
        float y = foldShape( u );
        if ( y > 1.0f + 1e-4f || y < -1.0f - 1e-4f ) bounded = false;
        if ( fabsf( y ) > maxAbs ) maxAbs = fabsf( y );
    }
    printf( "  max |output| over sweep: %.6f, bounded: %s\n\n", maxAbs, bounded ? "ok" : "FAIL" );

    printf( "Period check (f(u) should equal f(u+4) everywhere):\n" );
    bool periodic = true;
    for ( float u = -10.0f; u <= 10.0f; u += 0.37f )
    {
        float y1 = foldShape( u );
        float y2 = foldShape( u + 4.0f );
        if ( fabsf( y1 - y2 ) > 1e-4f ) periodic = false;
    }
    printf( "  period-4 consistency: %s\n\n", periodic ? "ok" : "FAIL" );

    printf( "Transfer curve, u from -5 to 5, step 0.5 (sanity print):\n" );
    for ( float u = -5.0f; u <= 5.0001f; u += 0.5f )
        printf( "  u=%+5.2f -> y=%+.4f\n", u, foldShape( u ) );

    bool allOk = identityOk && cornersOk && bounded && periodic;
    printf( "\n%s\n", allOk ? "P3 FOLD FORMULA: VERIFIED" : "P3 FOLD FORMULA: FAILURES" );
    return allOk ? 0 : 1;
}
