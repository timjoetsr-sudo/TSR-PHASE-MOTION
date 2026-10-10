// Renders the real TSR editor (fed with real engine analysis) to PNG files for visual verification.
#include <juce_gui_basics/juce_gui_basics.h>
#include <juce_audio_formats/juce_audio_formats.h>
#include "../Source/PluginProcessor.h"
#include "../Source/PluginEditor.h"
#include <random>

static void feed (TSRProcessor& p, juce::AudioBuffer<float>& src, int& pos, int n)
{
    juce::AudioBuffer<float> b (2, 512); juce::MidiBuffer m;
    for (int done = 0; done < n; done += 512) {
        for (int ch = 0; ch < 2; ++ch) for (int i = 0; i < 512; ++i) b.setSample (ch, i, src.getSample (ch, (pos + i) % src.getNumSamples()));
        pos += 512; p.processBlock (b, m);
    }
}

int main (int argc, char** argv)
{
    juce::ScopedJuceInitialiser_GUI init;
    juce::String outDir = argc > 1 ? argv[1] : ".";
    const double fs = 48000; const int N = (int) fs * 8;
    juce::AudioBuffer<float> src (2, N); std::mt19937 rng (7); std::normal_distribution<float> nd (0.f, 0.08f);
    std::vector<float> m ((size_t) N), s ((size_t) N);
    for (int i = 0; i < N; ++i) { m[(size_t) i] = nd (rng) * (i % 19200 < 2400 ? 2.f : 1.f); s[(size_t) i] = 0.4f * nd (rng); }
    for (int i = 0; i < N; ++i) {
        float d = 17.76f; int k = i - (int) d; float f = d - (int) d;
        float md = (k - 1 >= 0 && k < N) ? m[(size_t) k] * (1 - f) + m[(size_t) (k - 1)] * f : 0.f;
        src.setSample (0, i, m[(size_t) i] + s[(size_t) i]); src.setSample (1, i, md - s[(size_t) i]);
    }
    if (argc > 2 && juce::String (argv[2]) == "video") {
        // Real run: processor + editor, frame by frame, with automation; writes PNG frames + the plugin's real audio output.
        const double vfs = 48000; const int fps = 15, spf = (int) vfs / fps, blk = 320; const double dur = 20.0;
        const int NV = (int) (vfs * dur) + spf;
        std::vector<float> s0 ((size_t) NV, 0.f); std::mt19937 r2 (11); std::normal_distribution<float> n2 (0.f, 1.f);
        const double beat = 0.5; const double notes[4] = { 196.0, 246.94, 293.66, 220.0 };
        double hpS = 0, hpX = 0;
        for (int i = 0; i < NV; ++i) {
            double t = i / vfs, tb = std::fmod (t, beat); int b = (int) (t / beat);
            double kick = std::sin (2 * M_PI * (50 + 60 * std::exp (-tb * 30)) * tb) * std::exp (-tb * 9) * 0.30;
            double sn = (b % 2 == 1) ? n2 (r2) * std::exp (-tb * 14) * 0.40 : 0.0;
            double wn = n2 (r2); hpS = 0.92 * (hpS + wn - hpX); hpX = wn;               // cymbal wash: high-passed noise
            double hat = hpS * (0.10 + 0.20 * std::exp (-std::fmod (t, beat / 2) * 18));
            double f0 = notes[(b / 2) % 4], pl = 0;
            for (int h = 1; h <= 6; ++h) pl += std::sin (2 * M_PI * f0 * h * t) * std::exp (-tb * (2.0 + h)) / h;
            s0[(size_t) i] = (float) (kick + sn + hat + 0.16 * pl);
        }
        juce::AudioBuffer<float> in (2, NV);
        const double dl = 0.00037 * vfs;
        for (int i = 0; i < NV; ++i) {
            double x = i - dl; int i0 = (int) std::floor (x); double fr = x - i0;
            float del = (i0 >= 0 && i0 + 1 < NV) ? (float) ((1 - fr) * s0[(size_t) i0] + fr * s0[(size_t) i0 + 1]) : 0.f;
            float room = 0.03f * n2 (r2);
            in.setSample (0, i, s0[(size_t) i] + room); in.setSample (1, i, del - room);
        }
        TSRProcessor p; p.setPlayConfigDetails (2, 2, vfs, blk); p.prepareToPlay (vfs, blk);
        auto setP = [&] (const juce::String& id, float v) { auto* q = p.apvts.getParameter (id); q->setValueNotifyingHost (q->convertTo0to1 (v)); };
        setP (ids::coherence, 0); setP (ids::bypass, 1);
        std::unique_ptr<juce::AudioProcessorEditor> ed (p.createEditor());
        auto* e = dynamic_cast<TSREditor*> (ed.get());
        tsrui::HwButton* solveB = nullptr;
        std::function<void (juce::Component*)> walk = [&] (juce::Component* c) {
            if (auto* h = dynamic_cast<tsrui::HwButton*> (c)) if (h->getKind() == tsrui::HwButton::Solve) solveB = h;
            for (auto* ch : c->getChildren()) walk (ch);
        };
        walk (ed.get());
        juce::AudioBuffer<float> out (2, NV); out.clear();
        juce::AudioBuffer<float> b (2, blk); juce::MidiBuffer midi;
        bool solved = false; int pos = 0;
        const int frames = (int) (dur * fps);
        for (int fr = 0; fr < frames; ++fr) {
            const double t = (double) fr / fps;
            juce::String cap;
            auto ramp = [] (double t, double a, double b2) { return (float) juce::jlimit (0.0, 1.0, (t - a) / (b2 - a)); };
            if (t < 4) cap = juce::String::fromUTF8 ("1 \xc2\xb7 BYPASS ON \xe2\x80\x94 segnale originale: due microfoni sfasati di 0.37 ms");
            else if (t < 7) { setP (ids::bypass, 0); cap = juce::String::fromUTF8 ("2 \xc2\xb7 Plugin attivo, COHERENCE 0% \xe2\x80\x94 nessuna correzione di fase"); }
            else if (t < 10) { setP (ids::coherence, 100.f * ramp (t, 7.2, 9.0)); cap = juce::String::fromUTF8 ("3 \xc2\xb7 COHERENCE 0 \xe2\x86\x92 100% \xe2\x80\x94 L e R vengono allineati: correlazione e integrit\xc3\xa0 salgono"); }
            else if (t < 13) { setP (ids::depth, 58.f + 32.f * ramp (t, 10.3, 12.0)); cap = juce::String::fromUTF8 ("4 \xc2\xb7 Vista DEPTH, DEPTH 58 \xe2\x86\x92 90% \xe2\x80\x94 profondit\xc3\xa0 e riverbero, DEPTH GUARD li limita"); }
            else if (t < 17) { float k = ramp (t, 13.3, 15.0); setP (ids::width, 112.f + 88.f * k); setP (ids::motion, 34.f + 66.f * k);
                               cap = juce::String::fromUTF8 ("5 \xc2\xb7 Vista MOTION, WIDTH 112 \xe2\x86\x92 200%, MOTION 34 \xe2\x86\x92 100% \xe2\x80\x94 MONO SAFETY sorveglia il mono"); }
            else { if (! solved && t >= 17.4 && solveB) { solveB->onClick(); solved = true; }
                   cap = juce::String::fromUTF8 ("6 \xc2\xb7 SOLVE SPACE \xe2\x80\x94 il plugin imposta da solo i valori sicuri"); }
            for (int k = 0; k < spf; k += blk) {
                for (int ch = 0; ch < 2; ++ch) b.copyFrom (ch, 0, in, ch, pos, blk);
                p.processBlock (b, midi);
                for (int ch = 0; ch < 2; ++ch) out.copyFrom (ch, pos, b, ch, 0, blk);
                pos += blk;
            }
            e->tickForTest();
            auto ui = ed->createComponentSnapshot (ed->getLocalBounds(), true, 1.0f);
            juce::Image frame (juce::Image::RGB, 1240, 552 + 64, true);
            { juce::Graphics g (frame); g.fillAll (juce::Colour (0xff0b0d0f)); g.drawImageAt (ui, 0, 0);
              g.setColour (juce::Colour (0xff10151b)); g.fillRect (0, 552, 1240, 64);
              g.setColour (juce::Colour (0xff6fb2ff)); g.fillRect (0, 552, (int) (1240.0 * t / dur), 3);
              g.setFont (tsrui::fonts().get (600, 20.f)); g.setColour (juce::Colour (0xffe4edf8));
              g.drawText (cap, 28, 560, 1100, 32, juce::Justification::centredLeft);
              g.setFont (tsrui::fonts().get (400, 13.f)); g.setColour (juce::Colour (0xff7f90a6));
              auto& mm = p.meters();
              juce::String live = juce::String::fromUTF8 ("Editor e audio reali del plugin \xc2\xb7 ") + juce::String (t, 1) + " s"
                  + juce::String::fromUTF8 ("   |   misurato ora: integrit\xc3\xa0 ") + juce::String (juce::roundToInt (mm.integrityOut.load()))
                  + " (prima " + juce::String (juce::roundToInt (mm.integrityIn.load())) + ")"
                  + juce::String::fromUTF8 (" \xc2\xb7 mono loss ") + juce::String (mm.monoLossOut.load(), 1) + " dB"
                  + juce::String::fromUTF8 (" \xc2\xb7 riduzione mono safety ") + juce::String (juce::roundToInt (mm.reduction.load() * 100)) + "%";
              g.drawText (live, 28, 590, 1180, 20, juce::Justification::centredLeft); }
            juce::File f (outDir + juce::String::formatted ("/f%04d.png", fr)); f.deleteFile();
            juce::FileOutputStream os (f); juce::PNGImageFormat png; png.writeImageToStream (frame, os);
            if (fr % 30 == 0)
                std::printf ("t=%.1fs bypass=%d coh=%.0f view=%d integrity %.1f->%.1f monoLoss %.2f dB reduction %.0f%% lag %.3f ms\n", t,
                             (int) (p.apvts.getRawParameterValue (ids::bypass)->load() > 0.5f), p.apvts.getRawParameterValue (ids::coherence)->load(), 0,
                             p.meters().integrityIn.load(), p.meters().integrityOut.load(), p.meters().monoLossOut.load(), p.meters().reduction.load() * 100, p.meters().lagMs.load());
        }
        auto writeWav = [&] (const juce::String& name, bool mono) {
            juce::File f (outDir + "/" + name); f.deleteFile();
            juce::AudioBuffer<float> w (mono ? 1 : 2, pos);
            for (int i = 0; i < pos; ++i) {
                if (mono) w.setSample (0, i, 0.5f * (out.getSample (0, i) + out.getSample (1, i)));
                else { w.setSample (0, i, out.getSample (0, i)); w.setSample (1, i, out.getSample (1, i)); }
            }
            std::unique_ptr<juce::OutputStream> os (f.createOutputStream().release());
            juce::WavAudioFormat wav;
            std::unique_ptr<juce::AudioFormatWriter> wr (wav.createWriterFor (os.get(), vfs, (unsigned) w.getNumChannels(), 16, {}, 0));
            if (wr) { os.release(); wr->writeFromAudioSampleBuffer (w, 0, pos); }
        };
        writeWav ("out_stereo.wav", false); writeWav ("out_mono.wav", true);
        std::printf ("VIDEO frames=%d samples=%d solved=%d\n", frames, pos, (int) solved);
        return 0;
    }
    struct Shot { const char* name; int unusedView; std::vector<std::pair<juce::String, float>> prm; bool solve, info; };
    std::vector<Shot> shots = {
        { "01_default_PHASE",                 0, {}, false, false },
        { "02_DEPTH_depth80",                 1, { { ids::depth, 80 } }, false, false },
        { "03_MOTION_motion70_width140",      2, { { ids::motion, 70 }, { ids::width, 140 } }, false, false },
        { "04_VOCAL_centerlock100",           2, { { ids::profile, 0 }, { ids::centerLock, 100 }, { ids::motion, 60 } }, false, false },
        { "05_DRUMS_DEPTH",                   1, { { ids::profile, 1 }, { ids::depth, 70 } }, false, false },
        { "06_failsafe_width200_ms100",       2, { { ids::width, 200 }, { ids::motion, 100 }, { ids::monoSafety, 100 } }, false, false },
        { "07_solve_space_from_coh30",        0, { { ids::coherence, 30 }, { ids::width, 200 }, { ids::motion, 80 } }, true, false },
        { "08_DELTA_on",                 0, { { ids::delta, 1 } }, false, false },
        { "09_bypass_ON",                     0, { { ids::bypass, 1 } }, false, false },
        { "10_info_open",                     1, {}, false, true },
    };
    int fails = 0;
    for (auto& d : shots) {
        TSRProcessor p; p.setPlayConfigDetails (2, 2, fs, 512); p.prepareToPlay (fs, 512);
        for (auto& [id, v] : d.prm) { auto* prm = p.apvts.getParameter (id); prm->setValueNotifyingHost (prm->convertTo0to1 (v)); }
        int pos = 0; feed (p, src, pos, (int) fs * 2);
        std::unique_ptr<juce::AudioProcessorEditor> ed (p.createEditor());
        auto* e = dynamic_cast<TSREditor*> (ed.get());
        tsrui::HwButton* solveB = nullptr; tsrui::HwButton* infoB = nullptr;
        std::function<void (juce::Component*)> walk = [&] (juce::Component* c) {
            if (auto* h = dynamic_cast<tsrui::HwButton*> (c)) { if (h->getKind() == tsrui::HwButton::Solve) solveB = h; if (h->getKind() == tsrui::HwButton::Info) infoB = h; }
            for (auto* ch : c->getChildren()) walk (ch);
        };
        walk (ed.get());
        float before = p.apvts.getRawParameterValue (ids::coherence)->load();
        for (int t = 0; t < 90; ++t) { feed (p, src, pos, 1600); e->tickForTest(); }
        if (d.solve && solveB) solveB->onClick();
        if (d.info && infoB) infoB->onClick();
        for (int t = 0; t < 90; ++t) { feed (p, src, pos, 1600); e->tickForTest(); }
        auto img = ed->createComponentSnapshot (ed->getLocalBounds(), true, 1.5f);
        juce::File f (outDir + "/" + d.name + ".png"); f.deleteFile();
        juce::FileOutputStream os (f); juce::PNGImageFormat png; bool ok = png.writeImageToStream (img, os);
        if (! ok || solveB == nullptr || infoB == nullptr) ++fails;
        std::printf ("%s %s %dx%d | view=%d profile=%s delta=%s bypass=%d | coherence %.0f->%.0f width=%.0f | integrity %.1f->%.1f | monoLoss %.2f dB | reduction %.1f%%\n",
                     ok ? "WROTE" : "FAILED", d.name, img.getWidth(), img.getHeight(), d.unusedView,
                     p.apvts.getParameter (ids::profile)->getCurrentValueAsText().toRawUTF8(), p.apvts.getParameter (ids::delta)->getCurrentValueAsText().toRawUTF8(),
                     (int) (p.apvts.getRawParameterValue (ids::bypass)->load() > 0.5f), before, p.apvts.getRawParameterValue (ids::coherence)->load(),
                     p.apvts.getRawParameterValue (ids::width)->load(), p.meters().integrityIn.load(), p.meters().integrityOut.load(),
                     p.meters().monoLossOut.load(), p.meters().reduction.load() * 100);
    }
    return fails;
}
