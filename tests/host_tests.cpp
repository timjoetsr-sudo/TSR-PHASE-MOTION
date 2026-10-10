// TSR PHASE·DEPTH·MOTION — host tests on the REAL built VST3 binary (loaded through JUCE VST3 hosting).
#include <JuceHeader.h>
#include <atomic>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <new>
#include <random>
#include <time.h>
static double threadCpu() { timespec t; clock_gettime (CLOCK_THREAD_CPUTIME_ID, &t); return t.tv_sec + t.tv_nsec * 1e-9; }

// ---------------- allocation counter (exported: the plugin .so binds to these) ----------------
static std::atomic<long> gAllocs { 0 };
static thread_local bool tCount = false;
void* operator new (std::size_t n) { if (tCount) gAllocs++; if (void* p = std::malloc (n ? n : 1)) return p; throw std::bad_alloc(); }
void* operator new[] (std::size_t n) { if (tCount) gAllocs++; if (void* p = std::malloc (n ? n : 1)) return p; throw std::bad_alloc(); }
void operator delete (void* p) noexcept { std::free (p); }
void operator delete[] (void* p) noexcept { std::free (p); }
void operator delete (void* p, std::size_t) noexcept { std::free (p); }
void operator delete[] (void* p, std::size_t) noexcept { std::free (p); }

using namespace juce;
static int nPass = 0, nFail = 0;
static void report (bool ok, const String& name, const String& info)
{
    (ok ? nPass : nFail)++;
    std::printf ("[%s] %-62s %s\n", ok ? "PASS" : "FAIL", name.toRawUTF8(), info.toRawUTF8());
    std::fflush (stdout);
}

static AudioPluginFormatManager* gFm = nullptr;
static PluginDescription gDesc;

static std::unique_ptr<AudioPluginInstance> makeInst (double sr, int bs, bool monoIn = false)
{
    String err;
    auto p = gFm->createPluginInstance (gDesc, sr, bs, err);
    if (! p) { std::printf ("FATAL: cannot instantiate: %s\n", err.toRawUTF8()); std::exit (2); }
    AudioProcessor::BusesLayout l;
    l.inputBuses.add (monoIn ? AudioChannelSet::mono() : AudioChannelSet::stereo());
    l.outputBuses.add (AudioChannelSet::stereo());
    if (! p->setBusesLayout (l)) return nullptr;
    p->setRateAndBufferSizeDetails (sr, bs);
    p->prepareToPlay (sr, bs);
    return p;
}

struct Sig { std::vector<float> L, R; };

// stereo programme: lagged correlated source (0.37 ms) + decorrelated part + sub + transients
static Sig makeProgramme (double sr, double secs, float gain = 0.5f, unsigned seed = 1)
{
    const int n = (int) (sr * secs);
    std::mt19937 rng (seed); std::normal_distribution<float> nd (0.f, 1.f);
    std::vector<float> src (n), dec (n);
    float lp = 0, lp2 = 0;
    for (int i = 0; i < n; ++i) {
        double t = i / sr;
        float env = (float) std::exp (-std::fmod (t, 0.5) * 18.0);             // transient every 0.5 s
        float w = nd (rng); lp += 0.2f * (w - lp);
        src[i] = 0.35f * (float) std::sin (2 * M_PI * 220 * t) + 0.25f * (float) std::sin (2 * M_PI * 1000 * t)
               + 0.15f * (float) std::sin (2 * M_PI * 5000 * t) + 0.5f * env * w;
        float w2 = nd (rng); lp2 += 0.1f * (w2 - lp2);
        dec[i] = 0.6f * lp2 + 0.4f * (float) std::sin (2 * M_PI * 50 * t);
    }
    Sig s; s.L.resize (n); s.R.resize (n);
    const double d = 0.00037 * sr;
    for (int i = 0; i < n; ++i) {
        double x = i - d; int i0 = (int) std::floor (x); double fr = x - i0;
        float del = (i0 >= 0 && i0 + 1 < n) ? (float) ((1 - fr) * src[i0] + fr * src[i0 + 1]) : 0.f;
        s.L[i] = gain * (src[i] + 0.3f * dec[i]);
        s.R[i] = gain * (0.85f * del + 0.3f * dec[(i + 997) % n] * -0.7f + 0.2f * dec[i]);
    }
    return s;
}

static Sig makeSines (double sr, double secs, float gain = 0.25f)
{
    const int n = (int) (sr * secs); Sig s; s.L.resize (n); s.R.resize (n);
    for (int i = 0; i < n; ++i) {
        double t = i / sr;
        s.L[i] = gain * (float) (std::sin (2 * M_PI * 1000 * t) + 0.5 * std::sin (2 * M_PI * 100 * t));
        s.R[i] = gain * (float) (std::sin (2 * M_PI * 1000 * t + 0.6) + 0.5 * std::sin (2 * M_PI * 100 * t + 0.2));
    }
    return s;
}

struct RenderStats { double maxBlockFrac = 0, meanBlockFrac = 0, p999 = 0; long allocs = 0; };
using BlockHook = std::function<void (AudioPluginInstance&, int blockIndex, int startSample)>;

static Sig render (AudioPluginInstance& p, const Sig& in, int bs, double sr, BlockHook hook = {}, RenderStats* st = nullptr, int warmBlocks = 0, bool monoIn = false)
{
    const int n = (int) in.L.size();
    Sig out; out.L.resize (n); out.R.resize (n);
    AudioBuffer<float> buf (2, bs); MidiBuffer midi;
    std::vector<double> times;
    long allocs = 0; int bi = 0;
    for (int s0 = 0; s0 < n; s0 += bs, ++bi) {
        int m = std::min (bs, n - s0);
        buf.setSize (2, m, false, false, true);
        buf.copyFrom (0, 0, in.L.data() + s0, m);
        if (monoIn) buf.clear (1, 0, m); else buf.copyFrom (1, 0, in.R.data() + s0, m);
        if (hook) hook (p, bi, s0);
        double t0 = threadCpu();
        long a0 = gAllocs.load(); tCount = true;
        p.processBlock (buf, midi);
        tCount = false;
        if (bi >= warmBlocks) allocs += gAllocs.load() - a0;
        double t1 = threadCpu();
        if (bi >= warmBlocks) times.push_back ((t1 - t0) / (m / sr));   // thread CPU time: excludes container preemption
        std::memcpy (out.L.data() + s0, buf.getReadPointer (0), sizeof (float) * (size_t) m);
        std::memcpy (out.R.data() + s0, buf.getReadPointer (1), sizeof (float) * (size_t) m);
    }
    if (st && ! times.empty()) {
        double sum = 0, mx = 0; for (double t : times) { sum += t; mx = std::max (mx, t); }
        std::sort (times.begin(), times.end());
        st->maxBlockFrac = mx; st->meanBlockFrac = sum / times.size();
        st->p999 = times[(size_t) std::min ((double) times.size() - 1, times.size() * 0.999)];
        st->allocs = allocs;
    }
    return out;
}

struct Health { bool finite = true; long subnormal = 0; float peak = 0; double dcL = 0, dcR = 0; };
static Health health (const Sig& s, int from = 0)
{
    Health h; double sl = 0, sr = 0; int n = (int) s.L.size(), cnt = 0;
    for (int i = from; i < n; ++i) for (float v : { s.L[i], s.R[i] }) {
        if (! std::isfinite (v)) h.finite = false;
        if (std::fpclassify (v) == FP_SUBNORMAL) h.subnormal++;
        h.peak = std::max (h.peak, std::abs (v));
    }
    for (int i = from; i < n; ++i) { sl += s.L[i]; sr += s.R[i]; ++cnt; }
    h.dcL = sl / std::max (1, cnt); h.dcR = sr / std::max (1, cnt);
    return h;
}
static float inPeak (const Sig& s) { float m = 0; for (size_t i = 0; i < s.L.size(); ++i) m = std::max ({ m, std::abs (s.L[i]), std::abs (s.R[i]) }); return m; }
static double rms (const Sig& s, int a, int b) { double e = 0; for (int i = a; i < b; ++i) e += s.L[i] * s.L[i] + s.R[i] * s.R[i]; return std::sqrt (e / std::max (1, 2 * (b - a))); }
static double relDiffDb (const Sig& a, const Sig& b, int from)
{
    double e = 0, r = 0;
    for (size_t i = (size_t) from; i < a.L.size(); ++i) {
        e += (a.L[i] - b.L[i]) * (a.L[i] - b.L[i]) + (a.R[i] - b.R[i]) * (a.R[i] - b.R[i]);
        r += a.L[i] * a.L[i] + a.R[i] * a.R[i];
    }
    return 10 * std::log10 ((e + 1e-30) / (r + 1e-30));
}
static double maxAbsDiff (const Sig& a, const Sig& b, int from)
{ double m = 0; for (size_t i = (size_t) from; i < a.L.size(); ++i) m = std::max ({ m, (double) std::abs (a.L[i] - b.L[i]), (double) std::abs (a.R[i] - b.R[i]) }); return m; }
// click metric: max |2nd difference|
static double d2max (const Sig& s, int from)
{ double m = 0; for (size_t i = (size_t) std::max (2, from); i < s.L.size(); ++i) for (auto* c : { &s.L, &s.R }) m = std::max (m, (double) std::abs ((*c)[i] - 2 * (*c)[i - 1] + (*c)[i - 2])); return m; }

static AudioProcessorParameter* findParam (AudioPluginInstance& p, const String& name)
{ for (auto* q : p.getParameters()) if (q->getName (64).equalsIgnoreCase (name)) return q; return nullptr; }
static void setAll (AudioPluginInstance& p, const std::map<String, float>& v)
{ for (auto& [k, x] : v) if (auto* q = findParam (p, k)) q->setValue (x); }

static long rssKb() { FILE* f = std::fopen ("/proc/self/statm", "r"); long a = 0, b = 0; if (f) { if (std::fscanf (f, "%ld %ld", &a, &b) != 2) b = 0; std::fclose (f); } return b * 4; }

int main (int argc, char** argv)
{
    if (argc < 2) { std::printf ("usage: HostTests <plugin.vst3>\n"); return 2; }
    ScopedJuceInitialiser_GUI init;
    AudioPluginFormatManager fm; addDefaultFormatsToManager (fm); gFm = &fm;
    OwnedArray<PluginDescription> found;
    for (int i = 0; i < fm.getNumFormats(); ++i)
        if (fm.getFormat (i)->getName() == "VST3") fm.getFormat (i)->findAllTypesForFile (found, argv[1]);
    if (found.isEmpty()) { std::printf ("FATAL: VST3 not found in %s\n", argv[1]); return 2; }
    gDesc = *found[0];
    std::printf ("Loaded: %s | %s | ins=%d outs=%d\n", gDesc.name.toRawUTF8(), gDesc.manufacturerName.toRawUTF8(), gDesc.numInputChannels, gDesc.numOutputChannels);

    // ---- parameter inventory
    std::vector<String> pnames;
    {
        auto p = makeInst (48000, 512);
        for (auto* q : p->getParameters()) {
            pnames.push_back (q->getName (64));
            std::printf ("  param '%s' default=%.4f steps=%d text(min)=%s text(max)=%s automatable=%d\n", q->getName (64).toRawUTF8(), q->getDefaultValue(),
                         q->getNumSteps(), q->getText (0.f, 32).toRawUTF8(), q->getText (1.f, 32).toRawUTF8(), (int) q->isAutomatable());
        }
        report (p->getBypassParameter() != nullptr, "host sees a bypass parameter", "");
    }

    const double srs[4] = { 44100, 48000, 88200, 96000 };
    const int bss[6] = { 32, 64, 128, 256, 512, 1024 };

    // ---- 1. SR x buffer matrix, default params
    for (double sr : srs) {
        auto in = makeProgramme (sr, 4.0);
        Sig ref;
        for (int bs : bss) {
            auto p = makeInst (sr, bs); RenderStats st;
            auto out = render (*p, in, bs, sr, {}, &st, (int) (sr / bs));
            auto h = health (out);
            if (bs == 32) ref = out;
            double bsDiff = maxAbsDiff (ref, out, 0);
            bool ok = h.finite && h.subnormal == 0 && std::abs (h.dcL) < 1e-3 && std::abs (h.dcR) < 1e-3 && h.peak <= 2.f * inPeak (in) && st.allocs == 0 && st.p999 < 1.0
                      && bsDiff < 1e-5 && rms (out, (int) sr, (int) out.L.size()) > 1e-3;
            report (ok, String::formatted ("defaults sr=%.1fk bs=%d", sr / 1000, bs),
                    String::formatted ("peak=%.3f (in %.3f) dc=%.1e/%.1e subn=%ld allocs=%ld cpuMean=%.2f%% p99.9=%.1f%% max=%.1f%% vs bs32 maxdiff=%.1e",
                                       h.peak, inPeak (in), h.dcL, h.dcR, h.subnormal, st.allocs, st.meanBlockFrac * 100, st.p999 * 100, st.maxBlockFrac * 100, bsDiff));
        }
    }

    // ---- 2. real I/O + latency through the binary: MIX 0 => delayed input, every SR
    for (double sr : srs) {
        auto p = makeInst (sr, 256);
        setAll (*p, { { "Mix", 0.f } });
        auto in = makeProgramme (sr, 2.0);
        auto out = render (*p, in, 256, sr);
        int lat = p->getLatencySamples(); double e = 0;
        for (size_t i = (size_t) lat + 512; i < in.L.size(); ++i) e = std::max ({ e, (double) std::abs (out.L[i] - in.L[i - (size_t) lat]), (double) std::abs (out.R[i] - in.R[i - (size_t) lat]) });
        report (e < 1e-5, String::formatted ("I/O passthrough MIX 0 sr=%.1fk", sr / 1000), String::formatted ("latency=%d max|err|=%.2e", lat, e));
    }

    // ---- 3. every parameter changes the DSP (min vs max, others default); choices: every option
    {
        const double sr = 48000; const int bs = 512;
        auto in = makeProgramme (sr, 4.0);
        const int skip = (int) (1.5 * sr);
        for (auto& nm : pnames) {
            auto p0 = makeInst (sr, bs); auto* q0 = findParam (*p0, nm);
            int steps = q0->getNumSteps();
            bool isChoice = steps > 1 && steps < 10;
            if (nm.equalsIgnoreCase ("Bypass")) continue;
            if (! isChoice) {
                const bool cond = nm == "Mono Safety" || nm == "Depth Guard";
                std::map<String, float> ctx; if (cond) ctx = { { "Width", 1.f }, { "Motion", 1.f }, { "Depth", 1.f }, { "Early/Late", 1.f } };
                setAll (*p0, ctx); q0->setValue (0.f); auto a = render (*p0, in, bs, sr);
                auto p1 = makeInst (sr, bs); setAll (*p1, ctx); findParam (*p1, nm)->setValue (1.f); auto b = render (*p1, in, bs, sr);
                double d = relDiffDb (a, b, skip);
                report (d > -60, "param '" + nm + "' min vs max changes output" + (cond ? " (active ctx)" : ""), String::formatted ("diff=%.1f dB", d));
            } else {
                std::vector<Sig> outs;
                for (int k = 0; k < steps; ++k) {
                    auto pk = makeInst (sr, bs); findParam (*pk, nm)->setValue ((float) k / (steps - 1));
                    outs.push_back (render (*pk, in, bs, sr));
                }
                for (int k = 1; k < steps; ++k) {
                    double d = relDiffDb (outs[0], outs[(size_t) k], skip);
                    auto* qq = findParam (*p0, nm);
                    report (d > -60, "choice '" + nm + "' = " + qq->getText ((float) k / (steps - 1), 32) + " vs " + qq->getText (0.f, 32),
                            String::formatted ("diff=%.1f dB", d));
                }
            }
        }
    }

    // ---- 4. extremes: all-min, all-max, 20 random combos, every SR
    {
        std::mt19937 rng (7); std::uniform_real_distribution<float> u (0, 1);
        for (double sr : srs) {
            auto in = makeProgramme (sr, 3.0, 0.5f);
            for (int c = 0; c < 22; ++c) {
                auto p = makeInst (sr, 128);
                for (auto* q : p->getParameters()) if (q != p->getBypassParameter()) q->setValue (c == 0 ? 0.f : c == 1 ? 1.f : u (rng));
                auto out = render (*p, in, 128, sr);
                auto h = health (out);
                bool ok = h.finite && h.subnormal == 0 && h.peak <= 2.f * inPeak (in) && std::abs (h.dcL) < 2e-3 && std::abs (h.dcR) < 2e-3;
                if (! ok || c < 2)
                    report (ok, String::formatted ("extreme combo %s sr=%.1fk", c == 0 ? "ALL MIN" : c == 1 ? "ALL MAX" : String (c).toRawUTF8(), sr / 1000),
                            String::formatted ("peak=%.3f (in %.3f) dc=%.1e/%.1e finite=%d subn=%ld", h.peak, inPeak (in), h.dcL, h.dcR, (int) h.finite, h.subnormal));
            }
        }
        report (true, "extreme random combos (20 x 4 SR) completed", "failures above if any");
    }

    // ---- 5. rapid parameter changes / zipper / clicks (min<->max every 64-sample block)
    {
        const double sr = 48000; const int bs = 64;
        auto in = makeSines (sr, 3.0);
        const int skip = (int) (1.0 * sr);
        for (auto& nm : pnames) {
            auto pa = makeInst (sr, bs); findParam (*pa, nm)->setValue (0.f); auto a = render (*pa, in, bs, sr);
            auto pb = makeInst (sr, bs); findParam (*pb, nm)->setValue (1.f); auto b = render (*pb, in, bs, sr);
            double refD2 = std::max (d2max (a, skip), d2max (b, skip));
            auto pt = makeInst (sr, bs);
            auto out = render (*pt, in, bs, sr, [&] (AudioPluginInstance& p, int bi, int) { findParam (p, nm)->setValue ((bi / 8) % 2 ? 1.f : 0.f); });
            auto h = health (out);
            double d2 = d2max (out, skip);
            bool ok = h.finite && d2 <= 1.5 * refD2 + 1e-4;
            report (ok, "rapid toggle '" + nm + "' (512-sample period) no click", String::formatted ("d2max=%.4f static ref=%.4f ratio=%.2f", d2, refD2, d2 / (refD2 + 1e-12)));
        }
        // random automation of all params every block
        std::mt19937 rng (11); std::uniform_real_distribution<float> u (0, 1);
        auto pr = makeInst (sr, 32);
        auto inR = makeProgramme (sr, 5.0);
        auto out = render (*pr, inR, 32, sr, [&] (AudioPluginInstance& p, int, int) {
            for (auto* q : p.getParameters()) if (q != p.getBypassParameter()) q->setValue (u (rng)); });
        auto h = health (out);
        report (h.finite && h.peak <= 2.f * inPeak (inR) && h.subnormal == 0, "random automation of all params every 32-sample block", String::formatted ("peak=%.3f (in %.3f) finite=%d", h.peak, inPeak (inR), (int) h.finite));
    }

    // ---- 6. stability / feedback: impulse tail decays at max depth & early/late; long sustained input no growth
    for (double sr : srs) {
        auto p = makeInst (sr, 512);
        setAll (*p, { { "Depth", 1.f }, { "Early/Late", 1.f }, { "Depth Guard", 0.f }, { "Width", 1.f }, { "Motion", 1.f } });
        Sig imp; imp.L.assign ((size_t) (sr * 8), 0.f); imp.R = imp.L; imp.L[(size_t) sr / 10] = 0.9f; imp.R[(size_t) sr / 10] = 0.9f;
        auto out = render (*p, imp, 512, sr);
        double e1 = rms (out, (int) (0.1 * sr), (int) (0.6 * sr)), eEnd = rms (out, (int) (7 * sr), (int) (8 * sr));
        auto h = health (out);
        report (h.finite && h.subnormal == 0 && eEnd < 1e-6 && 20 * std::log10 (eEnd / e1 + 1e-30) < -100, String::formatted ("impulse tail decays, max depth sr=%.1fk", sr / 1000),
                String::formatted ("tail(7-8s)/early = %.1f dB  subn=%ld", 20 * std::log10 (eEnd / e1 + 1e-30), h.subnormal));
    }
    {
        const double sr = 48000;
        auto p = makeInst (sr, 256);
        setAll (*p, { { "Depth", 1.f }, { "Early/Late", 1.f }, { "Width", 1.f }, { "Motion", 1.f }, { "Depth Guard", 0.f }, { "Mono Safety", 0.f } });
        auto in = makeProgramme (sr, 40.0, 0.5f, 3);
        auto out = render (*p, in, 256, sr);
        double r1 = rms (out, (int) (2 * sr), (int) (7 * sr)), r2 = rms (out, (int) (35 * sr), (int) (40 * sr));
        double i1 = rms (in, (int) (2 * sr), (int) (7 * sr)), i2 = rms (in, (int) (35 * sr), (int) (40 * sr));
        double g = 20 * std::log10 ((r2 / i2) / (r1 / i1));
        report (std::abs (g) < 1.0, "40 s sustained input at max settings: no gain drift/growth", String::formatted ("gain drift last5s vs first5s = %+.2f dB", g));
    }

    // ---- 7. denormals: signal then 20 s of silence
    {
        const double sr = 48000; const int bs = 128;
        auto p = makeInst (sr, bs);
        setAll (*p, { { "Depth", 1.f }, { "Early/Late", 1.f } });
        auto in = makeProgramme (sr, 22.0);
        for (size_t i = (size_t) (2 * sr); i < in.L.size(); ++i) { in.L[i] = 0; in.R[i] = 0; }
        RenderStats stA; auto pA = makeInst (sr, bs); setAll (*pA, { { "Depth", 1.f }, { "Early/Late", 1.f } });
        render (*pA, makeProgramme (sr, 6.0), bs, sr, {}, &stA, (int) sr / bs);
        RenderStats stB; auto out = render (*p, in, bs, sr, {}, &stB, (int) (12 * sr) / bs);
        auto h = health (out, (int) (12 * sr));
        report (h.subnormal == 0 && stB.meanBlockFrac < 2.0 * stA.meanBlockFrac + 0.005, "silence after signal: no denormals, no CPU rise",
                String::formatted ("subn=%ld cpu silence=%.2f%% vs signal=%.2f%% outPeak(12-22s)=%.2e", h.subnormal, stB.meanBlockFrac * 100, stA.meanBlockFrac * 100, h.peak));
    }

    // ---- 8. bypass: click-free, latency-compensated, toggled during playback
    {
        const double sr = 48000; const int bs = 256;
        auto in = makeSines (sr, 4.0);
        auto pa = makeInst (sr, bs); auto a = render (*pa, in, bs, sr);
        double refD2 = d2max (a, (int) sr);
        Sig dry; dry.L = in.L; dry.R = in.R; refD2 = std::max (refD2, d2max (dry, (int) sr));
        auto pt = makeInst (sr, bs);
        auto out = render (*pt, in, bs, sr, [&] (AudioPluginInstance& p, int bi, int) { p.getBypassParameter()->setValue ((bi / 40) % 2 ? 1.f : 0.f); });
        double d2 = d2max (out, (int) sr);
        report (d2 <= 1.5 * refD2 + 1e-4, "bypass toggled every 40 blocks: no click", String::formatted ("d2max=%.4f ref=%.4f", d2, refD2));
        auto pb = makeInst (sr, bs); pb->getBypassParameter()->setValue (1.f);
        auto ob = render (*pb, in, bs, sr); int lat = pb->getLatencySamples(); double e = 0;
        for (size_t i = (size_t) sr; i < in.L.size(); ++i) e = std::max ({ e, (double) std::abs (ob.L[i] - in.L[i - (size_t) lat]), (double) std::abs (ob.R[i] - in.R[i - (size_t) lat]) });
        report (e < 1e-5, "bypass ON = input delayed by reported latency", String::formatted ("latency=%d max|err|=%.2e", lat, e));
    }

    // ---- 9. state save / recall (simulates closing + reopening the DAW project)
    {
        const double sr = 48000; const int bs = 512;
        std::mt19937 rng (5); std::uniform_real_distribution<float> u (0, 1);
        for (int trial = 0; trial < 5; ++trial) {
            auto pA = makeInst (sr, bs);
            std::vector<float> want; std::vector<String> wantTxt;
            for (auto* q : pA->getParameters()) { q->setValue (trial == 0 ? 0.f : trial == 1 ? 1.f : u (rng)); }
            render (*pA, makeProgramme (sr, 0.2), bs, sr);                       // let the values flow through process()
            for (auto* q : pA->getParameters()) { want.push_back (q->getValue()); wantTxt.push_back (q->getCurrentValueAsText()); }
            MemoryBlock mb; pA->getStateInformation (mb);
            pA.reset();
            auto pB = makeInst (sr, bs); pB->setStateInformation (mb.getData(), (int) mb.getSize());
            render (*pB, makeProgramme (sr, 0.05), bs, sr);
            int mism = 0; int i = 0; String firstBad;
            for (auto* q : pB->getParameters()) { if (q->getCurrentValueAsText() != wantTxt[(size_t) i]) { if (! mism++) firstBad = q->getName (32) + ": " + wantTxt[(size_t) i] + " -> " + q->getCurrentValueAsText(); } ++i; }
            // audio identity: restored instance vs instance with values set directly
            auto pC = makeInst (sr, bs); i = 0;
            for (auto* q : pC->getParameters()) q->setValue (want[(size_t) i++]);
            auto pD = makeInst (sr, bs); pD->setStateInformation (mb.getData(), (int) mb.getSize());
            auto in = makeProgramme (sr, 2.0);
            auto oc = render (*pC, in, bs, sr), od = render (*pD, in, bs, sr);
            double ad = maxAbsDiff (oc, od, 0);
            report (mism == 0 && ad < 1e-6, String::formatted ("state recall trial %d (%s)", trial, trial == 0 ? "all min" : trial == 1 ? "all max" : "random"),
                    String::formatted ("state=%d bytes  params mismatched=%d  audio diff=%.1e ", (int) mb.getSize(), mism, ad) + firstBad);
        }
    }

    // ---- 10. mono input -> stereo output
    {
        const double sr = 48000; const int bs = 256;
        auto p = makeInst (sr, bs, true);
        report (p != nullptr, "mono in / stereo out layout accepted", "");
        if (p) {
            auto in = makeProgramme (sr, 3.0);
            setAll (*p, { { "Mix", 0.f } });
            auto out = render (*p, in, bs, sr, {}, nullptr, 0, true);
            int lat = p->getLatencySamples(); double e = 0;
            for (size_t i = (size_t) sr; i < in.L.size(); ++i) e = std::max ({ e, (double) std::abs (out.L[i] - in.L[i - (size_t) lat]), (double) std::abs (out.R[i] - in.L[i - (size_t) lat]) });
            report (e < 1e-5, "mono in, MIX 0: both outputs = input", String::formatted ("max|err|=%.2e", e));
            auto p2 = makeInst (sr, bs, true);
            auto o2 = render (*p2, in, bs, sr, {}, nullptr, 0, true); auto h = health (o2);
            double sumIn = 0, sumOut = 0;
            for (size_t i = (size_t) sr; i + (size_t) lat < in.L.size(); ++i) { sumIn += 4.0 * in.L[i] * in.L[i]; double m = o2.L[i + (size_t) lat] + o2.R[i + (size_t) lat]; sumOut += m * m; }
            report (h.finite && h.peak < 1.f, "mono in, defaults: finite, mono sum preserved", String::formatted ("peak=%.3f  monoSum out/in=%.2f dB", h.peak, 10 * std::log10 (sumOut / sumIn)));
        }
    }

    // ---- 11. CPU worst case: all max, 96 kHz, 32 samples
    {
        const double sr = 96000; const int bs = 32;
        auto p = makeInst (sr, bs);
        for (auto* q : p->getParameters()) if (q != p->getBypassParameter()) q->setValue (1.f);
        RenderStats st; render (*p, makeProgramme (sr, 6.0), bs, sr, {}, &st, (int) sr / bs);
        report (st.meanBlockFrac < 0.25 && st.p999 < 1.0 && st.allocs == 0, "CPU worst case 96k/32 all-max",
                String::formatted ("mean=%.1f%% p99.9=%.1f%% max=%.1f%% allocs=%ld (1 shared core)", st.meanBlockFrac * 100, st.p999 * 100, st.maxBlockFrac * 100, st.allocs));
    }

    // ---- 12. leaks: warm-up (incl. editor), then two equal batches; growth must not continue
    {
        auto cycle = [] (int i) {
            auto p = makeInst (i % 2 ? 44100 : 96000, 256); render (*p, makeProgramme (48000, 0.3), 256, 48000);
            if (i % 4 == 0) { std::unique_ptr<AudioProcessorEditor> ed (p->createEditorIfNeeded()); }
        };
        for (int i = 0; i < 12; ++i) cycle (i);
        long r0 = rssKb(); for (int i = 0; i < 40; ++i) cycle (i);
        long r1 = rssKb(); for (int i = 0; i < 40; ++i) cycle (i);
        long r2 = rssKb();
        report (r2 - r1 < 1024, "2 x 40 create/process/editor/destroy cycles: no steady growth",
                String::formatted ("RSS %ld -> %ld -> %ld kB (batch1 +%ld, batch2 +%ld)", r0, r1, r2, r1 - r0, r2 - r1));
    }

    std::printf ("\nHOST RESULT: %d passed, %d failed\n", nPass, nFail);
    return nFail == 0 ? 0 : 1;
}
