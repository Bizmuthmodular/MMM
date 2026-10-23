// fft.h — in-place, float32, radix-2 complex FFT/IFFT.
// Implementation Plan Step 2. Standalone primitive, no plugin wiring yet.
//
// No dynamic allocation, no STL, no exceptions/RTTI — portable float C++11,
// <cmath> only (matches the platform's hard constraints, api.h not needed
// here since this is pure math with no disting NT API dependency).
//
// Supports any power-of-two size from 2 up to kFFTMaxSize (2048), sized per
// 4x4-System-Overview.md §2 (1024-sample analysis block, 2x zero-padded to
// 2048 for the frequency-domain pitch shift).
//
//   fft::initTwiddles();                 // once, e.g. from initialise()
//   fft::Complex buf[2048];
//   ...fill buf...
//   fft::transform( buf, 1024, false );  // forward, 1024-point
//   fft::transform( buf, 2048, true  );  // inverse, 2048-point
#pragma once
#include <cmath>
#include <cstdint>

namespace fft {

static const int kFFTMaxSize = 2048;
static const int kTwiddleCount = kFFTMaxSize / 2;   // 1024

struct Complex {
    float re;
    float im;
};

// Twiddle table for the largest supported size (2048). Smaller transforms
// index into this same table with a stride, since W_n^k = W_max^(k * max/n)
// for any power-of-two n <= max. Avoids per-call sinf/cosf in the hot loop
// and avoids needing one table per size.
static Complex g_twiddle[kTwiddleCount];
static bool    g_twiddleReady = false;

inline void initTwiddles()
{
    if ( g_twiddleReady ) return;
    const float kTwoPi = 6.283185307f;
    for ( int k = 0; k < kTwiddleCount; ++k )
    {
        float angle = -kTwoPi * (float)k / (float)kFFTMaxSize;   // forward convention
        g_twiddle[k].re = cosf( angle );
        g_twiddle[k].im = sinf( angle );
    }
    g_twiddleReady = true;
}

// Bit-reversal permutation, in place. n must be a power of two.
inline void bitReverse( Complex* data, int n )
{
    int j = 0;
    for ( int i = 0; i < n - 1; ++i )
    {
        if ( i < j )
        {
            Complex t = data[i];
            data[i] = data[j];
            data[j] = t;
        }
        int m = n >> 1;
        while ( m >= 1 && j >= m )
        {
            j -= m;
            m >>= 1;
        }
        j += m;
    }
}

// In-place iterative Cooley-Tukey radix-2 FFT/IFFT.
// n must be a power of two, 2 <= n <= kFFTMaxSize.
// inverse=true also applies the 1/n scaling (so transform(transform(x,false)),true) == x).
inline void transform( Complex* data, int n, bool inverse )
{
    bitReverse( data, n );

    const int stride = kFFTMaxSize / n;   // index step into g_twiddle for this size

    for ( int size = 2; size <= n; size <<= 1 )
    {
        int half = size >> 1;
        int tStep = stride * ( n / size );   // twiddle index step for this stage

        for ( int start = 0; start < n; start += size )
        {
            int tIdx = 0;
            for ( int k = 0; k < half; ++k )
            {
                Complex w = g_twiddle[tIdx];
                if ( inverse ) w.im = -w.im;   // conjugate for inverse transform

                Complex a = data[start + k];
                Complex b = data[start + k + half];

                // b * w
                Complex bw;
                bw.re = b.re * w.re - b.im * w.im;
                bw.im = b.re * w.im + b.im * w.re;

                data[start + k]        = Complex{ a.re + bw.re, a.im + bw.im };
                data[start + k + half] = Complex{ a.re - bw.re, a.im - bw.im };

                tIdx += tStep;
            }
        }
    }

    if ( inverse )
    {
        float invN = 1.0f / (float)n;
        for ( int i = 0; i < n; ++i )
        {
            data[i].re *= invN;
            data[i].im *= invN;
        }
    }
}

} // namespace fft
