// TSR PHASE · DEPTH · MOTION — DSP quality harness (headless, no JUCE, no GUI).
// Renders test signals through tsr::SpatialEngine and prints one CSV line per metric:
//   metric,condition,value,unit,threshold,result
// Build: CMake target tsr_dsp_tests  ->  ./build/tests/tsr_dsp_tests > tsr_dsp_metrics.csv
#include "../Source/dsp/SpatialEngine.h"
#include <algorithm>
#include <chrono>
#include <cmath>
#include <complex>
#include <cstdio>
#include <random>
#include <string>
#include <vector>

using namespace tsr;
using Buf = std::vector<float>;
static int nFail = 0;
static void row(const std::string& m, const std::string& c, double v, const char* unit, const std::string& thr, int res /*1 pass 0 fail -1 info*/) {
    if (res == 0) ++nFail;
    std::printf("%s,%s,%.6g,%s,%s,%s\n", m.c_str(), c.c_str(), v, unit, thr.c_str(), res < 0 ? "INFO" : res ? "PASS" : "FAIL");
}
static double dB(double x) { return 20.0 * std::log10(std::max(x, 1e-30)); }
static void run(SpatialEngine& e, Buf& L, Buf& R, const Params& p, int bs = 512) {
    for (size_t o = 0; o < L.size(); o += (size_t) bs) e.process(L.data() + o, R.data() + o, (int) std::min<size_t>((size_t) bs, L.size() - o), p);
}
static Params neutral() { Params p; p.coherence = 0; p.depth = 0; p.width = 1; p.motion = 0; return p; }
static Params maxed() { Params p; p.coherence = 1; p.depth = 1; p.earlyLate = 1; p.width = 2; p.motion = 1; p.monoSafety = 0; p.centerLock = 0; p.depthGuard = 0; return p; }
static void music(double fs, double secs, Buf& L, Buf& R, unsigned seed) {   // correlated pink-ish stereo
    const size_t n = (size_t) (fs * secs); L.assign(n, 0.f); R.assign(n, 0.f); std::mt19937 g(seed); std::normal_distribution<double> nd(0, 1);
    double b0 = 0, b1 = 0, b2 = 0;
    for (size_t i = 0; i < n; i++) { double w = nd(g); b0 = 0.99765 * b0 + w * 0.0990460; b1 = 0.96300 * b1 + w * 0.2965164; b2 = 0.57000 * b2 + w * 1.0526913;
        double c = (b0 + b1 + b2 + w * 0.1848) * 0.05; L[i] = (float) (c + 0.03 * nd(g)); R[i] = (float) (c + 0.03 * nd(g)); }
}
// radix-2 FFT
static void fft(std::vector<std::complex<double>>& a) {
    const size_t n = a.size();
    for (size_t i = 1, j = 0; i < n; i++) { size_t b = n >> 1; for (; j & b; b >>= 1) j ^= b; j ^= b; if (i < j) std::swap(a[i], a[j]); }
    for (size_t len = 2; len <= n; len <<= 1) { const double ang = -2 * kPi / (double) len; std::complex<double> wl(std::cos(ang), std::sin(ang));
        for (size_t i = 0; i < n; i += len) { std::complex<double> w(1); for (size_t k = 0; k < len / 2; k++) { auto u = a[i + k], v = a[i + k + len / 2] * w; a[i + k] = u + v; a[i + k + len / 2] = u - v; w *= wl; } } }
}
// magnitude spectrum of a Hann-windowed frame, scaled so a full-scale sine reads 0 dBFS
static std::vector<double> spec(const float* x, size_t n, bool bh7 = false) {   // Hann, or 7-term Blackman-Harris (side lobes < -180 dB)
    static const double c7[7] = { 0.27105140069342, -0.43329793923448, 0.21812299954311, -0.06592544638803, 0.01081174209837, -0.00077658482522, 0.00001388721735 };
    std::vector<std::complex<double>> a(n); double ws = 0;
    for (size_t i = 0; i < n; i++) { const double t = 2 * kPi * (double) i / (double) n; double w = 0;
        if (bh7) for (int j = 0; j < 7; j++) w += c7[j] * std::cos(j * t); else w = 0.5 - 0.5 * std::cos(t);
        ws += w; a[i] = x[i] * w; }
    fft(a); std::vector<double> m(n / 2); for (size_t k = 0; k < n / 2; k++) m[k] = 2 * std::abs(a[k]) / ws; return m;
}

int main() {
    std::printf("metric,condition,value,unit,threshold,result\n");
    const double rates[] = { 44100, 48000, 88200, 96000, 176400, 192000 };

    // 1. latency: impulse, active (neutral) and bypass, every rate
    for (double fs : rates) for (int byp = 0; byp < 2; byp++) {
        SpatialEngine e; e.prepare(fs, 512); Params p = neutral(); p.bypass = byp != 0;
        Buf L((size_t) fs, 0.f), R((size_t) fs, 0.f); L[1000] = R[1000] = 1.f; run(e, L, R, p);
        size_t pk = 0; for (size_t i = 0; i < L.size(); i++) if (std::fabs(L[i]) > std::fabs(L[pk])) pk = i;
        const long d = (long) pk - 1000 - e.latencySamples();
        row("latency_measured_minus_reported", (byp ? "bypass_" : "active_") + std::to_string((int) fs), (double) d, "samples", "0", d == 0);
    }
    // 2. null tests: bypass and neutral settings (music, 48 kHz)
    for (int mode = 0; mode < 2; mode++) {
        Buf L0, R0; music(48000, 8, L0, R0, 7); Buf L = L0, R = R0; SpatialEngine e; e.prepare(48000, 512);
        Params p = neutral(); if (mode == 0) { p = Params(); p.bypass = true; } run(e, L, R, p);
        const int lat = e.latencySamples(); double md = 0;
        for (size_t i = (size_t) 48000; i + (size_t) lat < L.size(); i++) md = std::max(md, (double) std::max(std::fabs(L[i + (size_t) lat] - L0[i]), std::fabs(R[i + (size_t) lat] - R0[i])));
        row("null_vs_input", mode == 0 ? "bypass(defaults)" : "neutral_settings", dB(md), "dBFS", "<=-140", dB(md) <= -140);
    }
    // 3. THD+N, 1 kHz at -1 dBFS, neutral and defaults (mono source), 48 kHz
    for (int cfg = 0; cfg < 2; cfg++) {
        const double fs = 48000, f0 = 1000.0 * 8192 / 48000.0 == 0 ? 1000 : std::round(1000.0 * 65536 / fs) * fs / 65536;   // bin-centred
        Buf L((size_t) (fs * 4)), R; for (size_t i = 0; i < L.size(); i++) L[i] = (float) (0.891 * std::sin(2 * kPi * f0 * (double) i / fs)); R = L;
        SpatialEngine e; e.prepare(fs, 512); run(e, L, R, cfg ? Params() : neutral());
        auto m = spec(L.data() + (size_t) fs * 2, 65536); size_t k0 = (size_t) std::lround(f0 * 65536 / fs); double s = 0, n = 0;
        for (size_t k = 2; k < m.size(); k++) { const double p2 = m[k] * m[k]; if (k + 3 >= k0 && k <= k0 + 3) s += p2; else if (k * fs / 65536 < 20000) n += p2; }
        const double v = 10 * std::log10(n / s);
        row("thd_n_1k_-1dBFS", cfg ? "defaults(mono_source)" : "neutral", v, "dB", cfg ? "info (DEPTH adds reflections)" : "<=-110", cfg ? -1 : v <= -110);
    }
    // 4. spurious products (aliasing check) on a 0 dBFS log sweep 20 Hz - 20 kHz, 48 kHz: largest bin farther than 1/3 octave
    //    (and 16 bins) from the instantaneous sweep frequency, above 10 Hz; 7-term Blackman-Harris. cfg 3 = unprocessed sweep
    //    through the same analysis (floor of the measurement itself).
    for (int cfg = 0; cfg < 4; cfg++) {
        const double fs = 48000, T = 10; const size_t n = (size_t) (fs * T); Buf L(n), R;
        const double k = std::log(20000.0 / 20.0); double ph = 0;
        for (size_t i = 0; i < n; i++) { const double f = 20 * std::exp(k * (double) i / (double) n); ph += 2 * kPi * f / fs; L[i] = (float) std::sin(ph); } R = L;
        if (cfg < 3) { Params p = cfg == 0 ? neutral() : cfg == 1 ? Params() : maxed(); SpatialEngine e; e.prepare(fs, 512); run(e, L, R, p); }
        double worst = -300, above = -300; const size_t N = 8192;
        for (size_t o = (size_t) fs; o + N < n; o += N / 2) {
            const double fc = 20 * std::exp(k * (double) (o + N / 2) / (double) n); auto m = spec(L.data() + o, N, true);
            const double guard = std::max(fc * 0.26, 16.0 * fs / N);
            for (size_t b = 1; b < m.size(); b++) { const double f = (double) b * fs / N; if (f > 10 && std::fabs(f - fc) > guard) worst = std::max(worst, dB(m[b]));
                if (f > fc + guard) above = std::max(above, dB(m[b])); }   // upward sweep: a linear path can only leave content BELOW the tone (reverb tail)
        }
        const char* nm[] = { "neutral", "defaults", "all_max", "unprocessed_reference" };
        row("sweep_spurious_far_from_tone", nm[cfg], worst, "dBFS", cfg == 0 ? "<=-100" : "info (includes reverb tail below the tone)", cfg == 0 ? worst <= -100 : -1);
        row("sweep_spurious_above_tone(aliasing/nonlinear)", nm[cfg], above, "dBFS", "<=-100", cfg == 3 ? -1 : above <= -100);
    }
    // 5. zipper: one parameter set to a random value every 512-sample block, 440 Hz sine at -6 dBFS; largest spectral bin above 2 kHz
    {
        const char* names[] = { "coherence", "depth", "earlyLate", "width", "motion", "mix", "monoSafety", "centerLock", "depthGuard", "profile", "monitor", "bypass" };
        for (int w = 0; w < 12; w++) {
            const double fs = 48000; const size_t n = (size_t) (fs * 6); Buf L(n), R(n);
            for (size_t i = 0; i < n; i++) { L[i] = (float) (0.5 * std::sin(2 * kPi * 440 * (double) i / fs)); R[i] = (float) (0.5 * std::sin(2 * kPi * 440 * (double) i / fs + 0.3)); }
            SpatialEngine e; e.prepare(fs, 512); Params p; std::mt19937 g(100 + (unsigned) w); std::uniform_real_distribution<double> u(0, 1);
            for (size_t o = 0; o < n; o += 512) {
                const float r = (float) u(g);
                switch (w) { case 0: p.coherence = r; break; case 1: p.depth = r; break; case 2: p.earlyLate = r; break; case 3: p.width = 2 * r; break; case 4: p.motion = r; break;
                    case 5: p.mix = r; break; case 6: p.monoSafety = r; break; case 7: p.centerLock = r; break; case 8: p.depthGuard = r; break;
                    case 9: p.mode = r < 0.33f ? MODE_VOCAL : r < 0.66f ? MODE_DRUMS : MODE_MUSIC; break; case 10: p.monitor = r < 0.5f ? MON_TSR : MON_DELTA; break; case 11: p.bypass = r < 0.5f; break; }
                e.process(L.data() + o, R.data() + o, (int) std::min<size_t>(512, n - o), p);
            }
            double worst = -300; const size_t N = 8192;
            for (size_t o = (size_t) fs; o + N < n; o += N / 2) { auto m = spec(L.data() + o, N); for (size_t b = (size_t) (2000.0 * N / fs); b < m.size(); b++) worst = std::max(worst, dB(m[b])); }
            const bool isMon = w == 10;   // DELTA monitor plays the difference signal: switching it is a level change, not a zipper
            row("zipper_peak_above_2k", std::string(names[w]) + "_random_per_block", worst, "dBFS", isMon ? "info (monitor switch)" : "<=-90", isMon ? -1 : worst <= -90);
        }
    }
    // 6. block-size independence (defaults and all max), sizes 1/7/64/512/4096 and random sizes
    for (int cfg = 0; cfg < 2; cfg++) {
        Buf L0, R0; music(48000, 5, L0, R0, 11); Params p = cfg ? maxed() : Params();
        Buf Lr = L0, Rr = R0; { SpatialEngine e; e.prepare(48000, 512); run(e, Lr, Rr, p, 512); }
        long diff = 0;
        for (int bs : { 1, 7, 64, 4096, -1 }) {
            Buf L = L0, R = R0; SpatialEngine e; e.prepare(48000, 4096);
            if (bs > 0) run(e, L, R, p, bs);
            else { std::mt19937 g(5); std::uniform_int_distribution<int> d(1, 1500); for (size_t o = 0; o < L.size();) { int m = std::min<int>(d(g), (int) (L.size() - o)); e.process(L.data() + o, R.data() + o, m, p); o += (size_t) m; } }
            for (size_t i = 0; i < L.size(); i++) diff += (L[i] != Lr[i]) + (R[i] != Rr[i]);
        }
        row("block_size_independence", cfg ? "all_max" : "defaults", (double) diff, "differing_samples", "0", diff == 0);
    }
    // 7. prepare() called again / reset after use: identical to a fresh instance
    {
        Buf A, B; music(48000, 4, A, B, 21); Buf L1 = A, R1 = B, L2 = A, R2 = B, W1, W2; music(48000, 3, W1, W2, 22);
        SpatialEngine used; used.prepare(44100, 256); run(used, W1, W2, maxed(), 256); used.prepare(48000, 512); run(used, L1, R1, Params());
        SpatialEngine fresh; fresh.prepare(48000, 512); run(fresh, L2, R2, Params());
        long d = 0; for (size_t i = 0; i < L1.size(); i++) d += (L1[i] != L2[i]) + (R1[i] != R2[i]);
        row("re_prepare_state_clean", "used(44.1k,max)->prepare(48k)_vs_fresh", (double) d, "differing_samples", "0", d == 0);
    }
    // 8. extreme inputs: silence, DC, full-scale square, +12 dBFS noise (defaults and all max)
    for (int cfg = 0; cfg < 2; cfg++) for (int sig = 0; sig < 4; sig++) {
        const size_t n = 48000 * 4; Buf L(n), R(n); std::mt19937 g(31); std::normal_distribution<double> nd(0, 1);
        for (size_t i = 0; i < n; i++) { float v = 0;
            if (sig == 1) v = 0.5f;
            if (sig == 2) v = ((i / 55) & 1) ? 1.f : -1.f;
            if (sig == 3) v = (float) (3.98 * std::max(-1.0, std::min(1.0, nd(g) * 0.4)));
            L[i] = v; R[i] = sig == 3 ? (float) (3.98 * std::max(-1.0, std::min(1.0, nd(g) * 0.4))) : v; }
        SpatialEngine e; e.prepare(48000, 512); run(e, L, R, cfg ? maxed() : Params());
        long bad = 0; double pk = 0; for (size_t i = 0; i < n; i++) { if (!std::isfinite(L[i]) || !std::isfinite(R[i])) bad++; else pk = std::max(pk, (double) std::max(std::fabs(L[i]), std::fabs(R[i]))); }
        const char* sn[] = { "silence", "dc_0.5", "square_fullscale", "noise_+12dBFS" };
        row("extreme_input_nonfinite", std::string(sn[sig]) + (cfg ? "_max" : "_defaults"), (double) bad, "samples", "0", bad == 0);
        if (sig == 0) row("extreme_input_silence_peak", cfg ? "max" : "defaults", dB(pk), "dBFS", "<=-200", dB(pk) <= -200);
    }
    // 9. denormals without FTZ: signal then 3 s of silence, subnormal outputs
    {
        Buf L((size_t) 48000 * 5, 0.f), R; music(48000, 2, L, R, 41); L.resize((size_t) 48000 * 5, 0.f); R.resize(L.size(), 0.f);
        SpatialEngine e; e.prepare(48000, 512); run(e, L, R, maxed()); long sub = 0;
        for (size_t i = 0; i < L.size(); i++) sub += (std::fpclassify(L[i]) == FP_SUBNORMAL) + (std::fpclassify(R[i]) == FP_SUBNORMAL);
        row("denormals_in_output_no_ftz", "all_max_signal_then_silence", (double) sub, "samples", "0", sub == 0);
    }
    // 10. time constants in ms independent of the sample rate: MONO SAFETY fall time and PHASE lock time
    for (double fs : rates) {
        // MONO SAFETY: centred noise, then fully decorrelated noise; time for the scale to fall below 0.5
        const size_t n = (size_t) (fs * 4), sw = (size_t) (fs * 2); Buf L(n), R(n); std::mt19937 g(51); std::normal_distribution<double> nd(0, 0.2);
        for (size_t i = 0; i < n; i++) { const double c = nd(g); L[i] = (float) (i < sw ? c : nd(g)); R[i] = (float) (i < sw ? c : nd(g)); }
        SpatialEngine e; e.prepare(fs, 32); Params p; p.width = 2; p.motion = 1; double tFall = -1;
        for (size_t o = 0; o < n; o += 32) { e.process(L.data() + o, R.data() + o, (int) std::min<size_t>(32, n - o), p); if (o > sw && tFall < 0 && e.meters.safeScale.load() < 0.5f) tFall = (double) (o - sw) / fs * 1000; }
        row("monosafety_fall_to_50pct", std::to_string((int) fs), tFall, "ms", "same at every rate", -1);
        // PHASE: R delayed by 0.2 ms, time until the applied delay is within 2 % of COHERENCE x delay
        Buf A, B; music(fs, 6, A, B, 61); const int dsm = (int) std::lround(0.0002 * fs); Buf Rd(A.size(), 0.f); for (size_t i = (size_t) dsm; i < A.size(); i++) Rd[i] = A[i - (size_t) dsm];
        SpatialEngine f; f.prepare(fs, 256); Params q; double tLock = -1; const double target = 0.86 * dsm / fs * 1000;
        for (size_t o = 0; o < A.size(); o += 256) { f.process(A.data() + o, Rd.data() + o, (int) std::min<size_t>(256, A.size() - o), q);
            const double a = f.meters.appliedDelayMs.load(); if (tLock < 0 && std::fabs(std::fabs(a) - target) < 0.02 * target) tLock = (double) o / fs; }
        row("phase_lock_time_0.2ms", std::to_string((int) fs), tLock, "s", "same at every rate", -1);
    }
    // 11. frequency of the DEPTH shelf: level at 4.5 kHz and 15 kHz relative to 200 Hz, mono sine, DEPTH 100 %, every rate
    for (double fs : rates) {
        auto lvl = [&](double f) { const size_t n = (size_t) (fs * 1.5); Buf L(n), R; for (size_t i = 0; i < n; i++) L[i] = (float) (0.25 * std::sin(2 * kPi * f * (double) i / fs)); R = L;
            SpatialEngine e; e.prepare(fs, 512); Params p = neutral(); p.depth = 1; p.depthGuard = 0; run(e, L, R, p);
            double s = 0, c = 0; for (size_t i = n / 2; i < n; i++) { s += L[i] * std::sin(2 * kPi * f * (double) (i - (size_t) e.latencySamples()) / fs); c += L[i] * std::cos(2 * kPi * f * (double) (i - (size_t) e.latencySamples()) / fs); }
            return 0.5 * dB(4 * (s * s + c * c) / ((double) (n / 2) * (double) (n / 2))) - dB(0.25); };
        const double ref = lvl(200);
        row("depth100_level_4.5k_rel_200Hz(shelf+reflections)", std::to_string((int) fs), lvl(4500) - ref, "dB", "same at every rate", -1);
        row("depth100_level_15k_rel_200Hz(shelf+reflections)", std::to_string((int) fs), lvl(15000) - ref, "dB", "same at every rate", -1);
    }
    // 12. CPU: microseconds per 512-sample stereo block, defaults, meters on/off, 48 and 96 kHz (7 runs, median of means, max p99)
    for (double fs : { 48000.0, 96000.0 }) for (int mOn = 1; mOn >= 0; mOn--) {
        Buf A, B; music(fs, 10, A, B, 71); std::vector<double> means, p99s;
        for (int r = 0; r < 7; r++) { Buf L = A, R = B; SpatialEngine e; e.prepare(fs, 512); e.setMetersActive(mOn != 0); Params p; std::vector<double> t;
            for (size_t o = 0; o + 512 <= L.size(); o += 512) { auto t0 = std::chrono::steady_clock::now(); e.process(L.data() + o, R.data() + o, 512, p);
                t.push_back(std::chrono::duration<double, std::micro>(std::chrono::steady_clock::now() - t0).count()); }
            double s = 0; for (double v : t) s += v; means.push_back(s / (double) t.size()); std::sort(t.begin(), t.end()); p99s.push_back(t[(size_t) (t.size() * 0.99)]); }
        std::sort(means.begin(), means.end()); std::sort(p99s.begin(), p99s.end());
        const std::string c = std::to_string((int) fs) + (mOn ? "_meters_on" : "_meters_off");
        row("cpu_us_per_512_block_mean", c, means[3], "us", "info", -1);
        row("cpu_us_per_512_block_p99", c, p99s[3], "us", "info", -1);
    }
    std::printf("TOTAL_FAIL,all,%d,count,0,%s\n", nFail, nFail ? "FAIL" : "PASS");
    return nFail ? 1 : 0;
}
