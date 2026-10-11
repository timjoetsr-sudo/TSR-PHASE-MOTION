// TSR PHASE · DEPTH · MOTION — CPU benchmark 1.0.2 (reference engine) vs current engine, same binary, same machine.
// % of one core = processing time / audio duration. Median of 7 interleaved runs per case.
// Worst block = for every block the MINIMUM time over 7 repetitions (removes OS preemption), then the maximum over blocks.
#include "../Source/dsp/SpatialEngine.h"
#include "ref/SpatialEngine_v102.h"
#include <algorithm>
#include <chrono>
#include <cstdio>
#include <random>
#include <type_traits>
#include <vector>

using Buf = std::vector<float>;
static double now() { return std::chrono::duration<double>(std::chrono::steady_clock::now().time_since_epoch()).count(); }
static void synth(double fs, double secs, Buf& L, Buf& R, unsigned seed) {   // correlated stereo, pink-ish common part
    long n = (long) (fs * secs); L.assign((size_t) n, 0.f); R.assign((size_t) n, 0.f); std::mt19937 rng(seed); std::normal_distribution<double> nd(0, 1);
    double b0 = 0, b1 = 0, b2 = 0;
    for (long i = 0; i < n; i++) { double w = nd(rng); b0 = 0.99765 * b0 + w * 0.0990460; b1 = 0.96300 * b1 + w * 0.2965164; b2 = 0.57000 * b2 + w * 1.0526913;
        double c = (b0 + b1 + b2 + w * 0.1848) * 0.05; L[(size_t) i] = (float) (c + 0.03 * nd(rng)); R[(size_t) i] = (float) (c + 0.03 * nd(rng)); }
}
template <class E, class P> static void setMax(P& p) { p.coherence = 1; p.depth = 1; p.earlyLate = 1; p.width = 2; p.motion = 1; p.monoSafety = 0; p.centerLock = 0; p.depthGuard = 0; }
template <class E, class P> static double pct(double fs, int bs, bool mx, int meters, const Buf& L0, const Buf& R0) {
    Buf L = L0, R = R0; E e; e.prepare(fs, bs); if constexpr (std::is_same<E, tsr::SpatialEngine>::value) e.setMetersActive(meters != 0); else (void) meters;
    P p; p.mode = 5; if (mx) setMax<E>(p);
    double t0 = now();
    for (size_t s = 0; s < L.size(); s += (size_t) bs) e.process(L.data() + s, R.data() + s, (int) std::min<size_t>((size_t) bs, L.size() - s), p);
    return 100.0 * (now() - t0) / (L.size() / fs);
}
template <class E, class P> static std::vector<double> blocks(double fs, int bs, int meters, const Buf& L0, const Buf& R0) {
    Buf L = L0, R = R0; E e; e.prepare(fs, bs); if constexpr (std::is_same<E, tsr::SpatialEngine>::value) e.setMetersActive(meters != 0); else (void) meters;
    P p; p.mode = 5; setMax<E>(p); std::vector<double> t;
    for (size_t s = 0; s + (size_t) bs <= L.size(); s += (size_t) bs) { double a = now(); e.process(L.data() + s, R.data() + s, bs, p); t.push_back(now() - a); }
    return t;
}
template <class E, class P> static void worst(const char* nm, double fs, int bs, int meters, const Buf& L0, const Buf& R0) {
    std::vector<double> m;
    for (int r = 0; r < 7; r++) { auto t = blocks<E, P>(fs, bs, meters, L0, R0); if (m.empty()) m = t; else for (size_t i = 0; i < t.size(); i++) m[i] = std::min(m[i], t[i]); }
    std::vector<double> u(m.begin() + (long) (fs / bs), m.end()); std::sort(u.begin(), u.end());
    const double blk = bs / fs;
    std::printf("  %-22s %6.0f Hz / %3d   median block %5.2f %%   worst block %6.2f %%   worst/median %.2f\n", nm, fs, bs, 100 * u[u.size() / 2] / blk, 100 * u.back() / blk, u.back() / u[u.size() / 2]);
}
static double med(std::vector<double> v) { std::sort(v.begin(), v.end()); return v[v.size() / 2]; }

int main() {
    using E2 = tsr_v102::SpatialEngine; using P2 = tsr_v102::Params; using E3 = tsr::SpatialEngine; using P3 = tsr::Params;
    std::printf("CPU, %% of one core (lower is better). 1.0.3a = meters on (window open), 1.0.3b = meters off (window closed)\n");
    std::printf("  %-8s %-6s %-8s %8s %8s %8s %9s %9s\n", "rate", "block", "params", "1.0.2", "1.0.3a", "1.0.3b", "a vs 1.0.2", "b vs 1.0.2");
    for (double fs : {44100.0, 48000.0, 96000.0, 192000.0}) {
        Buf L0, R0; synth(fs, 8, L0, R0, 5);
        for (int bs : {64, 512}) for (bool mx : {false, true}) {
            std::vector<double> a, b, c;
            for (int r = 0; r < 7; r++) { a.push_back(pct<E2, P2>(fs, bs, mx, 1, L0, R0)); b.push_back(pct<E3, P3>(fs, bs, mx, 1, L0, R0)); c.push_back(pct<E3, P3>(fs, bs, mx, 0, L0, R0)); }
            const double A = med(a), B = med(b), C = med(c);
            std::printf("  %-8.0f %-6d %-8s %8.3f %8.3f %8.3f %+8.1f%% %+8.1f%%\n", fs, bs, mx ? "max" : "default", A, B, C, 100 * (B / A - 1), 100 * (C / A - 1));
            std::fflush(stdout);
        }
    }
    std::printf("\nWorst block (all parameters at max, 7 repetitions, per-block minimum):\n");
    for (double fs : {48000.0, 96000.0}) { Buf L0, R0; synth(fs, 10, L0, R0, 9);
        for (int bs : {32, 128}) { worst<E2, P2>("1.0.2", fs, bs, 1, L0, R0); worst<E3, P3>("1.0.3 meters on", fs, bs, 1, L0, R0); worst<E3, P3>("1.0.3 meters off", fs, bs, 0, L0, R0); std::fflush(stdout); } }
    return 0;
}
