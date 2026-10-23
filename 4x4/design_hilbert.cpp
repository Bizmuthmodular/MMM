// design_hilbert.cpp — Step 6 design tool (not a plugin file, desktop-only,
// may use STL freely since it never runs on the module).
//
// HONEST NOTE ON METHOD: an initial attempt recalled a coefficient set from
// memory, believed (from memory) to be a well-known published IIR
// allpass-pair Hilbert transformer. Per CLAUDE.md's "not fabricated" rule
// and the plan's explicit requirement ("must come from a validated filter
// design reference or be properly derived — not fabricated"), that
// recollection was checked numerically before being trusted — and it
// FAILED the check (computed phase difference between the two chains was
// nowhere near the required 90 degrees across the band). Memory of an
// unverified number is not a validated source, so it was discarded.
//
// What this file does instead: DERIVES coefficients via a real numerical
// filter-design method — nonlinear least-squares (Levenberg-Marquardt,
// numerical Jacobian) minimizing the actual phase-difference error against
// the literal 90-degree target, evaluated at a dense frequency grid across
// the audio band, from multiple random restarts to avoid a poor local
// minimum (a first attempt with naive coordinate descent landed in one —
// worst-case error >150 degrees, rejected below rather than reported as
// success). The objective being minimized IS the criterion stated in
// 4x4-System-Overview.md §3, so a converged low-residual result,
// independently re-verified by direct frequency-response computation
// below, is a legitimate derivation — not a guess dressed up as one.
//
// Structure: two chains, each a cascade of N first-order allpass sections
// H(Z) = (a + Z) / (1 + a*Z), Z = z^-2. Coefficients are optimized in an
// unconstrained reparametrization a = tanh(x) to keep |a| < 1 (stability)
// automatically satisfied throughout the search.
//
// CRITICAL STRUCTURAL FINDING: with the plain phaseA(w) - phaseB(w)
// objective, optimization converges beautifully below ~2kHz (sub-degree
// error) but the error explodes to ~180 degrees by 20kHz — confirmed before
// committing to it, not just assumed. Root cause: Z=z^-2 depending on 2*w
// means each section's response completes an extra cycle within the audio
// band, and the two chains' responses only stay in the right relationship
// through roughly a quarter of the full 0..Nyquist range this way. Adding
// one full-rate sample of extra delay (z^-1, i.e. an extra -w phase) to
// chain A relative to chain B — confirmed empirically, then baked into the
// objective below as `pa - omega - pb` — fixes this across the ENTIRE
// Nyquist range. This matches the standard real-world implementation of
// these filters: chain A's output needs an explicit one-sample delay
// applied before being combined with chain B's output as the quadrature
// pair.
//
// FINAL ADOPTED RESULT (rng seed 12345, deterministic — rerunning this file
// reproduces the same coefficients): 6 sections per chain, worst-case error
// 0.119 degrees across the full [20Hz,20kHz] design band (most of the band
// under 0.05 degrees). Usable range extends further in practice: error
// stays under 1 degree from ~16Hz up to ~23.5kHz; below ~16Hz it degrades
// quickly (7.97 deg at 10Hz, 30.8 deg at 5Hz) — this is the "allpass Hilbert
// approximations typically degrade at the low end" caveat
// 4x4-System-Overview.md §3 anticipated, now with concrete numbers instead
// of a guess:
//   Chain A: -0.9983674986 -0.2712161970 -0.9887756210 -0.9625251036 -0.6730934176 -0.8841466094
//   Chain B: -0.8019822326 -0.4900702536 -0.9336325065 -0.0787602760 -0.9945280131 -0.9791624864
//   (chain A's output additionally delayed by one full-rate sample relative
//   to chain B's output when the two are combined downstream in Step 7)
// Build: g++ -std=c++17 -O2 -o design_hilbert design_hilbert.cpp
#include <complex>
#include <cstdio>
#include <cmath>
#include <vector>
#include <random>
#include <algorithm>

typedef std::complex<double> cd;
static const double kPi = 3.14159265358979323846;
static const int kSections = 6;   // per chain
static const int kParams = 2 * kSections;

static double chainPhase( const double* coeffs, int n, double omega )
{
    cd Z = std::exp( cd( 0.0, -2.0 * omega ) );
    double totalPhase = 0.0;
    for ( int i = 0; i < n; ++i )
    {
        cd H = ( coeffs[i] + Z ) / ( 1.0 + coeffs[i] * Z );
        totalPhase += std::arg( H );
    }
    return totalPhase;
}

struct Problem
{
    double fs, fLow, fHigh;
    int nFreqs;
    double target;   // radians

    std::vector<double> freqs() const
    {
        std::vector<double> f( nFreqs );
        for ( int i = 0; i < nFreqs; ++i )
        {
            double t = (double)i / (double)( nFreqs - 1 );
            f[i] = fLow * pow( fHigh / fLow, t );
        }
        return f;
    }

    // residual for frequency sample i, given raw coefficients a[kSections], b[kSections].
    // Structural correction confirmed empirically before this objective was
    // written (see file header): chain A's output needs an extra one-sample
    // delay (z^-1, i.e. an extra -omega phase) relative to chain B for the
    // two-chain phase difference to hold near -90 degrees across the WHOLE
    // Nyquist range, not just a narrow low-frequency sub-band. Without it,
    // the plain phaseA-phaseB difference only tracked -90 degrees up to
    // ~2kHz and then blew up to 180 degrees of error by 20kHz.
    double residual( const double* a, const double* b, double f ) const
    {
        double omega = 2.0 * kPi * f / fs;
        double pa = chainPhase( a, kSections, omega );
        double pb = chainPhase( b, kSections, omega );
        double diff = pa - omega - pb;
        while ( diff > kPi ) diff -= 2.0 * kPi;
        while ( diff <= -kPi ) diff += 2.0 * kPi;
        return diff - target;
    }
};

static void unpack( const std::vector<double>& x, double* a, double* b )
{
    for ( int i = 0; i < kSections; ++i ) a[i] = tanh( x[i] );
    for ( int i = 0; i < kSections; ++i ) b[i] = tanh( x[kSections + i] );
}

// Levenberg-Marquardt with numerical Jacobian (finite differences).
static double runLM( Problem& prob, std::vector<double>& x, int maxIters = 300 )
{
    std::vector<double> freqs = prob.freqs();
    int m = (int)freqs.size();
    double lambda = 1e-3;
    double a[kSections], b[kSections];
    unpack( x, a, b );

    std::vector<double> r( m );
    for ( int i = 0; i < m; ++i ) r[i] = prob.residual( a, b, freqs[i] );
    double cost = 0.0; for ( double v : r ) cost += v * v;

    const double h = 1e-6;
    for ( int iter = 0; iter < maxIters; ++iter )
    {
        // Jacobian: m x kParams
        std::vector<std::vector<double>> J( m, std::vector<double>( kParams ) );
        for ( int p = 0; p < kParams; ++p )
        {
            std::vector<double> xp = x;
            xp[p] += h;
            double ap[kSections], bp[kSections];
            unpack( xp, ap, bp );
            for ( int i = 0; i < m; ++i )
            {
                double rp = prob.residual( ap, bp, freqs[i] );
                J[i][p] = ( rp - r[i] ) / h;
            }
        }
        // Normal equations: (J^T J + lambda*I) delta = -J^T r
        double JtJ[kParams][kParams] = {};
        double Jtr[kParams] = {};
        for ( int p = 0; p < kParams; ++p )
        {
            for ( int q = 0; q < kParams; ++q )
            {
                double s = 0.0;
                for ( int i = 0; i < m; ++i ) s += J[i][p] * J[i][q];
                JtJ[p][q] = s;
            }
            double s = 0.0;
            for ( int i = 0; i < m; ++i ) s += J[i][p] * r[i];
            Jtr[p] = s;
        }
        for ( int p = 0; p < kParams; ++p ) JtJ[p][p] += lambda * ( JtJ[p][p] + 1e-9 );

        // Solve via Gaussian elimination (kParams is small: 12).
        double A[kParams][kParams + 1];
        for ( int p = 0; p < kParams; ++p )
        {
            for ( int q = 0; q < kParams; ++q ) A[p][q] = JtJ[p][q];
            A[p][kParams] = -Jtr[p];
        }
        bool singular = false;
        for ( int col = 0; col < kParams; ++col )
        {
            int piv = col;
            for ( int row = col + 1; row < kParams; ++row )
                if ( fabs( A[row][col] ) > fabs( A[piv][col] ) ) piv = row;
            if ( fabs( A[piv][col] ) < 1e-14 ) { singular = true; break; }
            std::swap( A[col], A[piv] );
            for ( int row = 0; row < kParams; ++row )
            {
                if ( row == col ) continue;
                double factor = A[row][col] / A[col][col];
                for ( int c = col; c <= kParams; ++c ) A[row][c] -= factor * A[col][c];
            }
        }
        std::vector<double> delta( kParams, 0.0 );
        if ( !singular )
            for ( int p = 0; p < kParams; ++p ) delta[p] = A[p][kParams] / A[p][p];

        std::vector<double> xNew = x;
        for ( int p = 0; p < kParams; ++p ) xNew[p] += delta[p];
        double aNew[kSections], bNew[kSections];
        unpack( xNew, aNew, bNew );
        std::vector<double> rNew( m );
        for ( int i = 0; i < m; ++i ) rNew[i] = prob.residual( aNew, bNew, freqs[i] );
        double costNew = 0.0; for ( double v : rNew ) costNew += v * v;

        if ( !singular && costNew < cost )
        {
            x = xNew; r = rNew; cost = costNew;
            lambda *= 0.7;
        }
        else
        {
            lambda *= 2.0;
            if ( lambda > 1e8 ) break;
        }
    }
    return cost;
}

int main()
{
    Problem prob;
    prob.fs = 48000.0;
    prob.fLow = 20.0;
    prob.fHigh = 20000.0;
    prob.nFreqs = 300;
    prob.target = -kPi / 2.0;

    std::mt19937 rng( 12345 );
    std::uniform_real_distribution<double> dist( -2.0, 2.0 );

    double bestCost = 1e300;
    std::vector<double> bestX;

    const int kRestarts = 40;
    for ( int trial = 0; trial < kRestarts; ++trial )
    {
        std::vector<double> x( kParams );
        for ( int i = 0; i < kParams; ++i ) x[i] = dist( rng );
        double cost = runLM( prob, x );
        if ( cost < bestCost ) { bestCost = cost; bestX = x; }
    }

    double a[kSections], b[kSections];
    unpack( bestX, a, b );
    printf( "Best mean-squared residual over %d restarts: %.6e\n", kRestarts, bestCost / prob.nFreqs );

    double worstErrDeg = 0.0;
    std::vector<double> freqs = prob.freqs();
    for ( double f : freqs )
    {
        double r = prob.residual( a, b, f );
        double errDeg = fabs( r ) * 180.0 / kPi;
        if ( errDeg > worstErrDeg ) worstErrDeg = errDeg;
    }
    printf( "Worst abs error over design band [%.0f,%.0f]Hz: %.4f deg\n\n", prob.fLow, prob.fHigh, worstErrDeg );

    printf( "Chain A coefficients:" );
    for ( int i = 0; i < kSections; ++i ) printf( " %.10f", a[i] );
    printf( "\nChain B coefficients:" );
    for ( int i = 0; i < kSections; ++i ) printf( " %.10f", b[i] );
    printf( "\n\n" );

    printf( "%10s  %12s  %10s\n", "freq(Hz)", "diff(deg)", "err(deg)" );
    double checkFreqs[] = { 5, 10, 15, 20, 25, 30, 50, 100, 200, 500, 1000, 2000, 5000, 10000, 15000, 20000, 21000, 22000, 23000 };
    for ( double f : checkFreqs )
    {
        double r = prob.residual( a, b, f );
        double diffDeg = r * 180.0 / kPi - 90.0;
        double errDeg = fabs( r ) * 180.0 / kPi;
        printf( "%10.1f  %12.4f  %10.4f\n", f, diffDeg, errDeg );
    }

    return 0;
}
