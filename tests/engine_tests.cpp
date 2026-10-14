// TSR PHASE · DEPTH · MOTION — engine test suite (no framework, prints measured values)
#include "../Source/dsp/SpatialEngine.h"
#include "ref/SpatialEngine_v102.h"   // 1.0.2 engine (namespace tsr_v102), reference for T37
#include <complex>
#include <chrono>
#include <cstdio>
#include <functional>
#include <string>
#include <random>

using namespace tsr;
static const int LAT = kBaseLatency;   // reported latency (15 samples since 1.0.3)
static int gFail = 0, gPass = 0;
static void check(bool ok, const char* name, const std::string& detail) {
    std::printf("[%s] %-58s %s\n", ok ? "PASS" : "FAIL", name, detail.c_str());
    (ok ? gPass : gFail)++;
}
static std::string fmt(const char* f, double a = 0, double b = 0, double c = 0, double d = 0) {
    char buf[256]; std::snprintf(buf, sizeof buf, f, a, b, c, d); return buf;
}

struct Rng { uint64_t s = 0x9E3779B97F4A7C15ull; double u() { s ^= s << 13; s ^= s >> 7; s ^= s << 17; return (double) (s >> 11) / 9007199254740992.0; }
             double g() { double a = u() + 1e-12, b = u(); return std::sqrt(-2 * std::log(a)) * std::cos(2 * kPi * b); } };

using Buf = std::vector<float>;
static Buf noise(int n, uint64_t seed, float amp = 0.1f) { Rng r; r.s ^= seed * 0x1234567ull; Buf x((size_t) n); for (auto& v : x) v = (float) (amp * r.g()); return x; }
static Buf lowpassed(const Buf& x, double fs, double fc) { Biquad a, b; a.lowpass(fs, fc, 0.707); b.lowpass(fs, fc, 0.707); Buf y(x.size()); for (size_t i = 0; i < x.size(); ++i) y[i] = b.process(a.process(x[i])); return y; }
static Buf sincDelay(const Buf& x, double D) {  // windowed-sinc fractional delay (independent of engine's Lagrange)
    Buf y(x.size(), 0.f); const int H = 48;
    for (size_t n = 0; n < x.size(); ++n) {
        double acc = 0; int c = (int) std::floor(D);
        for (int k = c - H; k <= c + H; ++k) {
            long idx = (long) n - k; if (idx < 0 || idx >= (long) x.size()) continue;
            double t = k - D; double s = std::fabs(t) < 1e-12 ? 1.0 : std::sin(kPi * t) / (kPi * t);
            double w = 0.5 * (1 + std::cos(kPi * t / (H + 1)));
            acc += x[(size_t) idx] * s * w;
        }
        y[n] = (float) acc;
    }
    return y;
}
static void run(SpatialEngine& e, Buf& L, Buf& R, const Params& p, int block = 512, std::function<void(int)> perBlock = nullptr) {
    for (size_t off = 0; off < L.size(); off += (size_t) block) {
        int n = (int) std::min<size_t>((size_t) block, L.size() - off);
        e.process(L.data() + off, R.data() + off, n, p);
        if (perBlock) perBlock((int) off);
    }
}
struct St { double ll = 0, rr = 0, lr = 0; };
static St stats(const Buf& L, const Buf& R, size_t a, size_t b) { St s; for (size_t i = a; i < b; ++i) { s.ll += (double) L[i] * L[i]; s.rr += (double) R[i] * R[i]; s.lr += (double) L[i] * R[i]; } return s; }
static double corr(const St& s) { return s.lr / std::sqrt(std::max(1e-30, s.ll * s.rr)); }
static double monoLoss(const St& s) { double em = 0.25 * (s.ll + s.rr + 2 * s.lr), es = 0.5 * (s.ll + s.rr); return 10 * std::log10(std::max(1e-30, em) / es); }
static Params neutral() { Params p; p.coherence = 0; p.depth = 0; p.earlyLate = 0.42f; p.width = 1; p.motion = 0; p.mix = 1; p.monoSafety = 0.94f; p.centerLock = 0.89f; p.depthGuard = 0.83f; p.monitor = MON_TSR; return p; }
static double maxErrDelayed(const Buf& out, const Buf& in, int lat) { double m = 0; for (size_t i = (size_t) lat; i < out.size(); ++i) m = std::max(m, (double) std::fabs(out[i] - in[i - (size_t) lat])); return m; }

int main() {
    const double fs = 48000; const int N = (int) (fs * 3);

    // ---------------------------------------------------------------- transparency
    {
        SpatialEngine e; e.prepare(fs, 512);
        Buf L0 = noise(N, 1), R0 = noise(N, 2), L = L0, R = R0;
        run(e, L, R, neutral());
        double err = std::max(maxErrDelayed(L, L0, LAT), maxErrDelayed(R, R0, LAT));
        check(err < 1e-6, "T01 neutral params = identity (latency = reported)", fmt("max|err|=%.3g", err));
    }
    {
        SpatialEngine e; e.prepare(fs, 512);
        Params p; p.mix = 0; p.coherence = 1; p.depth = 1; p.width = 2; p.motion = 1;  // everything cranked, MIX 0
        Buf L0 = noise(N, 3), R0 = sincDelay(L0, 13.3), L = L0, R = R0;
        run(e, L, R, p);
        double err = std::max(maxErrDelayed(L, L0, LAT), maxErrDelayed(R, R0, LAT));
        check(err < 1e-6, "T02 MIX 0% = identity even with all moves at max", fmt("max|err|=%.3g", err));
    }
    {
        SpatialEngine e; e.prepare(fs, 512);
        Buf imp((size_t) 4800, 0.f), z((size_t) 4800, 0.f); imp[100] = 1.f; Buf L = imp, R = z;
        run(e, L, R, neutral());
        int at = -1; for (size_t i = 0; i < L.size(); ++i) if (std::fabs(L[i]) > 0.5f) { at = (int) i; break; }
        check(at == 100 + LAT && e.latencySamples() == 15, "T03 reported latency matches measured impulse delay", fmt("impulse@%.0f expected %.0f, reported=%.0f", at, 100 + LAT, e.latencySamples()));
    }
    // ---------------------------------------------------------------- ENGINE 1: PHASE
    for (double srT : {44100.0, 48000.0, 96000.0}) {
        SpatialEngine e; e.prepare(srT, 512);
        int n = (int) (srT * 4);
        double Dms = 0.37, D = Dms * 0.001 * srT;
        Buf L0 = lowpassed(noise(n, 5), srT, 0.40 * srT), R0 = sincDelay(L0, D), L = L0, R = R0;
        Params p = neutral(); p.coherence = 1;
        run(e, L, R, p);
        double est = e.meters.lagMs.load();
        St si = stats(L0, R0, (size_t) (n - srT), (size_t) n), so = stats(L, R, (size_t) (n - srT), (size_t) n);
        char nm[96]; std::snprintf(nm, sizeof nm, "T04 sub-sample lag %.2f ms detected+aligned @%.1fk", Dms, srT / 1000);
        check(std::fabs(est - Dms) < 0.01 && corr(so) > 0.95 && monoLoss(so) > -0.3,
              nm, fmt("est=%.4f ms corr %.3f->%.3f monoLoss %.2f->", est, corr(si), corr(so)) + fmt("%.2f dB", monoLoss(so)));
    }
    {
        SpatialEngine e; e.prepare(fs, 512);
        Buf L0 = lowpassed(noise(N, 6), fs, 18000), R0 = sincDelay(L0, 20.4), L = L0, R = R0;
        Params p = neutral(); p.coherence = 0;
        run(e, L, R, p);
        double err = std::max(maxErrDelayed(L, L0, LAT), maxErrDelayed(R, R0, LAT));
        check(err < 1e-6 && std::fabs(e.meters.appliedDelayMs.load()) < 1e-9, "T05 COHERENCE 0 -> no phase correction applied",
              fmt("detected=%.3f ms applied=%.3g ms err=%.2g", e.meters.lagMs.load(), e.meters.appliedDelayMs.load(), err));
    }
    {
        SpatialEngine e; e.prepare(fs, 512);
        Buf L0 = lowpassed(noise(N, 7), fs, 18000), R0 = sincDelay(L0, -9.6 < 0 ? 0 : 0), L, R;
        // L lags R by 9.6 samples (0.2 ms)
        R0 = L0; L0 = sincDelay(R0, 9.6); L = L0; R = R0;
        Params p = neutral(); p.coherence = 1;
        run(e, L, R, p);
        St so = stats(L, R, (size_t) (N - fs), (size_t) N);
        check(std::fabs(e.meters.lagMs.load() + 0.2) < 0.01 && corr(so) > 0.95, "T06 negative lag (L late) -> R delayed",
              fmt("est=%.4f ms corr_out=%.3f", e.meters.lagMs.load(), corr(so)));
    }
    {
        SpatialEngine e; e.prepare(fs, 512);
        Params p = neutral(); p.coherence = 1;
        Buf L0 = noise(N, 8), R0 = noise(N, 9), L = L0, R = R0;   // uncorrelated: nothing to align
        run(e, L, R, p);
        check(std::fabs(e.meters.appliedDelayMs.load()) < 0.005, "T07 uncorrelated input -> no false alignment",
              fmt("applied=%.4f ms conf=%.3f", e.meters.appliedDelayMs.load(), e.meters.phaseConfidence.load()));
    }
    {
        SpatialEngine e; e.prepare(fs, 512);
        Buf src = noise(N, 10), low = lowpassed(src, fs, 200), R0 = low, L0((size_t) N);
        Biquad ap; ap.allpass(fs, 90, 0.6); for (int i = 0; i < N; ++i) L0[(size_t) i] = ap.process(low[(size_t) i]);
        Buf L = L0, R = R0; Params p = neutral(); p.coherence = 1;
        Biquad lpA, lpB; lpA.lowpass(fs, 250, 0.707); lpB.lowpass(fs, 250, 0.707);
        run(e, L, R, p);
        St si = stats(L0, R0, (size_t) (N - fs), (size_t) N), so = stats(L, R, (size_t) (N - fs), (size_t) N);
        check(e.meters.allpassIdx.load() >= 0 && corr(so) > corr(si) + 0.15, "T08 limited all-pass fixes low-band phase rotation",
              fmt("ap idx=%.0f corr_low %.3f->%.3f", e.meters.allpassIdx.load(), corr(si), corr(so)));
    }
    // ---------------------------------------------------------------- MOTION / WIDTH / FAIL-SAFE
    auto stereoMix = [&](uint64_t seed, float sAmp) { Buf m = noise(N, seed), s = noise(N, seed + 100, 0.1f * sAmp), L((size_t) N), R((size_t) N);
        for (int i = 0; i < N; ++i) { L[(size_t) i] = m[(size_t) i] + s[(size_t) i]; R[(size_t) i] = m[(size_t) i] - s[(size_t) i]; } return std::make_pair(L, R); };
    {
        auto [L0, R0] = stereoMix(11, 0.5f);
        for (float ms : {0.94f, 0.5f, 0.0f}) {
            SpatialEngine e; e.prepare(fs, 512);
            Buf L = L0, R = R0; Params p = neutral(); p.width = 2; p.monoSafety = ms;
            double worstDml = 0, worstC = 1;
            run(e, L, R, p);
            for (size_t a = (size_t) (0.5 * fs); a + 4800 <= (size_t) N; a += 4800) {
                St si = stats(L0, R0, a - LAT, a + 4800 - LAT), so = stats(L, R, a, a + 4800);
                worstDml = std::min(worstDml, monoLoss(so) - monoLoss(si)); worstC = std::min(worstC, corr(so));
            }
            double allowed = e.allowedMonoDeltaDb(ms), floorC = e.correlationFloor(ms);
            char nm[96]; std::snprintf(nm, sizeof nm, "T09 WIDTH 200%% respects MONO SAFETY %.0f%% (100ms windows)", ms * 100);
            check(worstDml >= -allowed && worstC >= std::min(floorC, 0.6), nm,
                  fmt("worst dMono=%.2f dB (lim -%.2f) worst corr=%.3f (floor %.3f)", worstDml, allowed, worstC, floorC) + fmt(" red=%.0f%%", e.meters.reduction.load() * 100));
        }
    }
    {
        auto [L0, R0] = stereoMix(12, 0.5f);
        SpatialEngine e; e.prepare(fs, 512); Buf L = L0, R = R0; Params p = neutral(); p.width = 2; p.monoSafety = 1.0f;
        run(e, L, R, p);
        float red = e.meters.reduction.load();
        SpatialEngine e2; e2.prepare(fs, 512); Buf L2 = L0, R2 = R0; p.monoSafety = 0.f; run(e2, L2, R2, p);
        check(red > 0.3f && e2.meters.reduction.load() < 0.01f, "T10 fail-safe declares reduction (MS 100%) / none (MS 0%)",
              fmt("reduction MS100=%.1f%% MS0=%.1f%% predicted dMono(s=1)=%.2f dB", red * 100, e2.meters.reduction.load() * 100, e.meters.predictedMonoDeltaDb.load()));
    }
    {
        auto [L0, R0] = stereoMix(13, 0.7f);
        SpatialEngine e; e.prepare(fs, 512); Buf L = L0, R = R0; Params p = neutral(); p.width = 2; p.motion = 1; p.centerLock = 0; p.monoSafety = 0;
        run(e, L, R, p);
        double m = 0; for (int i = LAT; i < N; ++i) m = std::max(m, (double) std::fabs((L[(size_t) i] + R[(size_t) i]) - (L0[(size_t) (i - LAT)] + R0[(size_t) (i - LAT)])));
        check(m < 1e-5, "T11 width+motion never change the mono sum (L+R)", fmt("max|dMono|=%.3g", m));
    }
    {
        SpatialEngine e; e.prepare(fs, 512);
        Buf L0((size_t) N), R0; for (int i = 0; i < N; ++i) L0[(size_t) i] = 0.3f * (float) std::sin(2 * kPi * 50 * i / fs); R0 = L0;
        Buf L = L0, R = R0; Params p = neutral(); p.width = 2; p.motion = 1; p.centerLock = 0; p.monoSafety = 0;
        run(e, L, R, p);
        double worst = 0;
        for (size_t a = (size_t) fs; a + 960 <= (size_t) N; a += 960) { St s = stats(L, R, a, a + 960); worst = std::max(worst, std::fabs(10 * std::log10(s.ll / s.rr))); }
        check(worst < 0.1, "T12 sub (50 Hz) stays centred under max motion+width", fmt("worst L/R level diff=%.4f dB", worst));
    }
    for (float cl : {1.0f, 0.0f}) {
        SpatialEngine e; e.prepare(fs, 512);
        int n = (int) (fs * 10);
        Buf L0((size_t) n), R0; for (int i = 0; i < n; ++i) L0[(size_t) i] = 0.3f * (float) std::sin(2 * kPi * 1000 * i / fs); R0 = L0;
        Buf L = L0, R = R0; Params p = neutral(); p.motion = 1; p.centerLock = cl; p.monoSafety = 0;
        run(e, L, R, p);
        double worst = 0;
        for (size_t a = (size_t) fs; a + 960 <= (size_t) n; a += 960) { St s = stats(L, R, a, a + 960); worst = std::max(worst, std::fabs(10 * std::log10(s.ll / s.rr))); }
        char nm[96]; std::snprintf(nm, sizeof nm, "T13 CENTER LOCK %.0f%%: 1 kHz lead %s", cl * 100, cl > 0.5f ? "stays centred" : "moves");
        check(cl > 0.5f ? worst < 0.25 : worst > 1.0, nm, fmt("worst L/R diff=%.3f dB", worst));
    }
    {
        SpatialEngine e; e.prepare(fs, 1);
        Params p = neutral(); p.motion = 1; p.centerLock = 0;
        int n = (int) (fs * 20); double prevP = 0, prevV = 0, maxV = 0, maxA = 0, maxP = 0;
        float l = 0, r = 0;
        for (int i = 0; i < n; ++i) {
            l = r = 0.f; e.process(&l, &r, 1, p);
            double pp = e.trajectory().p, v = (pp - prevP) * fs, a = (v - prevV) * fs;
            if (i > 1) { maxV = std::max(maxV, std::fabs(v)); maxA = std::max(maxA, std::fabs(a)); }
            maxP = std::max(maxP, std::fabs(pp)); prevP = pp; prevV = v;
        }
        double vmax = MotionTrajectory::vmaxFor(1.f, 1.f);
        check(maxV <= vmax * 1.0001 && maxA <= MotionTrajectory::amax * 1.01 && maxP <= 0.35 + 1e-6 && maxP > 0.1,
              "T14 motion density limit (velocity, acceleration, excursion)",
              fmt("max|v|=%.6f (lim %.6f) max|a|=%.5f (lim %.2f)", maxV, vmax, maxA, MotionTrajectory::amax) + fmt(" max|p|=%.7f", maxP));
    }
    {
        SpatialEngine e1, e2; e1.prepare(fs, 1); e2.prepare(fs, 1);
        Params p = neutral(); p.motion = 1; Params pd = p; pd.mode = MODE_DRUMS;
        double m1 = 0, m2 = 0, prev1 = 0, prev2 = 0;
        for (int i = 0; i < (int) (fs * 10); ++i) { float l = 0, r = 0; e1.process(&l, &r, 1, p); l = r = 0; e2.process(&l, &r, 1, pd);
            m1 = std::max(m1, std::fabs(e1.trajectory().p - prev1) * fs); m2 = std::max(m2, std::fabs(e2.trajectory().p - prev2) * fs);
            prev1 = e1.trajectory().p; prev2 = e2.trajectory().p; }
        check(m2 < m1 * 0.6, "T15 DRUMS profile halves motion speed (anti-pumping)", fmt("max|v| music=%.4f drums=%.4f", m1, m2));
    }
    // ---------------------------------------------------------------- ENGINE 2: DEPTH
    auto bursts = [&](int n) { Buf x((size_t) n, 0.f); Rng r; for (int i = 0; i < n; ++i) { int ph = i % (int) (0.4 * fs); if (ph < (int) (0.008 * fs)) x[(size_t) i] = (float) (0.3 * r.g() * std::exp(-ph / (0.002 * fs))); } return x; };
    {
        int n = (int) (fs * 6);
        Buf L0 = bursts(n), R0 = L0;
        SpatialEngine a, b; a.prepare(fs, 512); b.prepare(fs, 512);
        Buf La = L0, Ra = R0, Lb = L0, Rb = R0; Params pa = neutral(), pb = neutral(); pb.depth = 1; pb.depthGuard = 0; pb.centerLock = 0; pb.monoSafety = 0;
        run(a, La, Ra, pa); run(b, Lb, Rb, pb);
        double d0 = a.meters.depthOut.load(), d1 = b.meters.depthOut.load();
        double el0 = a.meters.earlyLateDbOut.load(), el1 = b.meters.earlyLateDbOut.load();
        double hf0 = a.meters.hfOut.load(), hf1 = b.meters.hfOut.load();
        check(d1 > d0 + 0.1 && el1 < el0 - 3 && hf1 < hf0, "T16 DEPTH 100% moves source back (depth idx, early/late, HF)",
              fmt("depthIdx %.2f->%.2f  early/late %.1f->%.1f dB", d0, d1, el0, el1) + fmt(" HF %.3f->%.3f", hf0, hf1));
    }
    {
        int n = (int) (fs * 6);
        Buf src = noise(n, 20, 0.1f);
        for (float g : {0.0f, 0.5f, 1.0f}) {
            SpatialEngine e; e.prepare(fs, 512); Buf L = src, R = src; Params p = neutral(); p.depth = 1; p.earlyLate = 1; p.depthGuard = g; p.monoSafety = 0;
            run(e, L, R, p);
            double capDb = -15.0 - 18.0 * g, got = e.meters.wetRatioDb.load(), gg = e.meters.guardGainDb.load();
            char nm[96]; std::snprintf(nm, sizeof nm, "T17 DEPTH GUARD %.0f%% caps wet/direct energy", g * 100);
            // must cap AND, for g > 0.3, actually engage (control not dead over its range)
            check(got <= capDb + 0.5 && (g < 0.3f || gg < -1.0), nm, fmt("wet/dir=%.2f dB cap=%.2f dB guard gain=%.2f dB", got, capDb, e.meters.guardGainDb.load()));
        }
    }
    {
        int n = (int) (fs * 4);
        Buf src = bursts(n);
        SpatialEngine e; e.prepare(fs, 512); Buf L = src, R = src; Params p = neutral(); p.depth = 1; p.centerLock = 1; p.monoSafety = 0; p.depthGuard = 0; p.earlyLate = 0;
        run(e, L, R, p);
        St s = stats(L, R, (size_t) fs, (size_t) n);
        SpatialEngine e2; e2.prepare(fs, 512); Buf L2 = src, R2 = src; p.earlyLate = 1; run(e2, L2, R2, p);
        St s2 = stats(L2, R2, (size_t) fs, (size_t) n); double bal = (s2.ll - s2.rr) / (s2.ll + s2.rr);
        check(corr(s) > 0.999 && std::fabs(bal) < 0.02, "T18 CENTER LOCK 100%: early refl. mirrored, late tail balanced",
              fmt("corr(early only)=%.5f  balance(late)=%.4f", corr(s), bal));
    }
    {
        auto [L0, R0] = stereoMix(21, 0.3f);
        SpatialEngine e; e.prepare(fs, 512); Buf L = L0, R = R0; Params p = neutral(); p.depth = 1; p.centerLock = 0; p.depthGuard = 0; p.earlyLate = 0; p.monoSafety = 1;
        double worst = 0; run(e, L, R, p);
        for (size_t a = (size_t) (0.5 * fs); a + 4800 <= (size_t) N; a += 4800) { St si = stats(L0, R0, a - LAT, a + 4800 - LAT), so = stats(L, R, a, a + 4800); worst = std::min(worst, monoLoss(so) - monoLoss(si)); }
        check(worst >= -e.allowedMonoDeltaDb(1) - 0.15, "T19 fail-safe also bounds DEPTH reflections in mono", fmt("worst dMono=%.2f dB (lim -%.2f)", worst, e.allowedMonoDeltaDb(1)));
    }
    // ---------------------------------------------------------------- monitor / bypass / robustness
    {
        auto [L0, R0] = stereoMix(30, 0.5f);
        Params p; p.monoSafety = 0.5f;
        SpatialEngine a, b; a.prepare(fs, 512); b.prepare(fs, 512);
        Buf La = L0, Ra = R0, Lb = L0, Rb = R0; Params pd = p; pd.monitor = MON_DELTA;
        run(a, La, Ra, p); run(b, Lb, Rb, pd);
        double m = 0; for (int i = LAT; i < N; ++i) m = std::max(m, (double) std::fabs(Lb[(size_t) i] + L0[(size_t) (i - LAT)] - La[(size_t) i]));
        SpatialEngine c; c.prepare(fs, 512); Buf Lc = L0, Rc = R0; Params po = p; po.monitor = MON_ORIGINAL; run(c, Lc, Rc, po);
        double mo = std::max(maxErrDelayed(Lc, L0, LAT), maxErrDelayed(Rc, R0, LAT));
        check(m < 1e-5 && mo < 1e-7, "T20 MONITOR: DELTA = TSR - ORIGINAL, ORIGINAL = input", fmt("delta err=%.3g original err=%.3g", m, mo));
    }
    {
        auto [L0, R0] = stereoMix(31, 0.5f);
        SpatialEngine e; e.prepare(fs, 512); Buf L = L0, R = R0; Params p; p.bypass = true;
        run(e, L, R, p);
        double err = std::max(maxErrDelayed(L, L0, LAT), maxErrDelayed(R, R0, LAT));
        // bypass engaged from the first sample fades from processed over 20 ms -> check after 30 ms
        double err2 = 0; for (size_t i = (size_t) (0.03 * fs); i < (size_t) N; ++i) err2 = std::max(err2, (double) std::fabs(L[i] - L0[i - (size_t) LAT]));
        check(err2 < 1e-7, "T21 BYPASS = latency-compensated input", fmt("err after fade=%.3g (whole=%.3g)", err2, err));
    }
    {
        SpatialEngine e; e.prepare(fs, 512); Params p; p.coherence = 1; p.depth = 1; p.width = 2; p.motion = 1; p.earlyLate = 1; p.depthGuard = 0; p.monoSafety = 0; p.centerLock = 0;
        Buf L = noise(N, 40, 10.f), R = noise(N, 41, 10.f);
        L[1000] = std::nanf(""); R[2000] = INFINITY;
        run(e, L, R, p);
        bool fin = true; double pk = 0; for (int i = 0; i < N; ++i) { fin &= std::isfinite(L[(size_t) i]) && std::isfinite(R[(size_t) i]); pk = std::max(pk, (double) std::fabs(L[(size_t) i])); }
        Buf Z((size_t) N, 0.f), Z2 = Z; SpatialEngine e2; e2.prepare(fs, 512); run(e2, Z, Z2, p);
        double zm = 0; for (int i = 0; i < N; ++i) zm = std::max(zm, (double) std::fabs(Z[(size_t) i]));
        check(fin && pk < 200 && zm == 0.0, "T22 robustness: NaN/Inf input, extreme gain, silence->silence", fmt("finite=%.0f peak=%.1f silence max=%.3g", fin, pk, zm));
    }
    {
        auto [L0, R0] = stereoMix(50, 0.5f);
        bool ok = true; std::string d;
        for (int bs : {1, 7, 64, 333, 4096}) {
            SpatialEngine e; e.prepare(fs, bs); Buf L = L0, R = R0; run(e, L, R, neutral(), bs);
            double err = maxErrDelayed(L, L0, LAT); ok &= err < 1e-6;
            SpatialEngine e2; e2.prepare(fs, bs); Buf L2 = L0, R2 = R0; Params p; run(e2, L2, R2, p, bs);
            for (float v : L2) ok &= std::isfinite(v);
            d += fmt("bs%.0f:%.1g ", bs, err);
        }
        check(ok, "T23 block sizes 1..4096: neutral exact, default finite", d);
    }
    {
        Buf L0 = lowpassed(noise(N, 60), fs, 18000), R0 = sincDelay(L0, 12.5);
        SpatialEngine e; e.prepare(fs, 512); Buf L = L0, R = R0; Params p = neutral(); p.coherence = 0.3f; run(e, L, R, p);
        auto s1 = e.solveSpace(p);
        auto [A, B] = stereoMix(61, 0.5f); SpatialEngine e2; e2.prepare(fs, 512); Params p2 = neutral(); p2.width = 2; p2.monoSafety = 1; run(e2, A, B, p2);
        auto s2 = e2.solveSpace(p2);
        check(s1.coherence == 1.f && s1.changed && s2.width < 1.9f && s2.width > 1.0f, "T24 SOLVE SPACE: coherence->100% on lag, width->safe value",
              fmt("coh %.2f->%.2f  width 2.00->%.3f", p.coherence, s1.coherence, s2.width));
    }
    {
        auto [L0, R0] = stereoMix(70, 0.5f);
        SpatialEngine e; e.prepare(fs, 512); Buf L = L0, R = R0; Params p;   // defaults = HTML values
        auto t0 = std::chrono::steady_clock::now(); run(e, L, R, p); auto t1 = std::chrono::steady_clock::now();
        double sec = std::chrono::duration<double>(t1 - t0).count(), audio = N / fs;
        check(sec < audio * 0.25, "T25 CPU: default params, 48 kHz stereo", fmt("%.3f s for %.1f s audio -> %.1f%% of 1 core (x%.0f realtime)", sec, audio, 100 * sec / audio, audio / sec));
    }
    {   // T26: re-prepare at a new sample rate must behave exactly like a fresh engine (no stale fs-dependent state)
        tsr::Params p; p.depth = 0.9f; p.earlyLate = 1.f;
        std::mt19937 r(3); std::normal_distribution<float> n(0, 0.2f); std::vector<float> L(96000), R(96000);
        for (int i = 0; i < 96000; ++i) { L[i] = n(r); R[i] = n(r); }
        tsr::SpatialEngine a; a.prepare(44100, 256); { auto l = L, rr = R; a.process(l.data(), rr.data(), 44100, p); } a.prepare(96000, 256);
        tsr::SpatialEngine b; b.prepare(96000, 256);
        auto l1 = L, r1 = R, l2 = L, r2 = R; a.process(l1.data(), r1.data(), 96000, p); b.process(l2.data(), r2.data(), 96000, p);
        double d = 0; for (int i = 0; i < 96000; ++i) d = std::max(d, (double) std::fabs(l1[i] - l2[i]) + std::fabs(r1[i] - r2[i]));
        check(d == 0.0, "T26 re-prepare 44.1k -> 96k == fresh engine", fmt("max diff=%.3g", d));
    }
    {   // T27: wide stereo synth (detuned saws + independent chorus per side): no real time offset -> no correction applied
        const double fs = 48000; const int N = (int) fs * 20;
        std::vector<float> L(N), R(N);
        double pl = 0, pr = 0;
        for (int i = 0; i < N; ++i) {
            double t = i / fs;
            double fl = 110.0 * (1 + 0.004 * std::sin(2 * M_PI * 0.31 * t)), fr = 110.7 * (1 + 0.004 * std::sin(2 * M_PI * 0.23 * t + 1));
            pl += fl / fs; pr += fr / fs; pl -= std::floor(pl); pr -= std::floor(pr);
            double sl = 0, sr = 0;
            for (int h = 1; h <= 24; ++h) { sl += std::sin(2 * M_PI * h * pl) / h; sr += std::sin(2 * M_PI * h * pr) / h; }
            L[i] = (float) (0.2 * sl); R[i] = (float) (0.2 * sr);
        }
        tsr::SpatialEngine e; e.prepare(fs, 512); tsr::Params p; p.coherence = 1.f; p.depth = 0; p.motion = 0; p.width = 1;
        long on = 0, blocks = 0; double mx = 0;
        for (int s = 0; s < N; s += 512) { e.process(L.data() + s, R.data() + s, std::min(512, N - s), p);
            float a = std::fabs(e.meters.appliedDelayMs.load()); mx = std::max(mx, (double) a); if (s > fs * 2) { ++blocks; if (a > 0.05f) ++on; } }
        check(on == 0, "T27 wide detuned synth: no false time alignment", fmt("time with delay>0.05ms=%.2f%% max applied=%.3f ms", 100.0 * on / std::max(1L, blocks), mx));
    }
    {   // T28: no flush-to-zero here: signal then 30 s of silence -> no subnormal output, no CPU blow-up
        const double fs = 48000; const int N = (int) fs * 32;
        std::vector<float> L(N, 0.f), R(N, 0.f); std::mt19937 r(9); std::normal_distribution<float> n(0, 0.3f);
        for (int i = 0; i < (int) fs * 2; ++i) { L[i] = n(r); R[i] = 0.5f * L[i] + 0.5f * n(r); }
        tsr::SpatialEngine e; e.prepare(fs, 512); tsr::Params p; p.depth = 1; p.earlyLate = 1; p.motion = 0.f;
        auto now = [] { return std::chrono::steady_clock::now(); };
        double tSig = 0, tSil = 0; long sub = 0;
        for (int s = 0; s < N; s += 512) {
            auto t0 = now(); e.process(L.data() + s, R.data() + s, 512, p); double dt = std::chrono::duration<double>(now() - t0).count();
            if (s < fs * 2) tSig += dt; else if (s > fs * 12) tSil += dt;
            for (int i = 0; i < 512; ++i) if (std::fpclassify(L[s + i]) == FP_SUBNORMAL || std::fpclassify(R[s + i]) == FP_SUBNORMAL) ++sub;
        }
        double perSig = tSig / (fs * 2), perSil = tSil / (fs * 20);
        check(sub == 0 && perSil < 2.0 * perSig, "T28 denormal guard (no FTZ): silence after signal", fmt("subnormal out=%.0f  cpu silence/signal=%.2f", (double) sub, perSil / perSig));
    }
    {   // T29: default settings are level-neutral (no loudness bias against BYPASS)
        const double fs = 48000; const int N = (int) fs * 10;
        std::vector<float> L(N), R(N); std::mt19937 r(4); std::normal_distribution<float> n(0, 0.2f);
        float lp = 0; for (int i = 0; i < N; ++i) { lp += 0.2f * (n(r) - lp); L[i] = R[i] = lp * (0.5f + 0.5f * (float) std::sin(2 * M_PI * 2 * i / fs)); }
        auto Li = L; tsr::SpatialEngine e; e.prepare(fs, 512); tsr::Params p;
        for (int s = 0; s < N; s += 512) e.process(L.data() + s, R.data() + s, std::min(512, N - s), p);
        double ei = 0, eo = 0; for (int i = (int) fs; i < N; ++i) { ei += 2.0 * Li[i] * Li[i]; eo += (double) L[i] * L[i] + (double) R[i] * R[i]; }
        double d = 10 * std::log10(eo / ei);
        check(std::fabs(d) < 0.5, "T29 defaults are level-neutral (mono source)", fmt("level change=%+.2f dB", d));
    }
    {   // T30: defaults level-neutral on the brightest material (white noise, like hi-hats)
        const double fs = 48000; const int N = (int) fs * 10;
        std::vector<float> L(N), R(N); std::mt19937 r(6); std::normal_distribution<float> n(0, 0.1f);
        for (int i = 0; i < N; ++i) { L[i] = n(r); R[i] = 0.8f * L[i] + 0.2f * n(r); }
        auto Li = L, Ri = R; tsr::SpatialEngine e; e.prepare(fs, 512); tsr::Params p;
        for (int s = 0; s < N; s += 512) e.process(L.data() + s, R.data() + s, std::min(512, N - s), p);
        double ei = 0, eo = 0; for (int i = (int) fs * 2; i < N; ++i) { ei += (double) Li[i] * Li[i] + (double) Ri[i] * Ri[i]; eo += (double) L[i] * L[i] + (double) R[i] * R[i]; }
        double d = 10 * std::log10(eo / ei);
        check(std::fabs(d) < 0.5, "T30 defaults are level-neutral (bright, hi-hat-like)", fmt("level change=%+.2f dB", d));
    }
    {   // T31 (1.0.1 regression, found on real stems): wide / already-problematic stereo must not be blocked by a broken
        //       fail-safe, harmless settings must not trigger it, and the defaults must not degrade problematic material.
        const double fs = 48000; const int N = (int) fs * 12;
        auto make = [&](float k, unsigned seed, std::vector<float>& L, std::vector<float>& R) {   // k: shared part (0..1)
            std::mt19937 r(seed); std::normal_distribution<float> n(0, 0.15f); L.assign(N, 0); R.assign(N, 0);
            float a = 0, b = 0, cc = 0;
            for (int i = 0; i < N; ++i) { a += 0.25f * (n(r) - a); b += 0.25f * (n(r) - b); cc += 0.25f * (n(r) - cc);
                L[i] = k * cc + (1 - k) * a; R[i] = k * cc + (1 - k) * b; }
        };
        struct Res { double red, iIn, iOut, cIn, cOut, mIn, mOut; };
        auto run = [&](std::vector<float> L, std::vector<float> R, tsr::Params p) {
            tsr::SpatialEngine e; e.prepare(fs, 512); Res s{0, 0, 0, 0, 0, 0, 0}; long k = 0;
            for (int i = 0; i < N; i += 512) { e.process(L.data() + i, R.data() + i, std::min(512, N - i), p); if (i < fs * 2) continue;
                auto& M = e.meters; s.red += M.reduction.load(); s.iIn += M.integrityIn.load(); s.iOut += M.integrityOut.load();
                s.cIn += M.corrIn.load(); s.cOut += M.corrOut.load(); s.mIn += M.monoLossIn.load(); s.mOut += M.monoLossOut.load(); ++k; }
            s.red /= k; s.iIn /= k; s.iOut /= k; s.cIn /= k; s.cOut /= k; s.mIn /= k; s.mOut /= k; return s;
        };
        std::vector<float> L, R;
        make(0.42f, 21, L, R);                 // wide but healthy (corr ~ +0.35, like the PAD stem)
        tsr::Params h; h.width = 1.f; h.motion = 0.f; h.depth = 0.f;
        Res a = run(L, R, h);
        check(a.red < 0.02, "T31a harmless settings on wide stereo: fail-safe stays off", fmt("mean reduction=%.1f%% (corr in %.2f)", a.red * 100, a.cIn));
        Res b = run(L, R, tsr::Params());
        check(b.iOut >= b.iIn - 0.3, "T31b defaults on already-wide stereo: integrity not lowered",
              fmt("mean reduction=%.0f%%  integrity %.1f -> %.1f", b.red * 100, b.iIn, b.iOut));
        std::vector<float> L2, R2; make(0.80f, 23, L2, R2);         // ordinary stereo (corr ~ +0.9)
        Res b2 = run(L2, R2, tsr::Params());
        check(b2.red < 0.2 && b2.iOut >= b2.iIn - 0.3, "T31d defaults on ordinary stereo: moves applied, integrity not lowered",
              fmt("mean reduction=%.0f%%  integrity %.1f -> %.1f  corr %.2f", b2.red * 100, b2.iIn, b2.iOut, b2.cIn));
        make(0.30f, 22, L, R); for (auto& v : R) v = -v * 0.6f + 0.0f;   // problematic (negative correlation)
        Res c2 = run(L, R, tsr::Params());
        check(c2.cOut >= c2.cIn - 0.03 && c2.mOut >= c2.mIn - 0.10, "T31c defaults never degrade already-problematic stereo",
              fmt("corr %+.3f -> %+.3f  mono loss %+.2f -> %+.2f dB", c2.cIn, c2.cOut, c2.mIn, c2.mOut));
    }
    // ---------------------------------------------------------------- 1.0.3
    {   // T32: sample rates below 22 kHz (1.0.2: NaN/Inf at 8 kHz, filter corners above Nyquist)
        bool ok = true; std::string d;
        for (double sr : {8000.0, 11025.0, 16000.0}) for (int mx = 0; mx < 2; ++mx) {
            SpatialEngine e; e.prepare(sr, 512); Params p;
            if (mx) { p.coherence = 1; p.depth = 1; p.earlyLate = 1; p.width = 2; p.motion = 1; p.monoSafety = 0; p.centerLock = 0; p.depthGuard = 0; }
            Buf L = noise((int) sr * 4, 80 + (uint64_t) mx, 0.2f), R = noise((int) sr * 4, 90 + (uint64_t) mx, 0.2f);
            for (size_t i = 0; i < L.size(); ++i) R[i] = 0.6f * L[i] + 0.4f * R[i];
            run(e, L, R, p); double pk = 0; bool fin = true;
            for (size_t i = 0; i < L.size(); ++i) { fin &= std::isfinite(L[i]) && std::isfinite(R[i]); if (fin) pk = std::max(pk, (double) std::max(std::fabs(L[i]), std::fabs(R[i]))); }
            ok &= fin && pk < 5.0; d += fmt("%.0fHz/", sr) + (mx ? "max" : "def") + (fin ? fmt(":%.2f ", pk) : std::string(":NaN "));
        }
        check(ok, "T32 8 / 11 / 16 kHz: finite, bounded (default + max)", d + "(peak)");
    }
    auto xspec = [](const Buf& x, const Buf& y, size_t a, size_t b, double sr, double f) {   // |Sxy| / Sxx in dB (Hann, Welch)
        const int Nw = 4096; std::complex<double> sxy = 0; double sxx = 0;
        std::vector<std::complex<double>> ph((size_t) Nw); for (int i = 0; i < Nw; ++i) ph[(size_t) i] = std::polar(1.0, -2 * kPi * f / sr * i);
        for (size_t s0 = a; s0 + Nw <= b; s0 += Nw / 2) { std::complex<double> X = 0, Y = 0;
            for (int i = 0; i < Nw; ++i) { double w = 0.5 - 0.5 * std::cos(2 * kPi * i / Nw); X += w * (double) x[s0 + (size_t) i] * ph[(size_t) i]; Y += w * (double) y[s0 + (size_t) i] * ph[(size_t) i]; }
            sxy += std::conj(X) * Y; sxx += std::norm(X); }
        return 20 * std::log10(std::abs(sxy) / sxx);
    };
    for (double sr : {44100.0, 48000.0}) {   // T33: PHASE fractional delay keeps the top end (1.0.2: -3.3 dB @16 kHz on the corrected channel)
        const int n = (int) (sr * 8);
        Buf L0 = noise(n, 120), R0 = sincDelay(L0, 9.5), L = L0, R = R0;   // worst case: half a sample
        SpatialEngine e; e.prepare(sr, 512); Params p = neutral(); p.coherence = 1; run(e, L, R, p);
        double worst = 0; std::string d;
        for (double f : {10000.0, 14000.0, 16000.0, 18000.0}) {
            double gL = xspec(L0, L, (size_t) (4 * sr), (size_t) (n - 8192), sr, f), gR = xspec(R0, R, (size_t) (4 * sr), (size_t) (n - 8192), sr, f);
            worst = std::max(worst, std::fabs(gL - gR)); d += fmt("%.0fk:%+.3f ", f / 1000, gL - gR);
        }
        char nm[96]; std::snprintf(nm, sizeof nm, "T33 PHASE half-sample delay flat to 18 kHz @%.1fk", sr / 1000);
        check(worst < 0.05 && std::fabs(e.meters.lagMs.load() - 9.5 / sr * 1000) < 0.002, nm, d + fmt("(L-R dB) lag %.4f ms", e.meters.lagMs.load()));
    }
    {   // T34: DEPTH and MIX automation without HF artefacts (1.0.2: -71 / -72 dB from 0.05 dB shelf coefficient steps)
        auto art = [&](int which) {
            const int n = (int) (fs * 6); Buf L((size_t) n), R((size_t) n);
            for (int i = 0; i < n; ++i) { L[(size_t) i] = (float) (0.4 * std::sin(2 * kPi * 220 * i / fs)); R[(size_t) i] = (float) (0.4 * std::sin(2 * kPi * 220 * i / fs + 0.6)); }
            SpatialEngine e; e.prepare(fs, 64); Params p;
            for (int s0 = 0; s0 < n; s0 += 64) { bool hi = ((s0 / (int) (0.25 * fs)) & 1) != 0; if (which == 0) p.depth = hi; else p.mix = hi; e.process(L.data() + s0, R.data() + s0, std::min(64, n - s0), p); }
            Biquad h[4], g[4]; for (auto& q : h) q.highpass(fs, 4000, 0.707); for (auto& q : g) q.highpass(fs, 4000, 0.707);
            double eh = 0, et = 0;
            for (int i = 0; i < n - 64; ++i) { float x = L[(size_t) i], y = R[(size_t) i]; for (auto& q : h) x = q.process(x); for (auto& q : g) y = q.process(y);
                if (i >= (int) fs) { eh += (double) x * x + (double) y * y; et += (double) L[(size_t) i] * L[(size_t) i] + (double) R[(size_t) i] * R[(size_t) i]; } }
            return 10 * std::log10(eh / et + 1e-300);
        };
        double d0 = art(0), d1 = art(1);
        check(d0 < -100 && d1 < -100, "T34 DEPTH / MIX automation: no HF artefacts", fmt("HF(>4k)/total: DEPTH %.1f dB  MIX %.1f dB", d0, d1));
    }
    {   // T35: meters off (no editor open) -> audio bit-identical
        auto [L0, R0] = stereoMix(140, 0.6f);
        SpatialEngine a, b; a.prepare(fs, 512); b.prepare(fs, 512); b.setMetersActive(false); Params p;
        Buf La = L0, Ra = R0, Lb = L0, Rb = R0; run(a, La, Ra, p); run(b, Lb, Rb, p);
        long diff = 0; for (size_t i = 0; i < La.size(); ++i) diff += (La[i] != Lb[i]) + (Ra[i] != Rb[i]);
        check(diff == 0, "T35 meters off -> identical audio", fmt("differing samples=%.0f of %.0f", (double) diff, 2.0 * La.size()));
    }
    {   // T36: defaults are block-size independent (bit-identical for any host buffer)
        auto [L0, R0] = stereoMix(150, 0.6f);
        Buf Lr = L0, Rr = R0; { SpatialEngine e; e.prepare(fs, 512); run(e, Lr, Rr, Params(), 512); }
        long diff = 0; std::string d;
        for (int bs : {1, 7, 64, 333, 4096}) { SpatialEngine e; e.prepare(fs, bs); Buf L = L0, R = R0; run(e, L, R, Params(), bs);
            long k = 0; for (size_t i = 0; i < L.size(); ++i) k += (L[i] != Lr[i]) + (R[i] != Rr[i]); diff += k; d += fmt("bs%.0f:%.0f ", bs, (double) k); }
        check(diff == 0, "T36 defaults: bit-identical for block sizes 1..4096", d + "(differing samples)");
    }
    {   // T37: current == 1.0.2 (output delayed by the 13 extra samples of latency) whenever no fractional PHASE delay is applied
        // and MONO SAFETY is off (1.0.4 changed only the MONO SAFETY time constants and the input clamp at +36 dBFS)
        bool ok = true; std::string d;
        for (int cfg = 0; cfg < 3; ++cfg) {
            auto [L0, R0] = stereoMix(160 + (uint64_t) cfg, 0.7f);
            Params p; tsr_v102::Params q; p.monoSafety = q.monoSafety = 0;
            if (cfg == 1) { p.mode = MODE_MUSIC; q.mode = tsr_v102::MODE_MUSIC; }
            if (cfg == 2) { p.coherence = q.coherence = 1; p.depth = q.depth = 1; p.earlyLate = q.earlyLate = 1; p.width = q.width = 1; p.motion = q.motion = 0;   // no widening move: the fail-safe has nothing to scale
                            p.centerLock = q.centerLock = 0; p.depthGuard = q.depthGuard = 0.3f; }
            SpatialEngine a; a.prepare(fs, 512); tsr_v102::SpatialEngine b; b.prepare(fs, 512);
            Buf La = L0, Ra = R0, Lb = L0, Rb = R0; run(a, La, Ra, p);
            for (size_t o = 0; o < Lb.size(); o += 512) b.process(Lb.data() + o, Rb.data() + o, (int) std::min<size_t>(512, Lb.size() - o), q);
            const int sh = a.latencySamples() - b.latencySamples(); long k = 0;
            for (size_t i = 0; i + (size_t) sh < La.size(); ++i) k += (La[i + (size_t) sh] != Lb[i]) + (Ra[i + (size_t) sh] != Rb[i]);
            ok &= k == 0 && a.meters.appliedDelayMs.load() == 0.f; d += fmt("cfg%.0f:%.0f ", cfg, (double) k);
        }
        check(ok, "T37 == 1.0.2 bit for bit (no PHASE delay, MONO SAFETY off)", d + "(differing samples)");
    }
    {   // T38: lag estimate precision (1.0.2 parabola: 0.107 samples off at quarter-sample lags)
        double worst = 0; std::string d;
        for (double D : {4.25, 20.75, 13.3}) {
            Buf L0 = noise(N, 170), R0 = sincDelay(L0, D), L = L0, R = R0; SpatialEngine e; e.prepare(fs, 512); Params p = neutral(); p.coherence = 1; run(e, L, R, p);
            double err = e.meters.lagMs.load() * fs / 1000 - D; worst = std::max(worst, std::fabs(err)); d += fmt("%.2f:%+.4f ", D, err);
        }
        check(worst < 0.05, "T38 sub-sample lag estimate within 0.05 samples", d + "(error, samples)");
    }
    {   // T39: spaced-pair recording (note-dependent inter-channel delays): PHASE must not chase every note (1.0.2 did)
        auto spaced = [&](unsigned seed, Buf& L, Buf& R) {
            const double secs = 40; long n = (long) (fs * secs); L.assign((size_t) n, 0.f); R.assign((size_t) n, 0.f);
            std::mt19937 rng(seed); std::uniform_real_distribution<double> u(0, 1); std::normal_distribution<double> nd(0, 1); double t = 0;
            while (t < secs - 1.5) {
                double dur = 0.4 + 0.8 * u(rng), d = -20 + 40 * u(rng), g = 0.6 + 0.4 * u(rng), f0 = 150 + 900 * u(rng);
                long a = (long) (t * fs), len = (long) (dur * fs); double ph[6]; for (auto& q : ph) q = 2 * kPi * u(rng);
                std::vector<double> x((size_t) (len + 64));
                for (long i = 0; i < len + 64; i++) { double tt = i / fs, env = std::exp(-3.0 * tt / dur) * (1 - std::exp(-tt / 0.005)); double v = 0;
                    for (int h = 1; h <= 6; h++) { v += std::sin(2 * kPi * f0 * h * tt + ph[h - 1]) / h; }
                    x[(size_t) i] = 0.15 * env * v + 0.01 * env * nd(rng); }
                const int K = 32; const int di = (int) std::floor(d); const double fr = d - di;
                for (long i = 0; i < len && a + i < n; i++) {
                    L[(size_t) (a + i)] += (float) (g * x[(size_t) (i + 32)]); double acc = 0;
                    for (int k = -K; k < K; k++) { long idx = i + 32 - di - k; if (idx < 0 || idx >= (long) x.size()) continue; double tt = k - fr;
                        double sn = std::fabs(tt) < 1e-12 ? 1 : std::sin(kPi * tt) / (kPi * tt); acc += x[(size_t) idx] * sn * 0.5 * (1 + std::cos(kPi * tt / (K + 1))); }
                    R[(size_t) (a + i)] += (float) ((1.6 - g) * acc);
                }
                t += 0.25 + 0.5 * u(rng);
            }
        };
        Buf L0, R0; spaced(4, L0, R0);
        auto episodes = [&](auto& e, auto p, double& frac) { Buf L = L0, R = R0; int ep = 0; bool on = false; long act = 0, tot = 0;
            for (size_t o = 0; o < L.size(); o += 256) { e.process(L.data() + o, R.data() + o, (int) std::min<size_t>(256, L.size() - o), p);
                bool nowOn = std::fabs(e.meters.appliedDelayMs.load()) > 0.01; if (nowOn && !on) ep++; on = nowOn; act += nowOn; tot++; }
            frac = (double) act / tot; return ep; };
        SpatialEngine a; a.prepare(fs, 256); tsr_v102::SpatialEngine b; b.prepare(fs, 256); double fa = 0, fb = 0;
        Params pa; pa.mode = MODE_MUSIC; tsr_v102::Params pb; pb.mode = tsr_v102::MODE_MUSIC;
        int ea = episodes(a, pa, fa), eb = episodes(b, pb, fb);
        check(ea <= 0.4 * eb && fa < 0.25, "T39 spaced pair: PHASE does not chase note-dependent delays",
              fmt("1.0.3: %.0f corrections, %.0f%% of the time | 1.0.2: %.0f corrections, ", ea, 100 * fa, eb) + fmt("%.0f%% of the time", 100 * fb));
    }
    {   // T40: transient material (hi-hat bursts, 2 mics 9.5 samples apart): correlation normalised per lag (|r| <= 1), the
        //       correction engages and HOLDS (1.0.2: a burst entering the shifted window gave r >> 1 at a bogus lag)
        const int n = (int) (fs * 12); Buf L0((size_t) n, 0.f); Rng r; r.s ^= 77;
        for (int b = 0; b < n; b += (int) (0.125 * fs)) { double g = (b / (int) (0.125 * fs)) % 4 == 0 ? 0.5 : 0.2;
            for (int i = 0; i < (int) (0.06 * fs) && b + i < n; ++i) L0[(size_t) (b + i)] = (float) (g * r.g() * std::exp(-i / (0.012 * fs))); }
        L0 = lowpassed(L0, fs, 18000); Buf R0 = sincDelay(L0, 9.5), L = L0, R = R0;
        auto hold = [&](auto& e, auto p, double& maxConf, double& off) { Buf a = L, b = R; long bad = 0; maxConf = 0; off = 0;
            for (size_t o = 0; o < a.size(); o += 256) { e.process(a.data() + o, b.data() + o, (int) std::min<size_t>(256, a.size() - o), p);
                maxConf = std::max(maxConf, (double) e.meters.phaseConfidence.load());
                if (o > 1.5 * fs) { double d = std::fabs(e.meters.appliedDelayMs.load() * fs / 1000 - 9.5); off = std::max(off, d); if (d > 0.05) ++bad; } }
            return bad; };
        SpatialEngine a; a.prepare(fs, 256); tsr_v102::SpatialEngine b; b.prepare(fs, 256);
        Params pa = neutral(); pa.coherence = 1; tsr_v102::Params pb; pb.coherence = 1; pb.depth = 0; pb.width = 1; pb.motion = 0;
        double ca, cb, oa, ob; long ba = hold(a, pa, ca, oa), bb = hold(b, pb, cb, ob);
        check(ba == 0 && ca <= 1.0, "T40 hi-hat bursts, 2 mics: lag held, |r| <= 1",
              fmt("1.0.3: blocks off-target %.0f, max conf %.3f | 1.0.2: blocks off-target %.0f, ", (double) ba, ca, (double) bb) + fmt("max conf %.1f", cb));
    }
    {   // T41 (1.0.4): MONO SAFETY falls over >= 20 ms (no 64-sample steps) and still reaches a deep reduction on wide material
        Buf L((size_t) (6 * fs)), R((size_t) (6 * fs)); Rng g; g.s ^= 410;
        for (size_t i = 0; i < L.size(); ++i) { const double c = g.g() * 0.2; const bool wide = i >= (size_t) (3 * fs);
            L[i] = (float) (wide ? g.g() * 0.2 : c); R[i] = (float) (wide ? g.g() * 0.2 : c + 0.02 * g.g()); }
        SpatialEngine e; e.prepare(fs, 64); Params p; p.width = 2; p.motion = 1; double worstStep = 0, minS = 1; float prev = 1;
        for (size_t o = 0; o < L.size(); o += 64) { e.process(L.data() + o, R.data() + o, 64, p); const float s1 = e.meters.safeScale.load();
            if (o > (size_t) fs) { worstStep = std::max(worstStep, (double) (prev - s1)); minS = std::min(minS, (double) s1); } prev = s1; }
        const double maxStep = 64.0 / (0.020 * fs) + 1e-4;
        check(worstStep <= maxStep && minS < 0.2, "T41 MONO SAFETY: fall >= 20 ms, still protects wide input", fmt("largest fall per 64 samples %.4f (limit %.4f), min scale %.3f", worstStep, maxStep, minS));
    }
    {   // T42 (1.0.4): one absurd input sample (1e30, e.g. a blown-up plugin before this one) must not bend the state for long
        Buf L0((size_t) (10 * fs)), R0((size_t) (10 * fs)); Rng g; g.s ^= 420;
        for (size_t i = 0; i < L0.size(); ++i) { const double c = g.g() * 0.2; L0[i] = (float) (c + 0.05 * g.g()); R0[i] = (float) (c + 0.05 * g.g()); }
        Buf La = L0, Ra = R0, Lb = L0, Rb = R0; Lb[(size_t) fs] = 1e30f;
        SpatialEngine a, b; a.prepare(fs, 512); b.prepare(fs, 512); Params p; run(a, La, Ra, p); run(b, Lb, Rb, p);
        double d = 0; for (size_t i = (size_t) (fs * 7); i < La.size(); ++i) d = std::max(d, (double) std::max(std::fabs(La[i] - Lb[i]), std::fabs(Ra[i] - Rb[i])));
        bool fin = true; for (size_t i = 0; i < Lb.size(); ++i) fin &= std::isfinite(Lb[i]) && std::isfinite(Rb[i]);
        check(fin && d < 1e-3, "T42 1e30 input spike: finite, recovered after 6 s", fmt("max |diff| vs clean run after 6 s = %.2e", d));
    }
    std::printf("\nRESULT: %d passed, %d failed\n", gPass, gFail);
    return gFail ? 1 : 0;
}
