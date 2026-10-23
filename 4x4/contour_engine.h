// contour_engine.h — Contour function generator engine (rounding-radius version), reference implementation.
// Self-contained: depends only on <math.h> and <stdint.h>. No heap, no exceptions, no RTTI, C++11.
// Verified sample-for-sample against the Contour Radius Generator web page (see CONTOUR_NT_SPEC.md).
//
//   contour::Engine eng;                 // Params defaults = page defaults
//   eng.setSampleRate(48000.0f);
//   eng.rebuild();                       // after changing eng.params (draws the random plan)
//   eng.trigger();                       // start or restart; the envelope level carries on
//   float volts = eng.process();         // once per sample, output in volts
#pragma once
#include <math.h>
#include <stdint.h>

namespace contour {

struct Params {
  float    lengthSec = 6.0f;   // 1..10 s
  int      windows   = 6;        // 1..32 equal windows, 2 envelopes in each
  float    chaos     = 0.6f;   // pulls each rate/level toward a random 0..99 target
  float    radiusSec = 0.12f;   // corner rounding: blends from radius before to radius after
  float    roundProb = 1.0f;   // share of the length where corners are rounded
  float    holdProb  = 0.0f;   // share of the length that is held
  float    levelV    = 8.0f;   // volts: 0..level, or +/-level when bipolar
  float    offsetV   = 0.0f;
  bool     bipolar   = false;
  bool     loop      = false;
  uint32_t seed      = 7u;
};

class Engine {
public:
  struct Env { double a, b; int win, sub, r1, l1, r2, l2; };   // start/end in seconds, rates, levels

  Params params;

  explicit Engine(const Params& p = Params()) : params(p) { rebuild(); }

  void  setSampleRate(float sr) { dt_ = 1.0 / sr; }
  void  reseed(uint32_t s)      { params.seed = s; rebuild(); }
  bool  running() const         { return running_; }
  float durationSec() const     { return (float)T_; }
  int   envelopeCount() const   { return nEnv_; }
  int   breakpointCount() const { return nPts_; }
  const Env& envelope(int i) const { return env_[i]; }
  double positionSec() const    { return t_; }
  // Fill out[0..n-1] with the current curve at n evenly spaced times from 0 to the length
  // (single pass; for a display). Does not change playback state. n >= 2.
  void renderCurve(float* out, int n) const {
    int k = 0;
    for (int i = 0; i < n; ++i) {
      const double t = T_ * i / (n - 1);
      while (k < nPts_ - 2 && t >= pt_[k + 1].t) ++k;
      out[i] = (float)eval(k, t);
    }
  }

  // Start or restart. The envelope level carries on from where it is, so the curve is re-planned
  // from that level (a few hundred simple operations, no random numbers).
  void trigger() {
    passStart_ = primed_ ? levelAt(t_) : (double)env_[nEnv_ - 1].l2;
    buildCurve(passStart_);
    primed_ = true; t_ = 0.0; k_ = 0; running_ = true;
  }

  // Draw the random plan from params (deterministic per seed): envelopes, rounded and held regions,
  // then rebuild the curve. Call after changing params.
  void rebuild() {
    const Params& p = params;
    T_ = clampd(p.lengthSec, 1.0, 10.0);
    const int W = imax(1, imin(p.windows, (int)kMaxWin));
    nEnv_ = 0;
    for (int wi = 0; wi < W; ++wi) {
      const double a = wi * T_ / W, b = (wi == W - 1) ? T_ : (wi + 1) * T_ / W, base = (b - a) / kEnvsPerWindow;
      double t = a;
      for (int s = 0; s < kEnvsPerWindow; ++s) {
        const double l = (s == kEnvsPerWindow - 1) ? b - t : base;
        const uint32_t idx = (uint32_t)nEnv_;
        // base rate is 0 and base level is random in 0..99; chaos pulls each toward its own random target
        auto pull = [&](double v, uint32_t j) { return (int)floor(v + p.chaos * (99.0 * rnd(3000 + idx, j) - v) + 0.5); };
        Env& e = env_[nEnv_++];
        e.a = t; e.b = t + l; e.win = wi; e.sub = s;
        e.r1 = pull(0.0, 1); e.l1 = pull(99.0 * rnd(2000 + idx, 2), 2);
        e.r2 = pull(0.0, 3); e.l2 = pull(99.0 * rnd(2000 + idx, 4), 4);
        t += l;
      }
    }
    nRound_ = probWindows(p.roundProb, 500000, round_);
    nHold_  = probWindows(p.holdProb,  600000, hold_);
    // Re-plan the current pass from the level it started at. Playback position and run state are
    // kept, so turning a parameter reshapes the curve without restarting it.
    buildCurve(passStart_ >= 0.0 ? passStart_ : (double)env_[nEnv_ - 1].l2);
    k_ = 0;
  }

  // One output sample in volts. Holds the last value when stopped.
  float process() {
    if (!running_) return out_;
    const double t = t_;
    while (k_ < nPts_ - 2 && t >= pt_[k_ + 1].t) ++k_;
    out_ = (float)eval(k_, t);
    t_ += dt_;
    if (t_ >= T_) { if (params.loop) trigger(); else running_ = false; }
    return out_;
  }

private:
  enum { kMaxWin = 32, kEnvsPerWindow = 2, kMaxEnv = kMaxWin * kEnvsPerWindow,
         kPieces = 24, kMaxLv = 3 * kMaxEnv + 2, kMaxPts = 3 * kMaxLv + 2 * kPieces + 8 };
  struct Span { double a, b; };
  struct Pt { double t, vl, vr, r, s1, s2; };   // time, value left/right of it, radius, slope in/out

  static double clampd(double v, double lo, double hi) { return v < lo ? lo : (v > hi ? hi : v); }
  static double dmin(double a, double b) { return a < b ? a : b; }
  static double dmax(double a, double b) { return a > b ? a : b; }
  static int    imin(int a, int b) { return a < b ? a : b; }
  static int    imax(int a, int b) { return a > b ? a : b; }
  static double sweepSec(int rate) { return 20.0 * pow(2.0, -rate / 7.5); }   // full 0..99 move

  // mulberry32 keyed by (seed, i, j): bit-identical to the page's rnd(i, j)
  double rnd(uint32_t i, uint32_t j) const {
    uint32_t a = (uint32_t)((uint64_t)params.seed * 100003ull + (uint64_t)i * 7919ull + (uint64_t)j * 104729ull);
    a += 0x6D2B79F5u;
    uint32_t t = (a ^ (a >> 15)) * (1u | a);
    t = (t + ((t ^ (t >> 7)) * (61u | t))) ^ t;
    return (double)(t ^ (t >> 14)) / 4294967296.0;
  }

  // 24 random-length pieces visited in random order until prob * T is covered, then merged.
  int probWindows(float prob, uint32_t base, Span* out) const {
    if (prob >= 1.0f) { out[0] = {0.0, T_}; return 1; }
    if (prob <= 0.0f) return 0;
    double w[kPieces], a[kPieces], len[kPieces], sum = 0.0; int order[kPieces];
    for (int k = 0; k < kPieces; ++k) { w[k] = 0.25 + rnd(base + k, 1); sum += w[k]; order[k] = k; }
    double acc = 0.0;
    for (int k = 0; k < kPieces; ++k) { a[k] = acc; len[k] = w[k] / sum * T_; acc += len[k]; }
    double key[kPieces];
    for (int k = 0; k < kPieces; ++k) key[k] = rnd(base + k, 2);
    for (int i = 1; i < kPieces; ++i) {                          // insertion sort of order[] by key
      const int o = order[i]; int j = i - 1;
      while (j >= 0 && key[order[j]] > key[o]) { order[j + 1] = order[j]; --j; }
      order[j + 1] = o;
    }
    Span picked[kPieces]; int np = 0; double need = prob * T_;
    for (int oi = 0; oi < kPieces && need > 1e-9; ++oi) {
      const int k = order[oi];
      if (len[k] <= need) { picked[np++] = {a[k], a[k] + len[k]}; need -= len[k]; }
      else { const double off = rnd(base + k, 3) * (len[k] - need); picked[np++] = {a[k] + off, a[k] + off + need}; need = 0.0; }
    }
    for (int i = 1; i < np; ++i) {                               // insertion sort of picked[] by start
      const Span sp = picked[i]; int j = i - 1;
      while (j >= 0 && picked[j].a > sp.a) { picked[j + 1] = picked[j]; --j; }
      picked[j + 1] = sp;
    }
    int n = 0;
    for (int i = 0; i < np; ++i) {
      if (n && picked[i].a <= out[n - 1].b + 1e-9) out[n - 1].b = dmax(out[n - 1].b, picked[i].b);
      else out[n++] = picked[i];
    }
    return n;
  }

  // Envelope level (0..99) at time t of the current pass, before volts, holds and rounding.
  double levelAt(double t) const {
    int i = 0; while (i < nLv_ - 2 && t >= lvT_[i + 1]) ++i;
    const double u = clampd((t - lvT_[i]) / (lvT_[i + 1] - lvT_[i]), 0.0, 1.0);
    return lvV_[i] + (lvV_[i + 1] - lvV_[i]) * u;
  }
  double clippedAt(double t) const {
    int i = 0; while (i < nC_ - 2 && t >= cT_[i + 1]) ++i;
    const double u = clampd((t - cT_[i]) / (cT_[i + 1] - cT_[i]), 0.0, 1.0);
    return cV_[i] + (cV_[i + 1] - cV_[i]) * u;
  }

  // Turn the plan into breakpoints: levels -> volts with rail clipping -> holds -> corner radii.
  void buildCurve(double startLevel) {
    const Params& p = params;
    // 1. envelope levels as a continuous polyline: toward L1 at R1, then L2 at R2, then flat
    nLv_ = 0; double cur = startLevel;
    auto pushLv = [&](double t, double v) {
      if (nLv_ && t - lvT_[nLv_ - 1] < 1e-9) { lvV_[nLv_ - 1] = v; return; }
      if (nLv_ < kMaxLv) { lvT_[nLv_] = t; lvV_[nLv_++] = v; }
    };
    pushLv(0.0, cur);
    for (int i = 0; i < nEnv_; ++i) {
      const Env& e = env_[i]; double t = e.a; pushLv(t, cur);
      for (int st = 0; st < 2 && t < e.b - 1e-12; ++st) {
        const int tgt = st ? e.l2 : e.l1;
        const double sp = 99.0 / sweepSec(st ? e.r2 : e.r1), need = fabs(tgt - cur) / sp;
        if (t + need <= e.b) { t += need; cur = tgt; }
        else { cur += (tgt > cur ? 1.0 : -1.0) * sp * (e.b - t); t = e.b; }
        pushLv(t, cur);
      }
    }
    pushLv(T_, cur);
    // 2. volts, clipped at the +/-10 V rails (a point is added where a segment crosses a rail)
    auto toV = [&](double L) { const double y = L / 99.0; return (p.bipolar ? p.levelV * (2.0 * y - 1.0) : p.levelV * y) + p.offsetV; };
    nC_ = 0;
    auto pushC = [&](double t, double v) { if (nC_ < kMaxPts) { cT_[nC_] = t; cV_[nC_++] = v; } };
    const double rails[2] = {-10.0, 10.0};
    for (int i = 0; i < nLv_; ++i) {
      const double v0 = toV(lvV_[i]);
      pushC(lvT_[i], clampd(v0, -10.0, 10.0));
      if (i + 1 < nLv_) {
        const double v1 = toV(lvV_[i + 1]); double u[2], rv[2]; int nc = 0;
        for (int q = 0; q < 2; ++q) if ((v0 - rails[q]) * (v1 - rails[q]) < 0.0) { u[nc] = (rails[q] - v0) / (v1 - v0); rv[nc++] = rails[q]; }
        if (nc == 2 && u[0] > u[1]) { double x = u[0]; u[0] = u[1]; u[1] = x; x = rv[0]; rv[0] = rv[1]; rv[1] = x; }
        for (int c = 0; c < nc; ++c) pushC(lvT_[i] + u[c] * (lvT_[i + 1] - lvT_[i]), rv[c]);
      }
    }
    // 3. holds: freeze at the value on entry, jump back on exit
    nPts_ = 0;
    auto pushP = [&](double t, double vl, double vr) { if (nPts_ < kMaxPts) pt_[nPts_++] = {t, vl, vr, 0.0, 0.0, 0.0}; };
    int ci = 0;
    for (int h = 0; h < nHold_; ++h) {
      const double a = hold_[h].a, b = hold_[h].b;
      while (ci < nC_ && cT_[ci] < a - 1e-9) { pushP(cT_[ci], cV_[ci], cV_[ci]); ++ci; }
      const double va = clippedAt(a), vb = clippedAt(b);
      pushP(a, va, va);
      pushP(b, va, b >= T_ - 1e-9 ? va : vb);
      while (ci < nC_ && cT_[ci] <= b + 1e-9) ++ci;
    }
    while (ci < nC_) { pushP(cT_[ci], cV_[ci], cV_[ci]); ++ci; }
    // 4. radius per breakpoint: the dial value inside a rounding window, never more than half
    //    the distance to a neighbour, so rounded corners cannot overlap
    for (int i = 1; i < nPts_; ++i) {
      Pt& q = pt_[i]; const double d1 = q.t - pt_[i - 1].t;
      q.s1 = (q.vl - pt_[i - 1].vr) / d1;
      if (i == nPts_ - 1) break;
      const double d2 = pt_[i + 1].t - q.t;
      q.s2 = (pt_[i + 1].vl - q.vr) / d2;
      bool in = false;
      for (int w = 0; w < nRound_; ++w) if (q.t >= round_[w].a && q.t <= round_[w].b) in = true;
      q.r = (in && p.radiusSec > 0.0f) ? dmin((double)p.radiusSec, dmin(d1 * 0.5, d2 * 0.5)) : 0.0;
    }
  }

  // Value on segment k. A rounded corner is a parabola tangent to both segments; a jump (the end
  // of a hold) becomes an S-curve across the same span.
  double eval(int k, double t) const {
    const Pt& A = pt_[k]; const Pt& B = pt_[k + 1];
    const double lin = A.vr + B.s1 * (t - A.t);
    if (B.r > 0.0 && t > B.t - B.r) {
      const double u = (t - (B.t - B.r)) / (2.0 * B.r);
      return lin + (B.s2 - B.s1) * B.r * u * u + (B.vr - B.vl) * u * u * (3.0 - 2.0 * u);
    }
    if (A.r > 0.0 && t < A.t + A.r) {
      const double u = (t - (A.t - A.r)) / (2.0 * A.r);
      return A.vl + A.s1 * (t - A.t) + (A.s2 - A.s1) * A.r * u * u + (A.vr - A.vl) * u * u * (3.0 - 2.0 * u);
    }
    return lin;
  }

  Env    env_[kMaxEnv];
  Span   round_[kPieces], hold_[kPieces];
  Pt     pt_[kMaxPts];
  double lvT_[kMaxLv], lvV_[kMaxLv], cT_[kMaxPts], cV_[kMaxPts];
  int    nEnv_ = 0, nRound_ = 0, nHold_ = 0, nLv_ = 0, nC_ = 0, nPts_ = 0, k_ = 0;
  double T_ = 1.0, dt_ = 1.0 / 48000.0, t_ = 0.0, passStart_ = -1.0;
  bool   running_ = false, primed_ = false;
  float  out_ = 0.0f;
};

} // namespace contour
