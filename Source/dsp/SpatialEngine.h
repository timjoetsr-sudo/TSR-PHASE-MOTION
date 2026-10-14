// TSR PHASE · DEPTH · MOTION — spatial engine (header-only, no JUCE dependency)
// TREE STUDIO RECORD AUDIO
//
// Signal flow per sample:
//   in -> [ENGINE 1 PHASE] sub-sample delay on the leading channel + optional low all-pass (R)
//      -> base (b)
//   b  -> M/S -> Linkwitz-Riley 3-band analysis; band-limited moves are ADDED to the untouched signal
//      -> [ENGINE 3 MOTION] S' = sum g_b*S_b + p_b*M_b   (M untouched -> L+R untouched)
//      -> [ENGINE 2 DEPTH]  direct HF-damping + micro-level, early reflections + late tail from M,
//                           DEPTH GUARD caps wet/direct energy
//      -> candidate (c)
//   FAIL-SAFE: out = b + s*(c-b); s in [0,1] chosen per block so that predicted mono loss and
//              correlation stay inside MONO SAFETY limits (closed-form from second moments).
//   MIX scales every move (parameter-domain mix, no dry/wet comb filtering).
//
// 1.0.3: PHASE fractional delay = 32-tap Kaiser windowed sinc (was 3rd-order Lagrange: -3.3 dB @16 kHz, -8.4 dB @20 kHz on
//        the corrected channel at 48 kHz); latency 2 -> 15 samples. Filter design frequencies clamped below Nyquist
//        (8 kHz sessions blew up). DEPTH shelf / send filter follow DEPTH smoothly (0.05 dB / 50 Hz coefficient steps
//        made HF artefacts at -71 dB under automation). CPU: meters only while a meter view is open, 4-lane meter filters,
//        cached early-reflection taps, sliced analysis jobs (no CPU spikes). Every optimisation is bit-exact vs 1.0.2.
#pragma once
#include <algorithm>
#include <array>
#include <atomic>
#include <cmath>
#include <climits>
#include <cstdint>
#include <cstring>
#include <vector>

namespace tsr {

constexpr double kPi = 3.14159265358979323846;
constexpr int kFirTaps = 32;                     // PHASE fractional delay: windowed-sinc taps (even)
constexpr int kBaseLatency = kFirTaps / 2 - 1;   // 15 samples, constant: room for a centred 32-tap interpolator
constexpr double kKaiserBeta = 7.0;              // window: <= 0.002 dB error to 18 kHz @44.1k and to 20 kHz @48k
constexpr double kMaxLagMs = 1.0;    // max phase-alignment correction
constexpr int kChunk = 256;          // internal processing chunk
constexpr float kInMax = 64.f;       // 1.0.4: input clamp (+36 dBFS): a blown-up upstream plugin no longer bends the state for minutes
inline double belowNyquist(double fs, double f) { return std::min(f, 0.45 * fs); }   // 1.0.3: fs = 8 kHz made NaN/Inf

enum Mode { MODE_PHASE = 0, MODE_DEPTH, MODE_MOTION, MODE_VOCAL, MODE_DRUMS, MODE_MUSIC };
enum Monitor { MON_ORIGINAL = 0, MON_TSR, MON_DELTA };

struct Params {
    float coherence = 0.86f;   // 0..1
    float depth = 0.58f;       // 0..1
    float earlyLate = 0.42f;   // 0 = all early, 1 = all late
    float width = 1.12f;       // 0..2
    float motion = 0.34f;      // 0..1
    float mix = 1.0f;          // 0..1 (scales every move)
    float monoSafety = 0.94f;  // 0..1
    float centerLock = 0.89f;  // 0..1
    float depthGuard = 0.83f;  // 0..1
    int mode = MODE_PHASE;
    int monitor = MON_TSR;
    bool bypass = false;
    double bpm = 120.0;
};

struct Profile { float motionMidScale, erTimeScale, lateScale, vmaxScale; };
inline Profile profileFor(int mode) {
    switch (mode) {
        case MODE_VOCAL: return {0.5f, 0.8f, 1.0f, 1.0f};
        case MODE_DRUMS: return {1.0f, 1.0f, 0.6f, 0.5f};
        default:         return {1.0f, 1.0f, 1.0f, 1.0f};
    }
}

// ---------------------------------------------------------------- meters (audio -> UI, lock-free)
struct BandView { std::atomic<float> corr{1}, balance{0}, spread{0}; };
struct Meters {
    std::atomic<float> integrityIn{0}, integrityOut{0};
    std::atomic<float> monoLossIn{0}, monoLossOut{0};
    std::atomic<float> corrIn{1}, corrOut{1};
    BandView bandIn[3], bandOut[3];
    std::atomic<float> lagMs{0}, appliedDelayMs{0}, phaseConfidence{0};
    std::atomic<int>   allpassIdx{-1};
    std::atomic<float> lowCorrBefore{1}, lowCorrAfter{1};
    std::atomic<float> depthIn{0}, depthOut{0}, earlyLateDbIn{0}, earlyLateDbOut{0}, hfIn{0}, hfOut{0};
    std::atomic<float> wetRatioDb{-120}, guardGainDb{0};
    std::atomic<float> safeScale{1}, reduction{0};
    std::atomic<float> predictedMonoDeltaDb{0}, allowedMonoDeltaDb{0};
    std::atomic<float> pan{0}, widthMod{0};
    std::atomic<float> confidence{0};
    std::atomic<float> levelDb{-120};
};

// ---------------------------------------------------------------- primitives
// Denormal guard for hosts without flush-to-zero (WebAssembly, plain C++): values below 1e-25 (-500 dB) are set to 0.
// JUCE hosts already run with FTZ (ScopedNoDenormals); there this is a no-op in practice.
inline void ftz(double& x) { if (std::fabs(x) < 1e-25) x = 0.0; }
inline void ftz(float& x) { if (std::fabs(x) < 1e-25f) x = 0.f; }
struct Biquad {
    double b0 = 1, b1 = 0, b2 = 0, a1 = 0, a2 = 0, z1 = 0, z2 = 0;
    inline float process(float x) {
        double y = b0 * x + z1;
        z1 = b1 * x - a1 * y + z2;
        z2 = b2 * x - a2 * y;
        return (float) y;
    }
    void reset() { z1 = z2 = 0; }
    void flush() { ftz(z1); ftz(z2); }
    void setNorm(double B0, double B1, double B2, double A0, double A1, double A2) {
        b0 = B0 / A0; b1 = B1 / A0; b2 = B2 / A0; a1 = A1 / A0; a2 = A2 / A0;
    }
    void lowpass(double fs, double f, double q) {
        double w = 2 * kPi * f / fs, c = std::cos(w), al = std::sin(w) / (2 * q);
        setNorm((1 - c) / 2, 1 - c, (1 - c) / 2, 1 + al, -2 * c, 1 - al);
    }
    void highpass(double fs, double f, double q) {
        double w = 2 * kPi * f / fs, c = std::cos(w), al = std::sin(w) / (2 * q);
        setNorm((1 + c) / 2, -(1 + c), (1 + c) / 2, 1 + al, -2 * c, 1 - al);
    }
    void allpass(double fs, double f, double q) {
        double w = 2 * kPi * f / fs, c = std::cos(w), al = std::sin(w) / (2 * q);
        setNorm(1 - al, -2 * c, 1 + al, 1 + al, -2 * c, 1 - al);
    }
    void highshelf(double fs, double f, double gainDb) {
        double w = 2 * kPi * f / fs;
        highshelfCS(std::cos(w), std::sin(w), gainDb);
    }
    // same formula with cos/sin of the (fixed) corner precomputed: lets DEPTH redesign the shelf every sample while it moves
    void highshelfCS(double c, double s, double gainDb) {
        double A = std::pow(10.0, gainDb / 40.0);
        double al = s / 2 * std::sqrt(2.0), sa = 2 * std::sqrt(A) * al;
        setNorm(A * ((A + 1) + (A - 1) * c + sa), -2 * A * ((A - 1) + (A + 1) * c), A * ((A + 1) + (A - 1) * c - sa),
                (A + 1) - (A - 1) * c + sa, 2 * ((A - 1) - (A + 1) * c), (A + 1) - (A - 1) * c - sa);
    }
};

// Linkwitz-Riley 4th-order low-pass (two cascaded Butterworth sections)
struct LR4LP {
    Biquad a, b;
    void set(double fs, double f) { a.lowpass(fs, f, 0.70710678); b.lowpass(fs, f, 0.70710678); }
    inline float process(float x) { return b.process(a.process(x)); }
    void reset() { a.reset(); b.reset(); }
    void flush() { a.flush(); b.flush(); }
};

struct LR4HP {
    Biquad a, b;
    void set(double fs, double f) { a.highpass(fs, f, 0.70710678); b.highpass(fs, f, 0.70710678); }
    inline float process(float x) { return b.process(a.process(x)); }
    void reset() { a.reset(); b.reset(); }
    void flush() { a.flush(); b.flush(); }
};

// Linkwitz-Riley 3-band split (low | mid | high). Bands are NOT summed back to form the signal:
// the engine adds band-limited *moves* on top of the untouched signal, so the neutral path is bit-exact.
// LR4 identity: LP4 + HP4 = AP2 (2nd-order all-pass, same f, Q = 1/sqrt2) -> HP4 = AP2 - LP4 (6 biquads instead of 8).
struct Split3 {
    LR4LP lp1, lp2; Biquad ap1, ap2;
    void set(double fs, double f1, double f2) {
        f1 = belowNyquist(fs, f1); f2 = belowNyquist(fs, f2);
        lp1.set(fs, f1); ap1.allpass(fs, f1, 0.70710678); lp2.set(fs, f2); ap2.allpass(fs, f2, 0.70710678);
    }
    inline void process(float x, float& l, float& m, float& h) {
        l = lp1.process(x);
        float r = ap1.process(x) - l;
        m = lp2.process(r);
        h = ap2.process(r) - m;
    }
    void reset() { lp1.reset(); lp2.reset(); ap1.reset(); ap2.reset(); }
    void flush() { lp1.flush(); lp2.flush(); ap1.flush(); ap2.flush(); }
};

// 1.0.3: the four metering splits (in M, in S, out M, out S) share their coefficients -> one 4-lane filter bank.
// Per lane the arithmetic is exactly Biquad::process / Split3::process (float in/out, double state): bit-identical meters,
// but the compiler can run the lanes in SIMD registers.
struct Biquad4 {
    double b0 = 1, b1 = 0, b2 = 0, a1 = 0, a2 = 0, z1[4] = {0, 0, 0, 0}, z2[4] = {0, 0, 0, 0};
    void from(const Biquad& q) { b0 = q.b0; b1 = q.b1; b2 = q.b2; a1 = q.a1; a2 = q.a2; }
    inline void process(const float* x, float* out) {
        for (int k = 0; k < 4; ++k) {
            double y = b0 * x[k] + z1[k];
            z1[k] = b1 * x[k] - a1 * y + z2[k];
            z2[k] = b2 * x[k] - a2 * y;
            out[k] = (float) y;
        }
    }
    void reset() { for (int k = 0; k < 4; ++k) z1[k] = z2[k] = 0; }
    void flush() { for (int k = 0; k < 4; ++k) { ftz(z1[k]); ftz(z2[k]); } }
};
struct Split3x4 {
    Biquad4 lp1a, lp1b, ap1, lp2a, lp2b, ap2;
    void set(double fs, double f1, double f2) {
        Split3 s; s.set(fs, f1, f2);
        lp1a.from(s.lp1.a); lp1b.from(s.lp1.b); ap1.from(s.ap1); lp2a.from(s.lp2.a); lp2b.from(s.lp2.b); ap2.from(s.ap2);
    }
    inline void process(const float* x, float* l, float* m, float* h) {
        float t[4], u[4], r[4];
        lp1a.process(x, t); lp1b.process(t, l);
        ap1.process(x, u); for (int k = 0; k < 4; ++k) r[k] = u[k] - l[k];
        lp2a.process(r, t); lp2b.process(t, m);
        ap2.process(r, u); for (int k = 0; k < 4; ++k) h[k] = u[k] - m[k];
    }
    void reset() { lp1a.reset(); lp1b.reset(); ap1.reset(); lp2a.reset(); lp2b.reset(); ap2.reset(); }
    void flush() { lp1a.flush(); lp1b.flush(); ap1.flush(); lp2a.flush(); lp2b.flush(); ap2.flush(); }
};

// Phase-coherent complementary split for the WIDTH / MOTION moves (1.0.2).
// l = (x + A1 x)/2 with A1 a 1st-order all-pass -> l + (x - l) == x EXACTLY and both parts stay within +-45 deg of x
// at their crossover, so a band gain is a real gain. (The LR4 split used before shares the all-pass phase of the
// crossover: at 2.5 kHz the "high" band is -180 deg from the input, so WIDTH 150 % NARROWED 2-3 kHz by -5 dB and
// MOTION panned backwards around the crossover.)
struct AP1 {
    double a = 0, z = 0;
    void set(double fs, double fc) { double t = std::tan(kPi * fc / fs); a = (t - 1) / (t + 1); }
    inline double process(double x) { double y = a * x + z; z = x - a * y; return y; }
    void reset() { z = 0; }
    void flush() { ftz(z); }
};
struct Split3C {
    AP1 a1, a2;
    void set(double fs, double f1, double f2) { a1.set(fs, belowNyquist(fs, f1)); a2.set(fs, belowNyquist(fs, f2)); }
    inline void process(float x, float& l, float& m, float& h) {
        double lo = 0.5 * (x + a1.process(x)), r = x - lo;
        double mi = 0.5 * (r + a2.process(r));
        l = (float) lo; m = (float) mi; h = (float) (r - mi);
    }
    void reset() { a1.reset(); a2.reset(); }
    void flush() { a1.flush(); a2.flush(); }
};

struct DelayLine {
    std::vector<float> buf; int mask = 0, w = 0;
    void init(int maxDelay) {
        int n = 1; while (n < maxDelay + 8) n <<= 1;
        buf.assign((size_t) n, 0.f); mask = n - 1; w = 0;
    }
    void clear() { std::fill(buf.begin(), buf.end(), 0.f); w = 0; }
    inline void push(float x) { buf[(size_t) w] = std::fabs(x) < 1e-25f ? 0.f : x; w = (w + 1) & mask; }
    inline float at(int d) const { return buf[(size_t) ((w - 1 - d) & mask)]; }   // d=0 -> newest
    // 3rd-order Lagrange; exact at integer delays; requires d >= 1
    inline float lagrange(double d) const {
        int i = (int) std::floor(d); double f = d - i;
        if (f < 1e-9) return at(i);
        // points at delays i-1, i, i+1, i+2 ; position relative to (i-1) is t = f + 1
        double t = f + 1.0;
        double y0 = at(i - 1), y1 = at(i), y2 = at(i + 1), y3 = at(i + 2);
        double c0 = -(t - 1) * (t - 2) * (t - 3) / 6.0;
        double c1 = t * (t - 2) * (t - 3) / 2.0;
        double c2 = -t * (t - 1) * (t - 3) / 2.0;
        double c3 = t * (t - 1) * (t - 2) / 6.0;
        return (float) (c0 * y0 + c1 * y1 + c2 * y2 + c3 * y3);
    }
    inline float linear(double d) const {
        int i = (int) std::floor(d); float f = (float) (d - i);
        return at(i) * (1.f - f) + at(i + 1) * f;
    }
    inline float linearAt(int i, float f) const { return at(i) * (1.f - f) + at(i + 1) * f; }   // same arithmetic as linear()
};

// 1.0.3: fractional delay for ENGINE 1 (PHASE). Kaiser windowed sinc, kFirTaps taps centred on the wanted delay D:
// flat magnitude and linear phase to 18 kHz @44.1k / 20 kHz @48k (3rd-order Lagrange lost 3.3 dB @16 kHz at half a sample).
// Coefficients come from a 512-phase table (shared, built once) with linear interpolation between phases: 1.6e-6 max
// coefficient error, < 0.0001 dB vs the exact design. Needs D >= kFirTaps/2 - 1 (= kBaseLatency). Integer delays are read
// directly (bit-exact).
struct FracTable {
    static constexpr int P = 512;
    float h[P + 1][kFirTaps];
    FracTable() {
        auto i0 = [] (double x) { double sum = 1, t = 1; for (int k = 1; k < 64; ++k) { t *= (x / (2.0 * k)) * (x / (2.0 * k)); sum += t; if (t < 1e-17 * sum) break; } return sum; };
        const double norm = i0(kKaiserBeta);
        for (int r = 0; r <= P; ++r) {
            const double f = (double) r / P; double c[kFirTaps], sum = 0;
            for (int k = 0; k < kFirTaps; ++k) {
                const int m = k - (kFirTaps / 2 - 1);                 // tap delay = i + m, m = -15..16
                const double t = m - f, u = t / (kFirTaps / 2);
                const double sinc = std::fabs(t) < 1e-12 ? 1.0 : std::sin(kPi * t) / (kPi * t);
                const double w = std::fabs(u) >= 1.0 ? 0.0 : i0(kKaiserBeta * std::sqrt(1.0 - u * u)) / norm;
                c[k] = sinc * w; sum += c[k];
            }
            for (int k = 0; k < kFirTaps; ++k) h[r][k] = (float) (c[k] / sum);
        }
    }
};
inline const FracTable& fracTable() { static const FracTable t; return t; }   // built on first prepare(), never on the audio thread
struct FracDelay {
    const FracTable* tab = nullptr;
    float h[kFirTaps] = {};
    double cachedD = -1.0;
    void init() { tab = &fracTable(); cachedD = -1.0; }
    void reset() { cachedD = -1.0; }
    void design(double D) {
        const double f = D - std::floor(D), p = f * FracTable::P;
        const int r = std::min(FracTable::P - 1, (int) p); const float a = (float) (p - r);
        const float* h0 = tab->h[r]; const float* h1 = tab->h[r + 1];
        for (int k = 0; k < kFirTaps; ++k) h[k] = h0[k] + a * (h1[k] - h0[k]);
        cachedD = D;
    }
    inline float read(const DelayLine& dl, double D) {
        const int i = (int) std::floor(D);
        if (D - i < 1e-9) return dl.at(i);
        if (std::fabs(D - cachedD) > 1e-7) design(D);
        const int i0 = (int) std::floor(cachedD) - (kFirTaps / 2 - 1);
        float acc = 0.f;
        for (int k = 0; k < kFirTaps; ++k) acc += h[k] * dl.at(i0 + k);
        return acc;
    }
};

inline float dbToLin(float db) { return std::pow(10.f, db / 20.f); }
inline float clamp01(float x) { return std::min(1.f, std::max(0.f, x)); }

// ---------------------------------------------------------------- second-moment statistics
struct MS3 {  // per-band M/S moments for metering
    double mm = 0, ss = 0, ms = 0;
    void decay(double a) { mm *= a; ss *= a; ms *= a; }
    void flush() { ftz(mm); ftz(ss); ftz(ms); }
};
inline float corrFromMS(double mm, double ss, double ms) {
    double el = mm + ss + 2 * ms, er = mm + ss - 2 * ms;
    if (el <= 1e-12 || er <= 1e-12) return 1.f;
    return (float) std::max(-1.0, std::min(1.0, (mm - ss) / std::sqrt(el * er)));
}
inline float balanceFromMS(double mm, double ss, double ms) {
    double t = mm + ss; return t <= 1e-12 ? 0.f : (float) std::max(-1.0, std::min(1.0, 2 * ms / t));
}
inline float spreadFromMS(double mm, double ss) {
    double t = mm + ss; return t <= 1e-12 ? 0.f : (float) (ss / t);
}
inline float monoLossDbFromMS(double mm, double ss) {
    double t = mm + ss; if (t <= 1e-12) return 0.f;
    return (float) std::max(-60.0, 10.0 * std::log10(std::max(mm, 1e-30) / t));
}

// Heuristic integrity score (0..100). Documented, NOT a validated perceptual metric.
inline float integrityScore(float monoLossDb, float corr, float lowCorr, float lowBal) {
    float mono = clamp01(1.f - (-monoLossDb - 1.5f) / 4.5f);
    // correlation counts only as a PROBLEM indicator: normal stereo width (corr >= +0.3) scores full; it falls to 0 at
    // -0.3 (anti-phase). 1.0.1: before, every bit of ordinary width lowered the score, so the default widening of healthy
    // material showed a lower "integrity" than the input.
    float c = clamp01((corr + 0.3f) / 0.6f);
    float center = clamp01(lowCorr) * (1.f - std::min(1.f, std::fabs(lowBal)));
    return 100.f * (0.40f * mono + 0.35f * c + 0.25f * center);
}

// ---------------------------------------------------------------- depth analysis (heuristic)
struct DepthAnalyzer {
    double fs = 48000, fast = 0, slow = 0, early = 1e-9, late = 1e-9, hf = 0, tot = 1e-9;
    double aFast = 0, aSlow = 0, aStat = 0;
    long sinceOnset = 1 << 30; long refractory = 0, earlyLen = 0, lateLen = 0;
    Biquad hp;
    void init(double sr) {
        fs = sr; aFast = std::exp(-1.0 / (0.005 * sr)); aSlow = std::exp(-1.0 / (0.200 * sr));
        aStat = std::exp(-1.0 / (1.5 * sr));
        refractory = (long) (0.100 * sr); earlyLen = (long) (0.050 * sr); lateLen = (long) (0.250 * sr);
        hp.highpass(sr, belowNyquist(sr, 4000), 0.707); reset();
    }
    void reset() { fast = slow = 0; early = late = 1e-9; hf = 0; tot = 1e-9; sinceOnset = 1 << 30; hp.reset(); }
    void flush() { ftz(fast); ftz(slow); ftz(hf); hp.flush(); }
    inline void process(float m) {
        double e = (double) m * m;
        fast = aFast * fast + (1 - aFast) * e;
        slow = aSlow * slow + (1 - aSlow) * e;
        if (sinceOnset > refractory && fast > 4.0 * slow && fast > 1e-8) sinceOnset = 0;
        if (sinceOnset < earlyLen) early = aStat * early + e;
        else if (sinceOnset < lateLen) late = aStat * late + e;
        else { early *= aStat; late *= aStat; }
        if (sinceOnset < (1 << 30)) ++sinceOnset;
        float h = hp.process(m);
        hf = aStat * hf + (double) h * h; tot = aStat * tot + e;
    }
    float earlyLateDb() const { return (float) (10.0 * std::log10((early + 1e-12) / (late + 1e-12))); }
    float hfRatio() const { return (float) (hf / (tot + 1e-12)); }
    // C50-like clarity, clamped to a usable range
    float c50() const { return std::max(-10.f, std::min(40.f, earlyLateDb())); }
    // Absolute (input) depth heuristic: 0 = front, 1 = back. Uses only source-independent cues.
    float depthIndexAbs(float corrFull) const {
        return clamp01(0.6f * clamp01((20.f - c50()) / 20.f) + 0.4f * clamp01(1.f - corrFull));
    }
    // Output depth relative to the input analysis (clarity drop, diffuseness gain, HF damping).
    float depthIndexRel(const DepthAnalyzer& in, float corrIn, float corrOut) const {
        float base = in.depthIndexAbs(corrIn);
        float clarityDrop = clamp01((in.c50() - c50()) / 20.f);
        float diffGain = std::max(-1.f, std::min(1.f, corrIn - corrOut));
        float hfDropDb = (float) (10.0 * std::log10((in.hfRatio() + 1e-9) / (hfRatio() + 1e-9)));
        return clamp01(base + 0.5f * clarityDrop + 0.3f * diffGain + 0.2f * clamp01(hfDropDb / 6.f));
    }
};

// ---------------------------------------------------------------- ENGINE 3: motion trajectory
struct MotionTrajectory {
    double fs = 48000, ph1 = 0, ph2 = 0, ph3 = 0, sm1 = 0, sm2 = 0, p = 0, v = 0, wm = 0;
    double lastV = 0, lastA = 0;  // exposed for tests
    double cDt = -1, cA = 0;
    void init(double sr) { fs = sr; reset(); }
    void reset() { ph1 = ph2 = ph3 = sm1 = sm2 = p = v = wm = lastV = lastA = 0; }
    void flush() { ftz(sm1); ftz(sm2); ftz(p); ftz(v); ftz(wm); }
    static double vmaxFor(float motion, float scale) { return (0.12 + 0.30 * motion) * scale; }  // pan units / s
    static constexpr double amax = 1.5;                                                         // pan units / s^2
    inline void step(float motion, double bpm, float vmaxScale) {
        double dt = 1.0 / fs;
        double beat = 60.0 / std::max(40.0, std::min(240.0, bpm));
        ph1 += dt / (8 * beat); ph2 += dt / (3 * beat); ph3 += dt / (12 * beat);
        // wrap (same result as ph -= floor(ph) for ph >= 0; floor() only when a wrap is due)
        if (ph1 >= 1.0) ph1 -= std::floor(ph1);
        if (ph2 >= 1.0) ph2 -= std::floor(ph2);
        if (ph3 >= 1.0) ph3 -= std::floor(ph3);
        double pmax = 0.35 * motion;
        double target = pmax * (0.7 * std::sin(2 * kPi * ph1) + 0.3 * std::sin(2 * kPi * ph2 + 1.3));
        // musical smoothing: critically damped 2-pole (tau 150 ms)
        if (dt != cDt) { cDt = dt; cA = std::exp(-dt / 0.150); }
        const double a = cA;
        sm1 = a * sm1 + (1 - a) * target; sm2 = a * sm2 + (1 - a) * sm1;
        // density limit: velocity + acceleration clamp
        double vmax = vmaxFor(motion, vmaxScale);
        double vWanted = (sm2 - p) / dt;
        vWanted = std::max(-vmax, std::min(vmax, vWanted));
        // braking: never carry more speed than can be stopped (amax) before the excursion limit
        double room = std::max(0.0, pmax - std::fabs(p) - std::fabs(v) * dt);  // discrete-step margin
        double vBrake = std::sqrt(2.0 * amax * room);
        if (vWanted * p > 0) vWanted = (vWanted > 0 ? 1 : -1) * std::min(std::fabs(vWanted), vBrake);
        double acc = (vWanted - v) / dt;
        acc = std::max(-amax, std::min(amax, acc));
        v += acc * dt;
        v = std::max(-vmax, std::min(vmax, v));
        p += v * dt;
        lastV = v; lastA = acc;
        wm = std::sin(2 * kPi * ph3);
    }
};

// ---------------------------------------------------------------- ENGINE 2: early reflections + late tail
struct DepthReverb {
    static constexpr int kTaps = 8;
    const double tL[kTaps] = {7.1, 11.3, 16.7, 23.9, 31.1, 41.3, 53.9, 67.1};
    const double tR[kTaps] = {8.9, 13.1, 19.3, 21.7, 34.9, 38.3, 57.1, 71.3};
    const float sL[kTaps] = {1, -1, 1, 1, -1, 1, -1, 1};
    const float sR[kTaps] = {-1, 1, 1, -1, 1, -1, 1, 1};
    float gTap[kTaps];
    double fs = 48000;
    DelayLine send;          // mid send
    DelayLine fdn[4]; int fdnLen[4] = {0, 0, 0, 0};
    double fdnLp[4] = {0, 0, 0, 0};
    Biquad sendLp;
    LR4HP sendHp;            // 1.0.1: reflections/tail above 200 Hz only -> the low end stays mono-coherent and centred
    float lastSendFc = -1;
    void init(double sr) {
        fs = sr;
        sendHp.set(sr, belowNyquist(sr, 200.0));
        send.init((int) (0.260 * sr));
        const double lens[4] = {29.7, 37.1, 41.1, 43.7};
        for (int i = 0; i < 4; ++i) { fdnLen[i] = (int) (lens[i] * 0.001 * sr); fdn[i].init(fdnLen[i] + 4); }
        double e = 0;
        for (int i = 0; i < kTaps; ++i) { gTap[i] = (float) std::pow(0.82, i); e += gTap[i] * gTap[i]; }
        for (int i = 0; i < kTaps; ++i) gTap[i] /= (float) std::sqrt(e);
        reset();
    }
    void reset() {
        send.clear(); for (auto& d : fdn) d.clear();
        for (auto& z : fdnLp) z = 0;
        sendLp.reset(); sendHp.reset(); lastSendFc = -1;
        cDepth = -1.f; cRt = -1.f;   // invalidate fs-dependent caches
        cPd = cEr = cCl = -1.f;
    }
    void flush() { for (auto& z : fdnLp) ftz(z); sendLp.flush(); sendHp.flush(); }
    // returns early (eL,eR) and late (lL,lR)
    float cDepth = -1.f, cRt = -1.f; double cDampA = 0, cG[4] = {0, 0, 0, 0};   // exact-input caches (bit-identical results)
    // 1.0.3: tap positions/gains cached on their exact inputs (pre-delay, ER scale, CENTER LOCK): same arithmetic as 1.0.2,
    // recomputed only while those inputs move (was 16 floor() + 40 multiplies per sample, ~25 % of the engine's CPU)
    float cPd = -1.f, cEr = -1.f, cCl = -1.f;
    int tapIL[kTaps] = {}, tapIR[kTaps] = {}; float tapFL[kTaps] = {}, tapFR[kTaps] = {}, tapGL[kTaps] = {}, tapGR[kTaps] = {};
    void updateTaps(float centerLock, float predelayMs, float erScale) {
        cPd = predelayMs; cEr = erScale; cCl = centerLock;
        double pd = predelayMs * 0.001 * fs;
        for (int i = 0; i < kTaps; ++i) {
            double dL = pd + tL[i] * erScale * 0.001 * fs;
            double dRa = pd + tR[i] * erScale * 0.001 * fs;
            double dR = dRa + (dL - dRa) * centerLock;               // CENTER LOCK: mirror pattern -> centred
            float gR = sR[i] + (sL[i] - sR[i]) * centerLock;
            dL = std::max(1.0, dL); dR = std::max(1.0, dR);
            tapIL[i] = (int) std::floor(dL); tapFL[i] = (float) (dL - tapIL[i]);
            tapIR[i] = (int) std::floor(dR); tapFR[i] = (float) (dR - tapIR[i]);
            tapGL[i] = gTap[i] * sL[i]; tapGR[i] = gTap[i] * gR;
        }
    }
    inline void process(float m, float depth, float centerLock, float predelayMs, float erScale, float rt60,
                        float& eL, float& eR, float& lL, float& lR) {
        float fc = 12000.f - 9000.f * depth;
        // 1.0.3: follows DEPTH exactly (was redesigned in 50 Hz steps -> coefficient jumps under automation)
        if (fc != lastSendFc) { sendLp.lowpass(fs, std::min<double>(fc, 0.45 * fs), 0.707); lastSendFc = fc; }
        send.push(sendLp.process(sendHp.process(m)));
        if (predelayMs != cPd || erScale != cEr || centerLock != cCl) updateTaps(centerLock, predelayMs, erScale);
        double accL = 0, accR = 0;
        for (int i = 0; i < kTaps; ++i) {
            accL += tapGL[i] * send.linearAt(tapIL[i], tapFL[i]);
            accR += tapGR[i] * send.linearAt(tapIR[i], tapFR[i]);
        }
        eL = (float) accL; eR = (float) accR;
        // 4-line FDN, Hadamard mixing, HF damping in loop
        float in = 0.5f * (eL + eR);
        double y[4];
        if (depth != cDepth) { cDepth = depth; cDampA = std::exp(-2 * kPi * (8000.0 - 5500.0 * depth) / fs); }
        if (rt60 != cRt) { cRt = rt60; for (int i = 0; i < 4; ++i) cG[i] = std::pow(10.0, -3.0 * (fdnLen[i] / fs) / std::max(0.05f, rt60)); }
        const double dampA = cDampA;
        for (int i = 0; i < 4; ++i) {
            double x = fdn[i].at(fdnLen[i] - 1) * cG[i];
            fdnLp[i] = (1 - dampA) * x + dampA * fdnLp[i];
            y[i] = fdnLp[i];
        }
        double h0 = 0.5 * (y[0] + y[1] + y[2] + y[3]);
        double h1 = 0.5 * (y[0] - y[1] + y[2] - y[3]);
        double h2 = 0.5 * (y[0] + y[1] - y[2] - y[3]);
        double h3 = 0.5 * (y[0] - y[1] - y[2] + y[3]);
        fdn[0].push((float) (h0 + in)); fdn[1].push((float) (h1 + in));
        fdn[2].push((float) (h2 - in)); fdn[3].push((float) (h3 - in));
        double lateNorm = 0.5;
        lL = (float) ((y[0] - y[2]) * lateNorm);
        lR = (float) ((y[1] - y[3]) * lateNorm);
    }
};

// ---------------------------------------------------------------- the engine
class SpatialEngine {
public:
    Meters meters;

    void prepare(double sampleRate, int /*maxBlock*/) {
        fs = sampleRate;
        maxLag = (int) std::ceil(kMaxLagMs * 0.001 * fs);
        dlL.init(maxLag + kBaseLatency + kFirTaps / 2 + 8); dlR.init(maxLag + kBaseLatency + kFirTaps / 2 + 8);
        dryL.init(kBaseLatency + 4); dryR.init(kBaseLatency + 4);
        fdL.init(); fdR.init(); initPeakWindow();
        win = 1; while (win < (int) (0.085 * fs)) win <<= 1;
        hop = win / 2;
        // 1.0.3: analysis rings are written twice (period 2*win, buffer 4*win): the last win samples are always contiguous
        // and stay untouched while the job runs (it ends within applyAt < win samples) -> no snapshot copy (was a 4*win memcpy
        // plus two energy loops inside one block)
        ringHpL.assign((size_t) (4 * win), 0.f); ringHpR.assign((size_t) (4 * win), 0.f);
        ringLoL.assign((size_t) (4 * win), 0.f); ringLoR.assign((size_t) (4 * win), 0.f);
        rmask = 2 * win - 1;
        applyAt = hop / 2;
        {   const double total = (double) kPrepUnits * (win - 2 * maxLag) + (double) (2 * maxLag + 1) * (win - 2 * maxLag) + (kAp + 1) * (double) kCandUnits * win;
            jobBudget = (long) std::ceil(total / std::max(1, applyAt - kChunk)) + 1; }
        frameDecay = std::exp(-(double) kFrame / (kSafeMomSec * fs));
        corrBuf.assign((size_t) (2 * maxLag + 1), 0.0); corrAvg.assign((size_t) (2 * maxLag + 1), 0.0); eyLag.assign((size_t) (2 * maxLag + 1), 0.0);
        avgA = std::exp(-(double) hop / (4.0 * fs));   // tau 4 s
        aMeter = std::exp(-1.0 / (0.300 * fs));
        hpAL.highpass(fs, belowNyquist(fs, 200), 0.707); hpAR.highpass(fs, belowNyquist(fs, 200), 0.707);
        lpAL.lowpass(fs, belowNyquist(fs, 250), 0.707); lpAR.lowpass(fs, belowNyquist(fs, 250), 0.707);
        // moves: complementary split, 400 Hz (1st-order skirt: bass kept mono-safe) and 4 kHz (CENTER LOCK protects the lead below it)
        splitM.set(fs, 400, 4000); splitS.set(fs, 400, 4000);
        an.set(fs, 150, 2500);
        for (int j = 0; j < kAp; ++j) { Biquad q; q.allpass(fs, apFreqs[j], 0.6); apB0[j] = q.b0; apB1[j] = q.b1; apB2[j] = q.b2; apA1[j] = q.a1; apA2[j] = q.a2; }
        rev.init(fs); traj.init(fs);
        daIn.init(fs); daOut.init(fs);
        shelfFs = -1;
        { const double w = 2 * kPi * belowNyquist(fs, 4500) / fs; shelfCos = std::cos(w); shelfSin = std::sin(w); }
        // per-sample smoothing constants (1.0.3: computed once here instead of once per chunk; same expressions)
        aP = 1.f - std::exp(-1.f / (0.030f * (float) fs));
        aDelay = 1.f - std::exp(-1.f / (0.150f * (float) fs));
        aEnv = std::exp(-1.0 / (0.300 * fs));
        aComp = std::exp(-1.0 / (0.400 * fs));
        aCompG = 1.f - std::exp(-1.f / (0.050f * (float) fs));
        gaFast = 1.f - std::exp(-1.f / (0.020f * (float) fs)); gaSlow = 1.f - std::exp(-1.f / (0.300f * (float) fs));
        fadeStep = 1.0 / (0.080 * fs); monStep = 1.0 / (0.020 * fs);
        reset();
    }

    // 1.0.3: the meters (in/out band correlation, balance, spread, depth, integrity) only feed the display. The plugin turns
    // them off while no editor is open (audio output is identical either way). Default on (tests, web page).
    void setMetersActive(bool on) { metersOn.store(on, std::memory_order_relaxed); }
    bool metersActive() const { return metersOn.load(std::memory_order_relaxed); }

    void reset() {
        dlL.clear(); dlR.clear(); dryL.clear(); dryR.clear();
        std::fill(ringHpL.begin(), ringHpL.end(), 0.f); std::fill(ringHpR.begin(), ringHpR.end(), 0.f);
        std::fill(ringLoL.begin(), ringLoL.end(), 0.f); std::fill(ringLoR.begin(), ringLoR.end(), 0.f);
        ringPos = 0; hopCount = 0; filled = 0;
        hpAL.reset(); hpAR.reset(); lpAL.reset(); lpAR.reset();
        splitM.reset(); splitS.reset(); an.reset();
        rev.reset(); traj.reset(); daIn.reset(); daOut.reset();
        apCur.reset(); apNew.reset(); apCurIdx = -1; apNewIdx = -1; apFade = 1.f;
        shL.reset(); shR.reset();
        fdL.reset(); fdR.reset();
        cwI = 0; cWetDep = -1.f; cWetLvl = 0.f;
        lagTarget = 0; delaySm = 0; lastLag = 0; lastConf = 0;
        std::fill(corrAvg.begin(), corrAvg.end(), 0.0); avgPow = 1;
        for (auto& m : mom) m = 0;
        sPrev = 1.f; guardGain = 1.f; wetEnv = dirEnv = 0;
        sStart = sEnd = 1.f; framePos = kFrame - kCtlDelay; for (auto& f : frameAcc) f = 0;   // frame grid on the control clock
        ctlCount = 0;
        jobStage = -1; jobAge = 0; jobAcc = 0;
        lagPos = 0; lagN = 0; incons = 0; apWantPrev = -2; apWantCount = 0;
        monG[0] = 0.f; monG[1] = 1.f; monG[2] = 0.f; bypG = 0.f;
        for (auto& b : bIn) b = MS3{};
        for (auto& b : bOut) b = MS3{};
        fullIn = fullOut = MS3{};
        sm = {}; smInit = false; profInit = false;
        monPrev = -1;
        integHist = 0;
        shelfDb = 99.f;                          // force shelf redesign at the (possibly new) sample rate
        eShIn = eShOut = 0; compG = 1.f;
        cEl = -1.f; cDg = -1.f;                  // exact-input caches
    }

    int latencySamples() const { return kBaseLatency; }

    void flushDenormals() {   // once per chunk; recursive states that can decay towards 0
        hpAL.flush(); hpAR.flush(); lpAL.flush(); lpAR.flush();
        splitM.flush(); splitS.flush(); an.flush();
        apCur.flush(); apNew.flush(); shL.flush(); shR.flush();   // (not cwZ1/cwZ2: the analysis must not depend on chunking)
        rev.flush(); traj.flush(); daIn.flush(); daOut.flush();
        for (auto& b : bIn) b.flush();
        for (auto& b : bOut) b.flush();
        fullIn.flush(); fullOut.flush();
        for (auto& m : mom) ftz(m);
        ftz(delaySm); ftz(wetEnv); ftz(dirEnv); ftz(eShIn); ftz(eShOut);
        ftz(sm.coh); ftz(sm.dep); ftz(sm.el); ftz(sm.wid); ftz(sm.mot); ftz(sm.cl); ftz(sm.dg);
        for (auto& g : monG) ftz(g);
        ftz(bypG);
    }
    double sampleRate() const { return fs; }

    // Test / inspection hooks
    const MotionTrajectory& trajectory() const { return traj; }
    double allowedMonoDeltaDb(float monoSafety) const { return 0.5 + 5.5 * (1.0 - monoSafety); }
    double correlationFloor(float monoSafety) const { return -0.1 + 0.6 * monoSafety; }
    // how much the plugin may lower the correlation of material that is ALREADY below the floor (wide pads, pianos, mixes)
    // healthy-but-wide material (corr ~ +0.4) may lose a little; material near/below anti-phase (corr <= -0.1) may lose nothing
    double correlationDropAllowed(float monoSafety, double c0) const {
        const double health = std::max(0.0, std::min(1.0, (c0 + 0.1) / 0.5));
        return (0.06 + 0.30 * (1.0 - monoSafety)) * health;
    }

    void process(float* L, float* R, int n, const Params& p) {
        int off = 0;
        while (off < n) {
            int k = std::min(kChunk, n - off);
            processChunk(L + off, R + off, k, p);
            flushDenormals();
            off += k;
        }
    }

    // SOLVE SPACE helper: recommended values from current analysis (UI thread may call; reads atomics only)
    struct Solve { float coherence, width, motion; bool changed; };
    Solve solveSpace(const Params& p) const {
        Solve s{p.coherence, p.width, p.motion, false};
        float lag = std::fabs(meters.lagMs.load()), conf = meters.phaseConfidence.load();
        if (lag > 0.02f && conf > 0.4f && p.coherence < 1.f) { s.coherence = 1.f; s.changed = true; }
        float sc = meters.safeScale.load();
        if (sc < 0.98f) {
            if (p.width > 1.f) { s.width = 1.f + (p.width - 1.f) * sc; s.changed = true; }
            if (p.motion > 0.f) { s.motion = p.motion * sc; s.changed = true; }
        }
        return s;
    }

private:
    double fs = 48000; int maxLag = 48;
    DelayLine dlL, dlR, dryL, dryR;
    // phase analysis
    int win = 4096, hop = 2048, ringPos = 0, hopCount = 0, filled = 0;
    std::vector<float> ringHpL, ringHpR, ringLoL, ringLoR;
    const float* tmpA = nullptr; const float* tmpB = nullptr; const float* tmpC = nullptr; const float* tmpD = nullptr;   // job window
    int rmask = 8191, prepT = 0; double prepEx = 0, prepEy = 0, prepE0 = 0;
    // amortised analysis job (snapshot at hop, fixed MAC budget per sample, commit at fixed sample offset)
    int jobStage = -1, jobLag = 0, jobI = 0, jobApIdx = -1, jobAge = 0, applyAt = 1024, jobBest = 0;
    long jobBudget = 1000; double jobAcc = 0, jobNorm = 1, jobBestV = -2, ex0 = 0; bool jobSilent = false;
    std::vector<double> eyLag;   // energy of the shifted R analysis window per lag (1.0.3)
    double apC[8] = {0};
    static constexpr int kCons = 10, kRelease = 6;
    double lagHist[kCons] = {}; int lagPos = 0, lagN = 0, incons = 0, apWantPrev = -2, apWantCount = 0;
    // fail-safe on a fixed 64-sample grid (host block size independent)
    // 1.0.4: second moments over 300 ms (was 100 ms) and a 20 ms fall: on a finished mix (NEON) the width pumped 16.5 times/min
    // below 70 %, now 9.0; drum hits no longer pull WIDTH/MOTION back (RIM mean reduction 10.2 % -> ~0.5 %)
    static constexpr double kSafeMomSec = 0.300, kSafeAttackSec = 0.020;
    static constexpr int kFrame = 64; double frameAcc[10] = {0}; int framePos = 0; float sStart = 1.f, sEnd = 1.f; double frameDecay = 0.99;
    float monG[3] = {0.f, 1.f, 0.f}; float bypG = 0.f;
    std::vector<double> corrBuf, corrAvg; double avgA = 0.98, avgPow = 1;   // corrAvg: long-term correlation (1.0.3)
    Biquad hpAL, hpAR, lpAL, lpAR;
    double lagTarget = 0, delaySm = 0, lastLag = 0, lastConf = 0;
    // all-pass
    static constexpr int kAp = 5;
    // budget weight of one all-pass-candidate sample relative to one float MAC of the lag search (scheduling only: results
    // never depend on it). 1.0.2 counted 4 -> the candidate pass was squeezed into ~2 blocks (worst block 4x the median);
    // 16 matches its real cost (measured sweep 4/8/16/24/32: worst/median 3.1 / 2.2 / 1.7 / 1.6 / 1.7 at 96 kHz)
    static constexpr int kCandUnits = 16;
    static constexpr int kPrepUnits = 8;   // budget weight of one window sample of the energy pass (3 dependent double MACs;
                                           // measured sweep 3/8/12/16: worst/median at 96 kHz/32 2.64 / 2.31 / 2.34 / 2.35)
    const double apFreqs[kAp] = {45, 65, 90, 125, 175};
    Biquad apCur, apNew; int apCurIdx = -1, apNewIdx = -1; float apFade = 1.f;
    // spatial
    Split3C splitM, splitS; Split3x4 an;   // an: metering split, lanes = in M, in S, out M, out S
    MotionTrajectory traj;
    DepthReverb rev;
    Biquad shL, shR; float shelfFs = -1, shelfDb = 99; double shelfCos = 1, shelfSin = 0;
    FracDelay fdL, fdR;
    std::atomic<bool> metersOn { true };
    float cWetDep = -1.f, cWetLvl = 0.f;   // exact-input cache of dep * dbToLin(-24 + 18 dep)
    bool metersWereOn = true;
    static constexpr int kLead = 2;                          // analysis/control look-ahead over the processed audio (1.0.2 value)
    static constexpr int kCtlDelay = kBaseLatency - kLead;   // control clock starts this many samples after the audio clock
    int ctlCount = 0;
    // incremental all-pass correlation (1.0.3: was one 4*win-sample burst inside a single block -> CPU spikes)
    int cwI = 0; double cwSxy[kAp + 1] = {}, cwSyy[kAp + 1] = {}, cwSxx = 0, cwZ1[kAp] = {}, cwZ2[kAp] = {};
    double apB0[kAp] = {}, apB1[kAp] = {}, apB2[kAp] = {}, apA1[kAp] = {}, apA2[kAp] = {};   // candidate all-pass coefficients
    // per-sample constants (prepare)
    float aP = 0, aDelay = 0, aCompG = 0, gaFast = 0, gaSlow = 0; double aEnv = 0, aComp = 0, fadeStep = 0, monStep = 0;
    float guardGain = 1.f; double wetEnv = 0, dirEnv = 0;
    // fail-safe moments: bLbL bRbR bLbR dLdL dRdR dLdR bLdL bRdR bLdR dLbR
    double mom[10] = {0};
    float sPrev = 1.f;
    // meters
    MS3 bIn[3], bOut[3], fullIn, fullOut;
    DepthAnalyzer daIn, daOut;
    struct Smoothed { float coh, dep, el, wid, mot, cl, dg; } sm{};
    bool smInit = false;
    Profile profSm{1.f, 1.f, 1.f, 1.f}; bool profInit = false;
    float cEl = -1.f, cCos = 1.f, cSin = 0.f, cDg = -1.f; double cCap = 1.0;   // exact-input caches
    double eShIn = 0, eShOut = 0; float compG = 1.f;                            // HF-damping loudness compensation
    int monPrev = -1;
    float integHist = 0;

    // scratch
    float bL[kChunk], bR[kChunk], dL[kChunk], dR[kChunk], inL[kChunk], inR[kChunk], ddL[kChunk], ddR[kChunk];

    inline float sAt(float target, float cur, float a) { return cur + a * (target - cur); }

    void startJob() {
        // window = the last win samples, contiguous in the double-written ring
        const size_t s0 = (size_t) ((ringPos - win) & rmask);
        tmpA = ringHpL.data() + s0; tmpB = ringHpR.data() + s0; tmpC = ringLoL.data() + s0; tmpD = ringLoR.data() + s0;
        prepT = 0; prepEx = prepEy = prepE0 = 0;
        jobLag = -maxLag; jobI = maxLag; jobAcc = 0; jobBest = 0; jobBestV = -2; jobApIdx = -1; jobAge = 0;
        jobStage = 3;   // energies first (sliced), then the lag search
    }

    // stage 3: window energies, same sums and order as 1.0.2's startJob (ex, ey over [maxLag, win - maxLag)) plus the energy of
    // the shifted R window for the most negative lag; then the per-lag energies by a sliding sum
    void finishPrep() {
        const double ex = prepEx, ey = prepEy;
        jobSilent = (ex < 1e-7 || ey < 1e-7); ex0 = ex;
        jobNorm = std::sqrt(ex * ey);
        // 1.0.3: energy of the SHIFTED R window for every lag, so that every correlation value is a true normalised correlation
        // (|r| <= 1). 1.0.2 divided all lags by the unshifted energy: a hi-hat hit entering the shifted window gave r = 152 at a
        // bogus lag (measured on a 2-mic hi-hat), which broke the lag history.
        const int end = win - maxLag; double e = prepE0;
        for (int k = -maxLag; k <= maxLag; ++k) {
            eyLag[(size_t) (k + maxLag)] = std::max(0.0, e);
            if (k < maxLag) { const double add = tmpB[(size_t) (end + k)], rem = tmpB[(size_t) (maxLag + k)]; e += add * add - rem * rem; }
        }
        jobStage = jobSilent ? 2 : 0;
    }

    void jobWork(long units) {
        std::vector<double>& r = corrBuf;
        const int end = win - maxLag;
        while (units > 0 && (jobStage == 0 || jobStage == 1 || jobStage == 3)) {
            if (jobStage == 3) {
                const int len = win - 2 * maxLag;
                const int stop = (int) std::min<long>((long) len, (long) prepT + std::max<long>(1L, units / kPrepUnits));
                double ex = prepEx, ey = prepEy, e0 = prepE0;
                for (int t = prepT; t < stop; ++t) {
                    const double a = tmpA[(size_t) (maxLag + t)], b = tmpB[(size_t) (maxLag + t)], c = tmpB[(size_t) t];
                    ex += a * a; ey += b * b; e0 += c * c;
                }
                prepEx = ex; prepEy = ey; prepE0 = e0; units -= (long) kPrepUnits * (stop - prepT); prepT = stop;
                if (prepT >= len) finishPrep();
            } else if (jobStage == 0) {
                // fixed 64-sample blocks, 8 independent float lanes (SIMD-friendly); block sums added in double, in order:
                // the result does not depend on how the host splits the work (buffer-size independent)
                const float* pa = tmpA;
                const float* pb = tmpB + maxLag + jobLag;   // pb[i - maxLag] == tmpB[i + jobLag]
                double acc = jobAcc;
                while (units > 0 && jobI < end) {
                    const int n = std::min(64, end - jobI);
                    float s[8] = {0, 0, 0, 0, 0, 0, 0, 0};
                    const float* x = pa + jobI; const float* y = pb + (jobI - maxLag);
                    int k = 0;
                    for (; k + 8 <= n; k += 8)
                        for (int j = 0; j < 8; ++j) s[j] += x[k + j] * y[k + j];
                    float tail = 0.f;
                    for (; k < n; ++k) tail += x[k] * y[k];
                    acc += (double) (((s[0] + s[1]) + (s[2] + s[3])) + ((s[4] + s[5]) + (s[6] + s[7])) + tail);
                    jobI += n; units -= n;
                }
                jobAcc = acc;
                if (jobI >= end) {
                    const double den = ex0 * eyLag[(size_t) (jobLag + maxLag)];
                    double v = den > 1e-14 ? jobAcc / std::sqrt(den) : 0.0;
                    v = std::max(-1.0, std::min(1.0, v));
                    r[(size_t) (jobLag + maxLag)] = v;
                    if (v > jobBestV) { jobBestV = v; jobBest = jobLag; }
                    jobAcc = 0; jobI = maxLag;
                    if (++jobLag > maxLag) { jobStage = 1; jobApIdx = -1; }
                }
            } else {
                // 1.0.3: the six low-band correlations (no all-pass + 5 all-pass candidates) in ONE pass over the window, in
                // slices that respect the budget (1.0.2 ran each candidate as one 4*win-sample burst -> CPU spikes).
                // Per candidate the arithmetic is exactly 1.0.2's corrWith(): bit-identical results. Same budget (4 units per
                // sample per candidate).
                constexpr int K = kAp + 1;
                if (cwI == 0) { for (int j = 0; j < kAp; ++j) cwZ1[j] = cwZ2[j] = 0; for (int j = 0; j < K; ++j) cwSxy[j] = cwSyy[j] = 0; cwSxx = 0; }
                const int start = win / 8;
                const int stop = (int) std::min<long>((long) win, (long) cwI + std::max<long>(1L, units / ((long) kCandUnits * K)));
                double sxy[K], syy[K], z1[kAp], z2[kAp], sxx = cwSxx;
                for (int j = 0; j < K; ++j) { sxy[j] = cwSxy[j]; syy[j] = cwSyy[j]; }
                for (int j = 0; j < kAp; ++j) { z1[j] = cwZ1[j]; z2[j] = cwZ2[j]; }
                for (int i = cwI; i < stop; ++i) {
                    const float dIn = tmpD[(size_t) i];
                    float y[K]; y[0] = dIn;
                    for (int j = 0; j < kAp; ++j) {
                        const double yy = apB0[j] * dIn + z1[j];
                        z1[j] = apB1[j] * dIn - apA1[j] * yy + z2[j];
                        z2[j] = apB2[j] * dIn - apA2[j] * yy;
                        y[j + 1] = (float) yy;
                    }
                    if (i < start) continue;
                    const double x = tmpC[(size_t) i]; sxx += x * x;
                    for (int j = 0; j < K; ++j) { const double yj = y[j]; sxy[j] += x * yj; syy[j] += yj * yj; }
                }
                for (int j = 0; j < K; ++j) { cwSxy[j] = sxy[j]; cwSyy[j] = syy[j]; }
                for (int j = 0; j < kAp; ++j) { cwZ1[j] = z1[j]; cwZ2[j] = z2[j]; }
                cwSxx = sxx;
                units -= (long) kCandUnits * K * (stop - cwI);
                cwI = stop;
                if (cwI >= win) {
                    for (int j = 0; j < K; ++j) apC[j] = (sxx < 1e-9 || syy[j] < 1e-9) ? 1.0 : sxy[j] / std::sqrt(sxx * syy[j]);
                    cwI = 0; jobApIdx = kAp; jobStage = 2;
                }
            }
        }
    }

    static constexpr int kPkHalf = 8;          // correlation points used on each side of the peak
    static constexpr int kPkWin = 1024;        // Kaiser (beta 5) table over v^2, v = distance / (kPkHalf + 1)
    double pkWin[kPkWin + 2] = {};
    void initPeakWindow() {
        auto i0 = [] (double x) { double s = 1, q = 1; for (int k = 1; k < 40; ++k) { q *= (x / (2.0 * k)) * (x / (2.0 * k)); s += q; if (q < 1e-17 * s) break; } return s; };
        const double beta = 5.0, n0 = i0(beta);
        for (int k = 0; k <= kPkWin + 1; ++k) { const double v2 = std::min(1.0, (double) k / kPkWin); pkWin[k] = i0(beta * std::sqrt(1.0 - v2)) / n0; }
    }
    double corrAt(int best, double t) const {  // band-limited value of the normalised correlation at lag best + t
        const double sp = std::sin(kPi * t), inv = 1.0 / ((kPkHalf + 1.0) * (kPkHalf + 1.0));
        double acc = 0;
        for (int k = -kPkHalf; k <= kPkHalf; ++k) {
            const double u = t - k;
            const double sinc = std::fabs(u) < 1e-12 ? 1.0 : ((k & 1) ? -sp : sp) / (kPi * u);
            const double x = u * u * inv * kPkWin; const int j = (int) x; const double fr = x - j;
            const double w = j >= kPkWin ? 0.0 : pkWin[j] + (pkWin[j + 1] - pkWin[j]) * fr;
            acc += corrBuf[(size_t) (best + k + maxLag)] * sinc * w;
        }
        return acc;
    }
    double sincPeak(int best) const {          // golden-section search of the maximum in [-0.6, 0.6] (precision ~1e-6 sample)
        const double g = 0.6180339887498949;
        double lo = -0.6, hi = 0.6, c = hi - g * (hi - lo), d = lo + g * (hi - lo), fc = corrAt(best, c), fd = corrAt(best, d);
        for (int it = 0; it < 28; ++it) {
            if (fc > fd) { hi = d; d = c; fd = fc; c = hi - g * (hi - lo); fc = corrAt(best, c); }
            else { lo = c; c = d; fc = fd; d = lo + g * (hi - lo); fd = corrAt(best, d); }
        }
        return std::max(-0.5, std::min(0.5, 0.5 * (lo + hi)));
    }

    void commitJob(float coherenceEff) {
        if (jobStage != 2) jobWork(LONG_MAX);          // safety: never reached with the computed budget
        jobStage = -1;
        if (jobSilent) { lastConf *= 0.8; return; }   // silence: hold
        const std::vector<double>& r = corrBuf;
        int best = jobBest; double bestV = jobBestV;
        double r0 = r[(size_t) maxLag];
        double frac = 0;
        if (best > -maxLag && best < maxLag) {
            double ym = r[(size_t) (best - 1 + maxLag)], y0 = r[(size_t) (best + maxLag)], yp = r[(size_t) (best + 1 + maxLag)];
            double den = ym - 2 * y0 + yp;
            if (std::fabs(den) > 1e-12) frac = std::max(-0.5, std::min(0.5, 0.5 * (ym - yp) / den));
        }
        // 1.0.3: sub-sample peak by band-limited (Kaiser-windowed sinc) interpolation of the correlation; the parabola is biased
        // on sharp peaks (measured worst error 0.115 samples -> 0.036 on white noise, 0.086 -> 0.027 on pink noise)
        if (best - kPkHalf >= -maxLag && best + kPkHalf <= maxLag) frac = sincPeak(best);
        double lag = best + frac;
        lastConf = std::max(0.0, bestV);
        // A real mic/time offset is STABLE over time; spurious peaks on wide synths/mixes wander (measured on real stems:
        // true offset spread 0.2 samples, synth pads/mixes 10-22 samples). Apply a lag only if the last kCons estimates
        // agree within 3 samples; persistent disagreement releases the correction to 0.
        // 1.0.3: a correction also needs LONG-TERM evidence. The normalised correlation is averaged over the hops (bias-corrected
        // EMA, tau 4 s) and its peak must agree with this estimate within 3 samples. A real mic/time offset passes at once (from
        // a cold start the average equals the estimate); note-dependent offsets of a spaced-pair recording do not. Measured on the
        // PIANO stem at defaults: 11 brief corrections of up to 0.42 ms in 3 min (audible phasing) -> 0. Release timing unchanged;
        // a NEW offset after other material takes ~3 s instead of ~0.8 s to engage (from a cold start: unchanged).
        avgPow *= avgA;
        const double nrm = 1.0 / (1.0 - avgPow);
        int bestA = 0; double bestVA = -2;
        for (int k = -maxLag; k <= maxLag; ++k) {
            const size_t j = (size_t) (k + maxLag);
            corrAvg[j] = avgA * corrAvg[j] + (1 - avgA) * corrBuf[j];
            const double h = corrAvg[j] * nrm; if (h > bestVA) { bestVA = h; bestA = k; }
        }
        const double r0A = corrAvg[(size_t) maxLag] * nrm;
        const bool longTerm = bestVA > 0.3 && (bestVA - r0A) > 0.05 && std::abs(bestA - best) <= 3;
        const bool cand = bestV > 0.3 && (bestV - r0) > 0.05 && std::abs(best) < maxLag && longTerm;
        const bool aligned = bestV > 0.3 && std::fabs(lag) < 0.5;
        lagHist[lagPos] = cand ? lag : 1e9; lagPos = (lagPos + 1) % kCons; lagN = std::min(lagN + 1, kCons);
        bool consistent = false; double mean = 0;
        if (lagN == kCons) {
            double mn = 1e9, mx = -1e9; bool all = true;
            for (int k = 0; k < kCons; ++k) { if (lagHist[k] > 1e8) { all = false; break; } mn = std::min(mn, lagHist[k]); mx = std::max(mx, lagHist[k]); mean += lagHist[k]; }
            if (all && mx - mn <= 3.0) { consistent = true; mean /= kCons; }
        }
        if (consistent) { lagTarget = mean; incons = 0; }
        else if (aligned) { lagTarget = 0; incons = 0; }
        else if (++incons >= kRelease) lagTarget = 0;
        lastLag = lagTarget;
        // limited all-pass decision on the low band (R channel)
        double c0 = apC[0];
        int bestIdx = -1; double bestC = c0;
        for (int i = 0; i < kAp; ++i) { double cc = apC[i + 1]; if (cc > bestC) { bestC = cc; bestIdx = i; } }
        double curC = apC[apCurIdx + 1];
        meters.lowCorrBefore.store((float) c0);
        int wantIdx = apCurIdx;
        if (coherenceEff < 0.5f) wantIdx = -1;
        else if (bestIdx >= 0 && bestC - c0 >= 0.15 && bestC - curC >= 0.05) wantIdx = bestIdx;
        else if (apCurIdx >= 0 && c0 >= curC) wantIdx = -1;
        if (wantIdx == apWantPrev) ++apWantCount; else { apWantPrev = wantIdx; apWantCount = 1; }   // same decision 3x in a row
        if (wantIdx != apCurIdx && apFade >= 1.f && (apWantCount >= 3 || coherenceEff < 0.5f)) {
            apNewIdx = wantIdx; apNew.reset();
            if (apNewIdx >= 0) apNew.allpass(fs, apFreqs[apNewIdx], 0.6);
            apFade = 0.f;
        }
        meters.lowCorrAfter.store((float) apC[(apFade >= 1.f ? apCurIdx : apNewIdx) + 1]);
    }

    void processChunk(float* L, float* R, int n, const Params& P) {
        const Profile profT = profileFor(P.mode);
        if (!profInit) { profSm = profT; profInit = true; }
        const Profile& prof = profSm;   // smoothed per sample (click-free profile changes)
        const float mix = clamp01(P.mix);
        const float tCoh = clamp01(P.coherence) * mix, tDep = clamp01(P.depth) * mix, tEl = clamp01(P.earlyLate);
        const float tWid = 1.f + (std::max(0.f, std::min(2.f, P.width)) - 1.f) * mix, tMot = clamp01(P.motion) * mix;
        const float tCl = clamp01(P.centerLock), tDg = clamp01(P.depthGuard);
        if (!smInit) { sm = {tCoh, tDep, tEl, tWid, tMot, tCl, tDg}; smInit = true; }
        if (jobStage == 0 || jobStage == 1 || jobStage == 3) jobWork(jobBudget * n);

        for (int i = 0; i < n; ++i) {
            float xl = L[i], xr = R[i];
            if (!std::isfinite(xl)) xl = 0; else if (std::fabs(xl) > kInMax) xl = std::copysign(kInMax, xl);
            if (!std::isfinite(xr)) xr = 0; else if (std::fabs(xr) > kInMax) xr = std::copysign(kInMax, xr);   // 1.0.4: +36 dBFS clamp
            inL[i] = xl; inR[i] = xr;
            dryL.push(xl); dryR.push(xr);
            ddL[i] = dryL.at(kBaseLatency); ddR[i] = dryR.at(kBaseLatency);
            // control clock (1.0.3): analysis, fail-safe frames and motion run kLead samples ahead of the processed audio, exactly
            // as in 1.0.2 -> the longer latency does not move any decision relative to the audio it acts on
            const bool ctl = ctlCount >= kCtlDelay; if (!ctl) ++ctlCount;
            if (ctl) {
                const float ax = dryL.at(kCtlDelay), ay = dryR.at(kCtlDelay);
                const float hl = hpAL.process(ax), hr = hpAR.process(ay), ll = lpAL.process(ax), lr = lpAR.process(ay);
                const size_t q0 = (size_t) ringPos, q1 = q0 + (size_t) (2 * win);
                ringHpL[q0] = hl; ringHpR[q0] = hr; ringLoL[q0] = ll; ringLoR[q0] = lr;
                ringHpL[q1] = hl; ringHpR[q1] = hr; ringLoL[q1] = ll; ringLoR[q1] = lr;
                ringPos = (ringPos + 1) & rmask; if (filled < win) ++filled;
                if (jobStage >= 0 && ++jobAge >= applyAt) commitJob(sm.coh);
                if (++hopCount >= hop) { hopCount = 0; if (filled >= win) startJob(); }
            }

            sm.coh = sAt(tCoh, sm.coh, aP); sm.dep = sAt(tDep, sm.dep, aP); sm.el = sAt(tEl, sm.el, aP);
            sm.wid = sAt(tWid, sm.wid, aP); sm.mot = sAt(tMot, sm.mot, aP); sm.cl = sAt(tCl, sm.cl, aP); sm.dg = sAt(tDg, sm.dg, aP);
            profSm.motionMidScale = sAt(profT.motionMidScale, profSm.motionMidScale, aP); profSm.erTimeScale = sAt(profT.erTimeScale, profSm.erTimeScale, aP);
            profSm.lateScale = sAt(profT.lateScale, profSm.lateScale, aP); profSm.vmaxScale = sAt(profT.vmaxScale, profSm.vmaxScale, aP);

            // ---------------- ENGINE 1: PHASE
            double want = lagTarget * sm.coh;
            want = std::max<double>(-maxLag, std::min<double>(maxLag, want));
            delaySm += aDelay * (want - delaySm);
            if (std::fabs(delaySm) < 1e-6 && want == 0) delaySm = 0;
            dlL.push(xl); dlR.push(xr);
            double dLd = kBaseLatency + std::max(0.0, delaySm);   // R lags -> delay L
            double dRd = kBaseLatency + std::max(0.0, -delaySm);  // L lags -> delay R
            float al = fdL.read(dlL, dLd), ar = fdR.read(dlR, dRd);   // 1.0.3: windowed sinc (integer delays read directly)
            // limited all-pass on R with crossfade on change
            float apOut;
            float cur = apCurIdx >= 0 ? apCur.process(ar) : ar;
            if (apFade < 1.f) {
                float nw = apNewIdx >= 0 ? apNew.process(ar) : ar;
                apOut = cur + (nw - cur) * apFade;
                apFade = (float) std::min(1.0, apFade + fadeStep);
                if (apFade >= 1.f) { apCur = apNew; apCurIdx = apNewIdx; }
            } else apOut = cur;
            bL[i] = al; bR[i] = apOut;

            // ---------------- ENGINE 3: MOTION (M/S, band-weighted)
            float M = 0.5f * (al + apOut), S = 0.5f * (al - apOut);
            float m0, m1, m2, s0, s1, s2;
            splitM.process(M, m0, m1, m2); splitS.process(S, s0, s1, s2);
            if (ctl) traj.step(sm.mot, P.bpm, prof.vmaxScale);
            float pan = (float) traj.p;
            float wmod = 1.f + 0.2f * sm.mot * (float) traj.wm;
            float gLow = std::min(sm.wid, 1.f);                    // sub never widened
            float gUp = sm.wid * wmod;
            float wMid = 0.5f * (1.f - sm.cl) * prof.motionMidScale; // CENTER LOCK protects lead band
            float wHigh = 1.f - 0.6f * sm.cl;
            (void) m0;
            float S2 = S + (gLow - 1.f) * s0 + (gUp - 1.f) * (s1 + s2) + pan * (wMid * m1 + wHigh * m2);
            float l1 = M + S2, r1 = M - S2;

            // ---------------- ENGINE 2: DEPTH
            float dep = sm.dep;
            float shelf = -6.f * dep;
            // 1.0.3: the shelf follows DEPTH every sample while it moves (0.05 dB coefficient steps made HF artefacts at -71 dB
            // under DEPTH/MIX automation); with DEPTH still the coefficients are the same as before
            if (shelf != shelfDb) { shL.highshelfCS(shelfCos, shelfSin, shelf); shR.highshelfCS(shelfCos, shelfSin, shelf); shelfDb = shelf; }
            // level-neutral depth: distance comes from reflections, pre-delay and HF damping, not from turning the source down
            // (a level drop biases every A/B against BYPASS; measured -1.4 dB at default before this change)
            float cl1 = dep > 0.f ? shL.process(l1) : l1;
            float cr1 = dep > 0.f ? shR.process(r1) : r1;
            // loudness compensation of the HF damping (slow, 0.4 s, bounded 0..+6 dB): timbre gets darker, level stays put
            eShIn = aComp * eShIn + (1 - aComp) * ((double) l1 * l1 + (double) r1 * r1);
            eShOut = aComp * eShOut + (1 - aComp) * ((double) cl1 * cl1 + (double) cr1 * cr1);
            if (dep > 0.f && eShOut > 1e-12) compG += aCompG * ((float) std::min(2.0, std::max(1.0, std::sqrt(eShIn / eShOut))) - compG);
            else if (dep <= 0.f) compG += aCompG * (1.f - compG);
            cl1 *= compG; cr1 *= compG;
            if (dep <= 0.f) { shL.process(l1); shR.process(r1); }
            float eL, eR, lL, lR;
            float predelay = 2.f + 20.f * (1.f - dep);
            float rt = (0.3f + 0.9f * dep) * prof.lateScale;
            rev.process(M, dep, sm.cl, predelay, prof.erTimeScale, rt, eL, eR, lL, lR);
            if (sm.el != cEl) { cEl = sm.el; cCos = std::cos(sm.el * (float) kPi * 0.5f); cSin = std::sin(sm.el * (float) kPi * 0.5f); }
            float ge = cCos, gl = cSin * prof.lateScale;
            if (dep != cWetDep) { cWetDep = dep; cWetLvl = dep * dbToLin(-24.f + 18.f * dep); }
            const float wetLvl = cWetLvl;
            float wl = wetLvl * (ge * eL + gl * lL), wr = wetLvl * (ge * eR + gl * lR);
            // DEPTH GUARD: cap wet/direct energy ratio
            wetEnv = aEnv * wetEnv + (1 - aEnv) * 0.5 * ((double) wl * wl + (double) wr * wr);
            dirEnv = aEnv * dirEnv + (1 - aEnv) * 0.5 * ((double) cl1 * cl1 + (double) cr1 * cr1);
            double capDb = -15.0 - 18.0 * sm.dg;   // range matched to measured wet/dir span (-16 dB @ depth 100% .. -43 dB @ 25%)
            if (sm.dg != cDg) { cDg = sm.dg; cCap = std::pow(10.0, capDb / 10.0); }
            const double cap = cCap;
            float gTarget = 1.f;
            if (wetEnv > 1e-12 && dirEnv > 1e-12 && wetEnv / dirEnv > cap) gTarget = (float) std::sqrt(cap * dirEnv / wetEnv);
            float ga = gTarget < guardGain ? gaFast : gaSlow;
            guardGain += ga * (gTarget - guardGain);
            float cL = cl1 + guardGain * wl, cR = cr1 + guardGain * wr;
            dL[i] = cL - al; dR[i] = cR - apOut;
        }

        // ---------------- FAIL-SAFE: mono-loss / correlation predictor on second moments (fixed 64-sample grid)
        const float mStep = (float) monStep;
        if (monPrev < 0) { for (int k = 0; k < 3; ++k) monG[k] = (k == P.monitor) ? 1.f : 0.f; monPrev = P.monitor; }
        const bool mOn = metersOn.load(std::memory_order_relaxed);
        for (int i = 0; i < n; ++i) {
            double a = bL[i], b = bR[i], c = dL[i], d = dR[i];
            frameAcc[0] += a * a; frameAcc[1] += b * b; frameAcc[2] += a * b; frameAcc[3] += c * c; frameAcc[4] += d * d;
            frameAcc[5] += c * d; frameAcc[6] += a * c; frameAcc[7] += b * d; frameAcc[8] += a * d; frameAcc[9] += c * b;
            float s = sStart + (sEnd - sStart) * (float) (framePos + 1) / (float) kFrame;
            sPrev = s;
            if (++framePos >= kFrame) {
                framePos = 0;
                for (int j = 0; j < 10; ++j) { mom[j] = frameDecay * mom[j] + (1 - frameDecay) * (frameAcc[j] / kFrame); frameAcc[j] = 0; }
                sStart = sEnd; sPrev = sStart;
                float sTarget = solveScale(P.monoSafety);
                // 1.0.4: the scale falls over at least kSafeAttackSec instead of within one 64-sample frame (no more steps of
                // the side level on drum hits); it still recovers over 0.4 s
                sEnd = sTarget < sStart ? (float) std::max<double>(sTarget, sStart - kFrame / (kSafeAttackSec * fs))
                                        : (float) std::min<double>(sTarget, sStart + kFrame / (0.4 * fs));
            }
            float oL = bL[i] + s * dL[i], oR = bR[i] + s * dR[i];
            if (mOn) meterSample(inL[i], inR[i], oL, oR);
            // monitor: continuous per-source gains (click-free under any switching rate)
            float srcL[3] = { ddL[i], oL, oL - ddL[i] }, srcR[3] = { ddR[i], oR, oR - ddR[i] };
            float selL = 0, selR = 0;
            for (int k = 0; k < 3; ++k) {
                float t = (k == P.monitor) ? 1.f : 0.f;
                monG[k] += std::max(-mStep, std::min(mStep, t - monG[k]));
                selL += monG[k] * srcL[k]; selR += monG[k] * srcR[k];
            }
            // bypass (latency-compensated), continuous gain
            float tb = P.bypass ? 1.f : 0.f;
            bypG += std::max(-mStep, std::min(mStep, tb - bypG));
            float yl = selL + (ddL[i] - selL) * bypG, yr = selR + (ddR[i] - selR) * bypG;
            ftz(yl); ftz(yr);                                   // never hand subnormals to the host (tails of fast filters)
            L[i] = yl; R[i] = yr;
        }
        publishMeters(n, P);
    }

    // returns largest s in [0,1] that satisfies the MONO SAFETY constraints (bisection)
    float solveScale(float monoSafety) {
        const double* m = mom;
        auto eval = [&](double s, double& ml, double& corr) {
            double ell = m[0] + 2 * s * m[6] + s * s * m[3];
            double err = m[1] + 2 * s * m[7] + s * s * m[4];
            double elr = m[2] + s * (m[8] + m[9]) + s * s * m[5];
            double emono = 0.25 * (ell + err + 2 * elr);
            double est = 0.5 * (ell + err);
            ml = est <= 1e-14 ? 0.0 : std::max(-60.0, 10 * std::log10(std::max(emono, 1e-30) / est));
            corr = (ell <= 1e-14 || err <= 1e-14) ? 1.0 : elr / std::sqrt(ell * err);
        };
        double ml0, c0; eval(0, ml0, c0);
        if (0.5 * (m[0] + m[1]) < 1e-12) return sPrev;              // silence: hold
        double allowed = allowedMonoDeltaDb(monoSafety);
        double floorC = correlationFloor(monoSafety);
        // Correlation target. Material above the floor may be widened down to the floor (+0.06 margin: the predictor uses
        // 300 ms EMA moments (1.0.4; was 100 ms), the promise holds on 300 ms windows). Material already below the floor may only lose a
        // small, MONO SAFETY-dependent amount relative to its own correlation. s = 0 always satisfies both constraints
        // (bug fixed in 1.0.1: the old rule could require the correlation to RISE, blocking every move on wide material).
        // never push healthy stereo below +0.30 (where the integrity score starts to fall); below that, no extra drop
        const double target = std::min(floorC + 0.06, std::max(c0 - correlationDropAllowed(monoSafety, c0), std::min(c0, 0.30 - std::max(0.0, 0.9 - (double) monoSafety)))) - 0.005;
        // mono cost: at most MONO SAFETY's allowance, and never past -1.5 dB of mono loss (where the integrity score starts
        // to fall) unless the input was already there -- then no extra mono loss at all ("non allargare indiscriminatamente")
        // (guards are exact at MONO SAFETY >= 90 % and relax below it, off at 0 %); 0.02 dB / 0.005 = numerical tolerance
        const double mlTarget = std::max(ml0 - allowed, std::min(ml0, -1.5 - 10.0 * std::max(0.0, 0.9 - (double) monoSafety))) - 0.02;
        auto ok = [&](double s) { double ml, c; eval(s, ml, c); return ml >= mlTarget - 1e-9 && c >= target - 1e-9; };
        double ml1, c1; eval(1, ml1, c1);
        meters.predictedMonoDeltaDb.store((float) (ml1 - ml0));
        meters.allowedMonoDeltaDb.store((float) allowed);
        if (ok(1.0)) return 1.f;
        double lo = 0, hi = 1;
        for (int it = 0; it < 14; ++it) { double mid = 0.5 * (lo + hi); if (ok(mid)) lo = mid; else hi = mid; }
        return (float) lo;
    }

    void meterSample(float il, float ir, float ol, float orr) {
        float mi = 0.5f * (il + ir), si = 0.5f * (il - ir), mo = 0.5f * (ol + orr), so = 0.5f * (ol - orr);
        const float x4[4] = { mi, si, mo, so };
        float l4[4], m4[4], h4[4];
        an.process(x4, l4, m4, h4);
        const float im[3] = { l4[0], m4[0], h4[0] }, is[3] = { l4[1], m4[1], h4[1] };
        const float om[3] = { l4[2], m4[2], h4[2] }, os[3] = { l4[3], m4[3], h4[3] };
        const double a = aMeter;
        for (int b = 0; b < 3; ++b) {
            bIn[b].mm = a * bIn[b].mm + (1 - a) * im[b] * im[b]; bIn[b].ss = a * bIn[b].ss + (1 - a) * is[b] * is[b];
            bIn[b].ms = a * bIn[b].ms + (1 - a) * im[b] * is[b];
            bOut[b].mm = a * bOut[b].mm + (1 - a) * om[b] * om[b]; bOut[b].ss = a * bOut[b].ss + (1 - a) * os[b] * os[b];
            bOut[b].ms = a * bOut[b].ms + (1 - a) * om[b] * os[b];
        }
        fullIn.mm = a * fullIn.mm + (1 - a) * mi * mi; fullIn.ss = a * fullIn.ss + (1 - a) * si * si; fullIn.ms = a * fullIn.ms + (1 - a) * mi * si;
        fullOut.mm = a * fullOut.mm + (1 - a) * mo * mo; fullOut.ss = a * fullOut.ss + (1 - a) * so * so; fullOut.ms = a * fullOut.ms + (1 - a) * mo * so;
        daIn.process(mi); daOut.process(mo);
    }
    double aMeter = 0.9999;

    void resetMeterStates() {
        an.reset(); daIn.reset(); daOut.reset();
        for (auto& b : bIn) b = MS3{};
        for (auto& b : bOut) b = MS3{};
        fullIn = fullOut = MS3{};
    }

    void publishMeters(int n, const Params& P) {
        (void) n;
        // always: phase / fail-safe / guard / motion state (cheap, used by SOLVE SPACE and the status line)
        meters.lagMs.store((float) (lastLag / fs * 1000.0));
        meters.appliedDelayMs.store((float) (delaySm / fs * 1000.0));
        meters.phaseConfidence.store((float) lastConf);
        meters.allpassIdx.store(apCurIdx);
        meters.wetRatioDb.store(wetEnv > 1e-14 && dirEnv > 1e-14 ? (float) (10 * std::log10(guardGain * guardGain * wetEnv / dirEnv)) : -120.f);
        meters.guardGainDb.store(20.f * std::log10(std::max(1e-6f, guardGain)));
        meters.safeScale.store(sPrev);
        meters.reduction.store(1.f - sPrev);
        meters.pan.store((float) traj.p); meters.widthMod.store((float) traj.wm);
        const bool mOn = metersOn.load(std::memory_order_relaxed);
        if (mOn && !metersWereOn) resetMeterStates();   // fresh start after the view was closed (stale state otherwise)
        metersWereOn = mOn;
        if (!mOn) return;
        float ciF = corrFromMS(fullIn.mm, fullIn.ss, fullIn.ms), coF = corrFromMS(fullOut.mm, fullOut.ss, fullOut.ms);
        float mlI = monoLossDbFromMS(fullIn.mm, fullIn.ss), mlO = monoLossDbFromMS(fullOut.mm, fullOut.ss);
        meters.corrIn.store(ciF); meters.corrOut.store(coF);
        meters.monoLossIn.store(mlI); meters.monoLossOut.store(mlO);
        for (int b = 0; b < 3; ++b) {
            meters.bandIn[b].corr.store(corrFromMS(bIn[b].mm, bIn[b].ss, bIn[b].ms));
            meters.bandIn[b].balance.store(balanceFromMS(bIn[b].mm, bIn[b].ss, bIn[b].ms));
            meters.bandIn[b].spread.store(spreadFromMS(bIn[b].mm, bIn[b].ss));
            meters.bandOut[b].corr.store(corrFromMS(bOut[b].mm, bOut[b].ss, bOut[b].ms));
            meters.bandOut[b].balance.store(balanceFromMS(bOut[b].mm, bOut[b].ss, bOut[b].ms));
            meters.bandOut[b].spread.store(spreadFromMS(bOut[b].mm, bOut[b].ss));
        }
        meters.integrityIn.store(integrityScore(mlI, ciF, meters.bandIn[0].corr.load(), meters.bandIn[0].balance.load()));
        meters.integrityOut.store(integrityScore(mlO, coF, meters.bandOut[0].corr.load(), meters.bandOut[0].balance.load()));
        meters.depthIn.store(daIn.depthIndexAbs(ciF)); meters.depthOut.store(daOut.depthIndexRel(daIn, ciF, coF));
        meters.earlyLateDbIn.store(daIn.earlyLateDb()); meters.earlyLateDbOut.store(daOut.earlyLateDb());
        meters.hfIn.store(daIn.hfRatio()); meters.hfOut.store(daOut.hfRatio());
        double lvl = 0.5 * (fullIn.mm + fullIn.ss) * 2.0;
        meters.levelDb.store(lvl > 1e-12 ? (float) (10 * std::log10(lvl)) : -120.f);
        float levelConf = clamp01((meters.levelDb.load() + 60.f) / 30.f);
        meters.confidence.store(levelConf * (0.5f + 0.5f * (float) std::min(1.0, lastConf / 0.6)));
        (void) P;
    }
};

} // namespace tsr
