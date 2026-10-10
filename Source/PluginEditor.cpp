#include "PluginEditor.h"
#include "BinaryData.h"

using namespace juce;

namespace tsrui {

// ------------------------------------------------------------------ palette 04 "Canna di fucile brunita blu"
namespace C {
static const Colour panel0 (0xff2e3742), panel1 (0xff232a33), panel2 (0xff181d24), edge (0xff45505e),
    ink (0xffd9e4f0), ink2 (0xff8796a8), btn0 (0xff36404c), btn1 (0xff28303a), btnInk (0xffe1ebf7), btnOn (0xff6fb2ff),
    disp (0xff05080c), dispEdge (0xff3f4a58), dispInk (0xffe4edf8), dispInk2 (0xff7f90a6),
    prio (0xffffb36b), conf (0xff6fb2ff), space (0xffbcd8ff),
    kc0 (0xff5d6f86), kc1 (0xff1c232c), kc2 (0xff4c5c70), kc3 (0xff141920), kc4 (0xff5d6f86),
    kf0 (0xff2f3a47), kf1 (0xff191f27), kDark (0xff212832), kLight (0xff5f7189), kInd (0xff6fb2ff), kTick (0xff8796a8);
static const Colour sep = Colour::fromFloatRGBA (170 / 255.f, 200 / 255.f, 1.f, .18f);
// TSR AUDIO line rule: ONE accent per category (MIX = blue of theme 04), only on knob indicator, display traces,
// plugin name, category label and active LEDs. Everything else stays neutral blued metal.
static const Colour accent (0xff6fb2ff);
static const Colour lane = Colour::fromFloatRGBA (160 / 255.f, 200 / 255.f, 1.f, .035f);
}

// ------------------------------------------------------------------ fonts (embedded, from the style-04 reference)
Fonts::Fonts()
{
    w400 = Typeface::createSystemTypefaceFor (BinaryData::Barlow400_ttf, BinaryData::Barlow400_ttfSize);
    w500 = Typeface::createSystemTypefaceFor (BinaryData::Barlow500_ttf, BinaryData::Barlow500_ttfSize);
    w600 = Typeface::createSystemTypefaceFor (BinaryData::Barlow600_ttf, BinaryData::Barlow600_ttfSize);
    w700 = Typeface::createSystemTypefaceFor (BinaryData::Barlow700_ttf, BinaryData::Barlow700_ttfSize);
    inter700 = Typeface::createSystemTypefaceFor (BinaryData::Inter700_ttf, BinaryData::Inter700_ttfSize);
}
Font Fonts::get (int weight, float px, float trackingPx) const
{
    auto tf = weight <= 400 ? w400 : weight <= 500 ? w500 : weight <= 600 ? w600 : w700;
    return Font (FontOptions (tf).withPointHeight (px).withKerningFactor (trackingPx / px));
}
Font Fonts::inter (float px, float trackingPx) const { return Font (FontOptions (inter700).withPointHeight (px).withKerningFactor (trackingPx / px)); }
const Fonts& fonts() { static Fonts f; return f; }

static float textW (const Font& f, const String& s) { return GlyphArrangement::getStringWidth (f, s); }
static void text (Graphics& g, const String& s, const Font& f, Colour c, Rectangle<float> r, Justification j = Justification::centredLeft)
{
    g.setFont (f); g.setColour (c); g.drawText (s, r, j, false);
}
static String U (const char* utf8) { return String (CharPointer_UTF8 (utf8)); }
static const String kDot = U (" \xc2\xb7 "), kMinus = U ("\xe2\x88\x92");

// CSS conic-gradient approximation ("from" angle in degrees, clockwise from 12 o'clock)
static void conic (Graphics& g, Rectangle<float> r, float fromDeg, const std::vector<std::pair<float, Colour>>& stops, int N = 180)
{
    auto c = r.getCentre(); float rad = r.getWidth() * 0.5f;
    for (int i = 0; i < N; ++i) {
        float t0 = (float) i / N, t1 = (float) (i + 1) / N, tm = (t0 + t1) * 0.5f;
        Colour col = stops.back().second;
        for (size_t k = 1; k < stops.size(); ++k)
            if (tm <= stops[k].first) {
                float a = (tm - stops[k - 1].first) / jmax (1e-6f, stops[k].first - stops[k - 1].first);
                col = stops[k - 1].second.interpolatedWith (stops[k].second, a); break;
            }
        Path p; p.startNewSubPath (c);
        p.addCentredArc (c.x, c.y, rad, rad, 0, degreesToRadians (fromDeg + 360.f * t0), degreesToRadians (fromDeg + 360.f * t1 + 0.5f));
        p.closeSubPath(); g.setColour (col); g.fillPath (p);
    }
}
// CSS linear-gradient at an angle over a rectangle
static ColourGradient cssLinear (Rectangle<float> r, float deg, Colour a, Colour b)
{
    float s = std::sin (degreesToRadians (deg)), c = -std::cos (degreesToRadians (deg));
    float len = std::abs (r.getWidth() * s) + std::abs (r.getHeight() * c);
    auto ctr = r.getCentre();
    return ColourGradient (a, ctr.x - s * len * 0.5f, ctr.y - c * len * 0.5f, b, ctr.x + s * len * 0.5f, ctr.y + c * len * 0.5f, false);
}
static Colour withA (Colour c, float a) { return c.withAlpha (a); }

// ------------------------------------------------------------------ knob (collar, knurl with lit tooth, scale, face, sheen)
static Image knobLayer (int px, bool front)
{
    Image im (Image::ARGB, px, px, true);
    Graphics g (im); g.addTransform (AffineTransform::scale ((float) px / 148.f));
    if (! front) {
        Path sh; sh.addEllipse (9, 12, 130, 130);
        DropShadow (Colours::black.withAlpha (0.55f), 9, { 0, 0 }).drawForPath (g, sh);
        g.setColour (Colours::black.withAlpha (0.35f)); g.fillPath (sh);
        Rectangle<float> col (8, 8, 132, 132);
        conic (g, col, 20.f, { { 0.f, C::kc0 }, { .25f, C::kc1 }, { .5f, C::kc2 }, { .75f, C::kc3 }, { 1.f, C::kc4 } });
        g.setGradientFill (ColourGradient (Colours::white.withAlpha (0.6f), 0, 8, Colours::transparentWhite, 0, 30, false));
        g.drawEllipse (col.reduced (0.5f), 1.f);
        g.setGradientFill (ColourGradient (Colours::transparentBlack, 0, 112, Colours::black.withAlpha (0.5f), 0, 140, false));
        g.drawEllipse (col.reduced (1.f), 2.f);
        g.setColour (C::kTick);
        for (int i = 0; i < 11; ++i) {
            float a = degreesToRadians (-135.f + 27.f * i), r1 = (i % 5 == 0) ? 73.5f : 71.5f;
            g.drawLine (74 + 67 * std::sin (a), 74 - 67 * std::cos (a), 74 + r1 * std::sin (a), 74 - r1 * std::cos (a), i % 5 == 0 ? 1.4f : 0.9f);
        }
    } else {
        Rectangle<float> face (20, 20, 108, 108);
        Path fp; fp.addEllipse (face);
        DropShadow (Colours::black.withAlpha (0.45f), 4, { 0, 2 }).drawForPath (g, fp);
        ColourGradient rg (C::kf0, 20 + 0.38f * 108, 20 + 0.30f * 108, C::kf1, 20 + 0.38f * 108 + 101.f, 20 + 0.30f * 108, true);
        rg.clearColours(); rg.addColour (0.0, C::kf0); rg.addColour (0.75, C::kf1); rg.addColour (1.0, C::kf1);
        g.setGradientFill (rg); g.fillPath (fp);
        { Graphics::ScopedSaveState ss (g); g.reduceClipRegion (fp);
          g.setGradientFill (ColourGradient (Colours::transparentBlack, 0, 92, Colours::black.withAlpha (0.45f), 0, 128, false)); g.fillPath (fp);
          g.setGradientFill (ColourGradient (Colours::white.withAlpha (0.35f), 0, 20, Colours::transparentWhite, 0, 26, false)); g.drawEllipse (face.reduced (0.6f), 1.6f);
          conic (g, face, 300.f, { { 0.f, Colours::transparentWhite }, { .08f, Colours::white.withAlpha (0.16f) }, { .18f, Colours::transparentWhite },
                                   { .5f, Colours::transparentWhite }, { .58f, Colours::white.withAlpha (0.08f) }, { .68f, Colours::transparentWhite },
                                   { 1.f, Colours::transparentWhite } }, 120); }
    }
    return im;
}

void LookAndFeel::drawRotarySlider (Graphics& g, int x, int y, int w, int h, float pos, float, float, Slider& s)
{
    const float size = (float) jmin (w, h);
    const float sf = g.getInternalContext().getPhysicalPixelScaleFactor();
    const int px = jmax (8, roundToInt (size * sf * 1.5f));
    static std::map<int, std::pair<Image, Image>> cache;   // message thread only
    auto it = cache.find (px);
    if (it == cache.end()) it = cache.emplace (px, std::make_pair (knobLayer (px, false), knobLayer (px, true))).first;
    Rectangle<float> box ((float) x + (w - size) * 0.5f, (float) y + (h - size) * 0.5f, size, size);
    g.drawImage (it->second.first, box);
    // knurl teeth with the indicator tooth (no pointer line in this style)
    const int N = (int) s.getProperties().getWithDefault ("teeth", 112);
    const float ang = -135.f + 270.f * pos;
    const int lit = ((roundToInt (ang / (360.f / N)) % N) + N) % N;
    {
        Graphics::ScopedSaveState ss (g);
        g.addTransform (AffineTransform::scale (size / 148.f).translated (box.getX(), box.getY()));
        Path t; t.addRoundedRectangle (73.3f, 8.6f, 1.4f, 4.2f, 0.3f);
        for (int i = 0; i < N; ++i) {
            float a = i * MathConstants<float>::twoPi / N;
            float l = 0.5f + 0.5f * std::cos (a + 0.8f);
            g.setColour (i == lit ? C::accent : C::kDark.interpolatedWith (C::kLight, l));
            g.fillPath (t, AffineTransform::rotation (a, 74.f, 74.f));
        }
    }
    g.drawImage (it->second.second, box);
}

// ------------------------------------------------------------------ buttons
static void hwBox (Graphics& g, Rectangle<float> r, float rad, bool pressed)
{
    if (! pressed) DropShadow (Colours::black.withAlpha (0.30f), 4, { 0, 2 }).drawForRectangle (g, r.toNearestInt());
    g.setGradientFill (ColourGradient (C::btn0, 0, r.getY(), C::btn1, 0, r.getBottom(), false));
    g.fillRoundedRectangle (r, rad);
    Graphics::ScopedSaveState ss (g);
    Path p; p.addRoundedRectangle (r, rad); g.reduceClipRegion (p);
    if (pressed) {
        g.setGradientFill (ColourGradient (Colours::black.withAlpha (0.45f), 0, r.getY(), Colours::transparentBlack, 0, r.getY() + 6.f, false));
        g.fillRect (r);
    } else {
        g.setColour (Colours::white.withAlpha (0.45f)); g.fillRect (r.getX(), r.getY() + 0.5f, r.getWidth(), 1.f);
        g.setGradientFill (ColourGradient (Colours::transparentBlack, 0, r.getBottom() - 3.f, Colours::black.withAlpha (0.25f), 0, r.getBottom(), false));
        g.fillRect (r);
    }
    g.setColour (C::edge); g.drawRoundedRectangle (r.reduced (0.5f), rad, 1.f);
}

RoleRow::RoleRow (StringArray l, std::function<bool (int)> on, std::function<void (int)> c) : labels (l), isOn (std::move (on)), click (std::move (c)) {}
void RoleRow::paint (Graphics& g)
{
    auto f = fonts().get (700, 10.f, 1.2f);
    for (int i = 0; i < labels.size(); ++i) {
        Rectangle<float> r (i * (bw + gap), 0, bw, bh);
        bool on = isOn && isOn (i);
        hwBox (g, r, 8.f, on);
        text (g, labels[i], f, C::btnInk, r, Justification::centred);
        if (on) {
            Rectangle<float> u (r.getX() + 10.f, r.getBottom() - 5.f, r.getWidth() - 20.f, 2.f);
            g.setColour (withA (C::accent, 0.35f)); g.fillRoundedRectangle (u.expanded (1.5f, 1.5f), 2.f);   // active LED bar
            g.setColour (C::accent); g.fillRoundedRectangle (u, 1.f);
        }
    }
}
void RoleRow::mouseDown (const MouseEvent& e)
{
    int i = (int) (e.position.x / (bw + gap));
    if (i >= 0 && i < labels.size() && e.position.x - i * (bw + gap) <= bw && click) { click (i); repaint(); }
}

void HwButton::paintButton (Graphics& g, bool, bool down)
{
    auto r = getLocalBounds().toFloat();
    if (kind == Info) {
        hwBox (g, r, 4.f, down);
        Graphics::ScopedSaveState ss (g);
        g.addTransform (AffineTransform::translation (6.f, 7.f).scaled (r.getWidth() / 30.f));
        g.setColour (C::btnInk);
        g.drawEllipse (0.75f, 0.75f, 14.5f, 14.5f, 1.5f);
        g.drawLine (8.f, 7.015f, 8.f, 11.938f, 1.5f); g.drawLine (8.f, 3.692f, 8.f, 4.677f, 1.5f);
        return;
    }
    const bool on = kind == Bypass ? getToggleState() : (flash || down);
    hwBox (g, r, 5.f, on);
    String t = kind == Bypass ? "BYPASS" : "SOLVE SPACE";
    auto f = fonts().get (700, 12.f, 1.2f);
    const float tw = textW (f, t), x0 = r.getCentreX() - (tw + 13.f) * 0.5f;
    Rectangle<float> led (x0, r.getCentreY() - 3.f, 6.f, 6.f);          // LED: accent only when active
    if (on) { g.setColour (withA (C::accent, 0.35f)); g.fillEllipse (led.expanded (2.5f)); g.setColour (C::accent); }
    else g.setColour (Colour (0xff1b2129));
    g.fillEllipse (led);
    tsrui::text (g, t, f, C::btnInk, { x0 + 13.f, r.getY(), tw + 4.f, r.getHeight() });
}

// ------------------------------------------------------------------ panel
class Panel : public Component, private AudioProcessorValueTreeState::Listener
{
public:
    explicit Panel (TSRProcessor& p) : proc (p)
    {
        struct KD { const char* id; const char* label; const char* mn; const char* mx; };
        const KD kd[9] = { { "coherence", "COHERENCE", "0", "100" }, { "depth", "DEPTH", "0", "100" }, { "earlyLate", "EARLY/LATE", "0", "100" },
                           { "width", "WIDTH", "0", "200" }, { "motion", "MOTION", "0", "100" }, { "mix", "MIX", "0", "100" },
                           { "monoSafety", "MONO SAFETY", "0", "100" }, { "centerLock", "CENTER LOCK", "0", "100" }, { "depthGuard", "DEPTH GUARD", "0", "100" } };
        for (int i = 0; i < 9; ++i) {
            auto* s = knobs.add (new Slider (Slider::RotaryVerticalDrag, Slider::NoTextBox));
            s->setLookAndFeel (&lnf);
            s->setRotaryParameters (degreesToRadians (225.f), degreesToRadians (495.f), true);
            s->setMouseDragSensitivity (180);
            s->getProperties().set ("teeth", i == 0 ? 156 : 112);
            addAndMakeVisible (s);
            knobAtt.add (new AudioProcessorValueTreeState::SliderAttachment (proc.apvts, kd[i].id, *s));
            auto* prm = proc.apvts.getParameter (kd[i].id);
            s->setDoubleClickReturnValue (true, prm->convertFrom0to1 (prm->getDefaultValue()));
            s->onValueChange = [this] { repaint (params.toNearestInt()); };
            label[i] = kd[i].label; mn[i] = kd[i].mn; mx[i] = kd[i].mx;
        }
        profAtt  = std::make_unique<ParameterAttachment> (*proc.apvts.getParameter (ids::profile), [this] (float v) { profSel = roundToInt (v); if (profiles) profiles->repaint(); });
        deltaAtt = std::make_unique<ParameterAttachment> (*proc.apvts.getParameter (ids::delta), [this] (float v) { deltaOn = v > 0.5f; if (delta) delta->repaint(); });
        profiles = std::make_unique<RoleRow> (StringArray { "VOCAL", "DRUMS", "MUSIC" }, [this] (int i) { return profSel == i; },
                                              [this] (int i) { profAtt->setValueAsCompleteGesture ((float) i); });
        delta = std::make_unique<RoleRow> (StringArray { "DELTA" }, [this] (int) { return deltaOn; },
                                           [this] (int) { deltaAtt->setValueAsCompleteGesture (deltaOn ? 0.f : 1.f); });
        profAtt->sendInitialUpdate(); deltaAtt->sendInitialUpdate();
        addAndMakeVisible (*profiles); addAndMakeVisible (*delta);

        bypass.setClickingTogglesState (true);
        bypassAtt = std::make_unique<AudioProcessorValueTreeState::ButtonAttachment> (proc.apvts, ids::bypass, bypass);
        bypass.onStateChange = [this] { repaint (valuesR.toNearestInt()); };
        infoBtn.onClick = [this] { infoOpen = ! infoOpen; repaint(); };
        infoBtn.setTooltip ("Come si usa");
        solve.onClick = [this] { proc.solveSpace(); solve.flash = true; solve.repaint(); flashUntil = Time::getMillisecondCounter() + 900; };
        for (Component* c : { (Component*) &bypass, (Component*) &infoBtn, (Component*) &solve }) addAndMakeVisible (c);
        setSize (TSREditor::baseW, TSREditor::baseH);
        proc.apvts.addParameterListener (ids::bypass, this);
    }
    ~Panel() override
    {
        proc.apvts.removeParameterListener (ids::bypass, this);
        for (auto* s : knobs) s->setLookAndFeel (nullptr);
    }

    void parameterChanged (const String&, float) override { MessageManager::callAsync ([sp = SafePointer<Panel> (this)] { if (sp) sp->repaint(); }); }

    void resized() override
    {
        float right = 1240.f - 34.f;
        bypass.setBounds (Rectangle<float> (right - 76.f, 23.f, 76.f, 36.f).toNearestInt()); right -= 76.f + 12.f;
        infoBtn.setBounds (Rectangle<float> (right - 30.f, 25.5f, 30.f, 31.f).toNearestInt()); right -= 30.f + 12.f;
        solve.setBounds (Rectangle<float> (right - 124.f, 23.f, 124.f, 36.f).toNearestInt());
        // DELTA: inside the values display, top right
        delta->setBounds (Rectangle<float> (valuesR.getRight() - 16.f - RoleRow::bw, valuesR.getY() + 12.f, RoleRow::bw, RoleRow::bh).toNearestInt());
        profiles->setBounds (Rectangle<float> (valuesR.getRight() - 16.f - profiles->preferredWidth(), valuesR.getY() + 206.f, profiles->preferredWidth(), RoleRow::bh).toNearestInt());
        knobs[0]->setBounds (69, 366, 148, 148);
        for (int i = 1; i < 9; ++i) knobs[i]->setBounds (roundToInt (smallLeft (i - 1) + 7.f), 388, 100, 100);
    }

    // ---------------------------------------------------------------- live data (message thread, 30 Hz)
    void pullMeters()
    {
        auto& m = proc.meters();
        integIn = m.integrityIn.load(); integOut = m.integrityOut.load();
        mlIn = m.monoLossIn.load(); mlOut = m.monoLossOut.load();
        { const float r = m.reduction.load(); reduction += (r > reduction ? 0.35f : 0.06f) * (r - reduction); }   // display: fast up, ~0.5 s down
        appliedMs = m.appliedDelayMs.load(); lagMs = m.lagMs.load();
        corrIn = m.corrIn.load(); corrOut = m.corrOut.load();
        for (int b = 0; b < 3; ++b) { bandIn[b] = m.bandIn[b].corr.load(); bandOut[b] = m.bandOut[b].corr.load(); }
        live = m.levelDb.load() > -60.f;
        // vectorscope: last kPts points of input and output (mid/side)
        auto& sc = proc.scope();
        const int w = sc.w.load (std::memory_order_acquire);
        float peak = 1e-4f;
        for (int i = 0; i < kPts; ++i) {
            const int k = (w - kPts + i) & (TSRProcessor::Scope::N - 1);
            ptIn[(size_t) i] = { sc.inS[k].load (std::memory_order_relaxed), sc.inM[k].load (std::memory_order_relaxed) };
            ptOut[(size_t) i] = { sc.outS[k].load (std::memory_order_relaxed), sc.outM[k].load (std::memory_order_relaxed) };
            peak = jmax (jmax (peak, std::abs (ptIn[(size_t) i].x), std::abs (ptIn[(size_t) i].y)), jmax (std::abs (ptOut[(size_t) i].x), std::abs (ptOut[(size_t) i].y)));
        }
        scopeScale = peak > scopeScale ? peak : scopeScale * 0.96f + peak * 0.04f;   // fast attack, slow release
        if (solve.flash && Time::getMillisecondCounter() > flashUntil) { solve.flash = false; solve.repaint(); }
        repaint (scopeR.toNearestInt()); repaint (valuesR.toNearestInt());
    }

    // ---------------------------------------------------------------- painting
    void paint (Graphics& g) override
    {
        float sf = g.getInternalContext().getPhysicalPixelScaleFactor();
        if (bg.isNull() || std::abs (sf - bgScale) > 0.01f) {
            bgScale = sf;
            bg = Image (Image::ARGB, jmax (1, roundToInt (1240 * sf)), jmax (1, roundToInt (552 * sf)), true);
            Graphics gb (bg); gb.addTransform (AffineTransform::scale (sf)); paintStatic (gb);
        }
        g.drawImage (bg, getLocalBounds().toFloat());
        paintScope (g);
        paintValuesDisplay (g);
        paintKnobValues (g);
    }
    void paintOverChildren (Graphics& g) override { if (infoOpen) paintInfo (g); }

private:
    static constexpr int kPts = 1024;
    TSRProcessor& proc;
    LookAndFeel lnf;
    OwnedArray<Slider> knobs;
    OwnedArray<AudioProcessorValueTreeState::SliderAttachment> knobAtt;
    std::unique_ptr<ParameterAttachment> profAtt, deltaAtt;
    std::unique_ptr<RoleRow> profiles, delta;
    int profSel = 2; bool deltaOn = false;
    HwButton bypass { HwButton::Bypass }, infoBtn { HwButton::Info }, solve { HwButton::Solve };
    std::unique_ptr<AudioProcessorValueTreeState::ButtonAttachment> bypassAtt;
    uint32 flashUntil = 0;
    bool infoOpen = false, live = false;
    Image bg; float bgScale = 0;
    String label[9], mn[9], mx[9];
    const Rectangle<float> scopeR { 26, 92, 286, 244 }, valuesR { 326, 92, 888, 244 }, params { 0, 337, 1240, 215 };
    std::array<Point<float>, kPts> ptIn {}, ptOut {};
    float scopeScale = 0.05f;
    float integIn = 0, integOut = 0, mlIn = 0, mlOut = 0, reduction = 0, appliedMs = 0, lagMs = 0, corrIn = 1, corrOut = 1;
    float bandIn[3] { 1, 1, 1 }, bandOut[3] { 1, 1, 1 };

    static float smallLeft (int i) { return 300.f + i * 115.75f + 0.375f; }
    static String dB (float v) { return (v < -0.05f ? kMinus : String()) + String (std::abs (v), 1) + " dB"; }
    static String sgn2 (float v) { return (v < 0 ? kMinus : String ("+")) + String (std::abs (v), 2); }

    void paintStatic (Graphics& g)
    {
        const Rectangle<float> P (0, 0, 1240, 552);
        Path pp; pp.addRoundedRectangle (P, 18.f);
        g.setColour (C::panel2); g.fillPath (pp);
        Graphics::ScopedSaveState ss (g);
        g.reduceClipRegion (pp);
        ColourGradient pg (C::panel0, 0, 0, C::panel2, 0, 552, false); pg.addColour (0.5, C::panel1);
        g.setGradientFill (pg); g.fillRect (P);
        // satin texture: horizontal brushed streaks (feTurbulence 0.006 x 0.35), overlay at 16%
        {
            Random rnd (4);
            for (int y = 0; y < 552; ++y) {
                float ph = rnd.nextFloat() * 6.283f, fr = 0.004f + 0.006f * rnd.nextFloat(), amp = rnd.nextFloat();
                for (int x0 = 0; x0 < 1240; x0 += 40) {
                    float v = (std::sin (ph + x0 * fr) * 0.5f + 0.5f) * amp - 0.5f;
                    g.setColour ((v > 0 ? Colours::white : Colours::black).withAlpha (std::abs (v) * 0.16f * 0.45f));
                    g.fillRect ((float) x0, (float) y, 40.f, 1.f);
                }
            }
        }
        // gloss (115deg soft-light bands, opacity .35)
        {
            ColourGradient gl = cssLinear (P, 115.f, Colours::transparentWhite, Colours::transparentWhite);
            gl.addColour (0.18, Colours::transparentWhite); gl.addColour (0.30, Colours::white.withAlpha (0.55f * 0.35f * 0.35f)); gl.addColour (0.42, Colours::transparentWhite);
            gl.addColour (0.58, Colours::transparentWhite); gl.addColour (0.64, Colours::white.withAlpha (0.22f * 0.35f * 0.35f)); gl.addColour (0.70, Colours::transparentWhite);
            g.setGradientFill (gl); g.fillRect (P);
        }
        // header
        {
            Rectangle<float> h (0, 0, 1240, 82);
            ColourGradient hg (Colour::fromFloatRGBA (150 / 255.f, 190 / 255.f, 1.f, .10f), 0, 0, Colours::black.withAlpha (0.25f), 0, 82, false);
            hg.addColour (0.55, Colours::transparentBlack);
            g.setGradientFill (hg); g.fillRect (h);
            g.setColour (C::sep); g.fillRect (0.f, 81.f, 1240.f, 1.f);
            g.setColour (Colours::white.withAlpha (0.12f)); g.fillRect (0.f, 82.f, 1240.f, 1.f);
        }
        paintLogo (g, { 34.f, 8.f });
        {
            float x = 34.f + 150.f + 10.f;
            g.setColour (C::sep); g.fillRect (x, 30.f, 1.f, 22.f); x += 1.f + 10.f;
            auto fN = fonts().get (700, 14.f, 3.08f), fS = fonts().get (500, 10.5f, 1.05f);
            String name = U ("PHASE \xc2\xb7 DEPTH \xc2\xb7 MOTION");
            float nw = textW (fN, name);
            text (g, name, fN, C::accent, { x, 31.f, nw + 4.f, 20.f }); x += nw + 10.f;
            {   // category label
                auto fC = fonts().get (700, 9.f, 1.5f); const float cw = textW (fC, "MIX") + 12.f;
                Rectangle<float> cr (x, 33.f, cw, 16.f);
                g.setColour (withA (C::accent, 0.6f)); g.drawRoundedRectangle (cr.reduced (0.5f), 3.f, 1.f);
                text (g, "MIX", fC, C::accent, cr, Justification::centred);
                x += cw + 10.f;
            }
            String sub = "Spatial coherence & movement engine";
            float limit = (float) solve.getX() - 14.f;
            if (x + textW (fS, sub) < limit) text (g, sub, fS, C::ink2, { x, 32.f, textW (fS, sub) + 4.f, 18.f });
        }
        // displays
        display (g, scopeR); display (g, valuesR);
        // params frame
        g.setColour (C::sep); g.fillRect (0.f, 337.f, 1240.f, 1.f);
        g.setColour (Colours::white.withAlpha (0.18f)); g.fillRect (0.f, 338.f, 1240.f, 1.f);
        // knob labels / scale ends
        auto lblL = fonts().get (700, 12.5f, 2.5f), lblS = fonts().get (700, 11.5f, 1.5f), fmm = fonts().get (600, 9.5f);
        text (g, label[0], lblL, C::ink, { 69.f - 20.f, 345.f, 188.f, 16.f }, Justification::centred);
        text (g, mn[0], fmm, C::ink2, { 23.f, 434.f, 36.f, 12.f }, Justification::centredRight);
        text (g, mx[0], fmm, C::ink2, { 227.f, 434.f, 36.f, 12.f }, Justification::centredLeft);
        // LCD
        Rectangle<float> lcd (103.f, 522.f, 80.f, 30.f);
        g.setColour (C::disp); g.fillRoundedRectangle (lcd, 4.f);
        g.setGradientFill (ColourGradient (Colours::black.withAlpha (0.8f), 0, lcd.getY(), Colours::transparentBlack, 0, lcd.getY() + 7.f, false));
        g.fillRoundedRectangle (lcd, 4.f);
        g.setColour (C::dispEdge); g.drawRoundedRectangle (lcd.reduced (0.5f), 4.f, 1.f);
        for (int i = 1; i < 9; ++i) {
            float L = smallLeft (i - 1);
            text (g, label[i], lblS, C::ink, { L - 10.f, 370.f, 135.f, 15.f }, Justification::centred);
            text (g, mn[i], fmm, C::ink2, { L + 7.f, 492.f, 30.f, 16.f }, Justification::centredLeft);
            text (g, mx[i], fmm, C::ink2, { L + 77.f, 492.f, 30.f, 16.f }, Justification::centredRight);
        }
        // group divider: main moves | safety controls
        float dx = smallLeft (5) - 0.5f;
        g.setColour (C::sep); g.fillRect (dx, 372.f, 1.f, 136.f);
        // panel inset edges
        g.setColour (Colours::white.withAlpha (0.55f)); g.fillRect (18.f, 0.5f, 1204.f, 1.f);
        g.setGradientFill (ColourGradient (Colours::transparentBlack, 0, 548, Colours::black.withAlpha (0.45f), 0, 552, false)); g.fillRect (0.f, 548.f, 1240.f, 4.f);
        g.setColour (C::edge); g.strokePath (pp, PathStrokeType (2.f));   // inner half of the stroke = 1 px edge
    }

    void paintLogo (Graphics& g, Point<float> o)
    {
        auto f36 = fonts().inter (36.f), f18 = fonts().inter (18.f, 0.72f), f8 = fonts().inter (8.f);
        auto emboss = [&] (const String& s, const Font& f, Rectangle<float> r) {
            text (g, s, f, Colours::white.withAlpha (0.18f), r.translated (0, 1), Justification::centredTop);
            text (g, s, f, Colours::black.withAlpha (0.25f), r.translated (0, -1), Justification::centredTop);
            text (g, s, f, C::ink, r, Justification::centredTop);
        };
        emboss ("TSR", f36, { o.x, o.y + 0.f, 88.f, 44.f });
        emboss ("AUDIO", f18, { o.x, o.y + 38.f, 88.f, 22.f });
        auto bar = [&] (float x, float y, float w, float h, float r) {
            Rectangle<float> rr (o.x + x, o.y + y, w, h);
            g.setColour (C::ink); g.fillRoundedRectangle (rr, r);
            g.setGradientFill (ColourGradient (Colours::black.withAlpha (0.5f), 0, rr.getY(), Colours::transparentBlack, 0, rr.getY() + 1.2f, false));
            g.fillRoundedRectangle (rr, r);
        };
        bar (95.f, 6.f, 7.f, 50.f, 3.5f);
        { Rectangle<float> cap (o.x + 88.f, o.y + 28.f, 21.f, 9.f);
          g.setGradientFill (ColourGradient (C::btn0, 0, cap.getY(), C::btn1, 0, cap.getBottom(), false)); g.fillRoundedRectangle (cap, 4.5f);
          g.setColour (C::edge); g.drawRoundedRectangle (cap.reduced (0.75f), 4.5f, 1.5f); }
        const float tops[8] = { 8.f, 14.5f, 21.f, 27.5f, 34.f, 40.5f, 47.f, 53.5f }; const float ws[8] = { 10, 6, 6, 10, 6, 6, 10, 6 };
        for (int i = 0; i < 8; ++i) bar (115.f, tops[i], ws[i], 1.8f, 0.f);
        text (g, "dB", f8, C::ink, { o.x + 127.f, o.y + 3.f, 18.f, 10.f }, Justification::centredTop);
        text (g, "dB", f8, C::ink, { o.x + 127.f, o.y + 50.f, 18.f, 10.f }, Justification::centredTop);
    }

    void display (Graphics& g, Rectangle<float> r)
    {
        g.setColour (Colours::white.withAlpha (0.10f)); g.drawRoundedRectangle (r.expanded (2.f).translated (0, 1.f), 13.f, 1.f);
        g.setColour (C::dispEdge); g.fillRoundedRectangle (r.expanded (1.f), 12.f);
        g.setColour (C::disp); g.fillRoundedRectangle (r, 11.f);
        Graphics::ScopedSaveState ss (g);
        Path p; p.addRoundedRectangle (r, 11.f); g.reduceClipRegion (p);
        g.setGradientFill (ColourGradient (Colours::black.withAlpha (0.85f), 0, r.getY(), Colours::transparentBlack, 0, r.getY() + 13.f, false)); g.fillRect (r);
    }
    void glass (Graphics& g, Rectangle<float> r)
    {
        Graphics::ScopedSaveState ss (g);
        Path p; p.addRoundedRectangle (r, 11.f); g.reduceClipRegion (p);
        ColourGradient gg = cssLinear (r, 160.f, Colours::white.withAlpha (0.9f * 0.1f), Colours::transparentWhite);
        gg.addColour (0.28, Colours::white.withAlpha (0.15f * 0.1f)); gg.addColour (0.40, Colours::transparentWhite);
        g.setGradientFill (gg); g.fillRect (r);
    }

    // ---- left display: integrity

    // ---- left display: vectorscope (input grey, TSR output orange) + correlation meter
    void paintScope (Graphics& g)
    {
        auto r = scopeR;
        const bool byp = bypass.getToggleState();
        text (g, "STEREO IMAGE", fonts().get (500, 11.f, 0.6f), C::dispInk2, { r.getX() + 16.f, r.getY() + 14.f, 160.f, 13.f });
        auto f9 = fonts().get (400, 9.f);
        text (g, "IN", f9, withA (C::dispInk, 0.6f), { r.getRight() - 64.f, r.getY() + 14.f, 20.f, 13.f });
        text (g, "TSR", f9, C::accent, { r.getRight() - 40.f, r.getY() + 14.f, 30.f, 13.f });
        const Point<float> c (r.getCentreX(), r.getY() + 112.f);
        const float R = 76.f;
        // axes: M vertical, L/R diagonals, unit circle
        g.setColour (Colours::white.withAlpha (0.07f));
        g.drawEllipse (c.x - R, c.y - R, 2 * R, 2 * R, 1.f);
        g.drawLine (c.x, c.y - R, c.x, c.y + R, 1.f); g.drawLine (c.x - R, c.y, c.x + R, c.y, 1.f);
        const float d = R * 0.7071f;
        g.drawLine (c.x - d, c.y - d, c.x + d, c.y + d, 1.f); g.drawLine (c.x + d, c.y - d, c.x - d, c.y + d, 1.f);
        auto f8 = fonts().get (600, 8.5f, 1.f);
        text (g, "M", f8, C::dispInk2, { c.x - 6.f, c.y - R - 13.f, 12.f, 10.f }, Justification::centred);
        text (g, "L", f8, C::dispInk2, { c.x - d - 14.f, c.y - d - 12.f, 10.f, 10.f }, Justification::centred);
        text (g, "R", f8, C::dispInk2, { c.x + d + 4.f, c.y - d - 12.f, 10.f, 10.f }, Justification::centred);
        if (live) {
            Graphics::ScopedSaveState ss (g);
            g.reduceClipRegion (Rectangle<float> (c.x - R - 4.f, c.y - R - 4.f, 2 * R + 8.f, 2 * R + 8.f).toNearestInt());
            const float k = R * 0.92f / scopeScale;
            g.setColour (withA (C::dispInk, 0.22f));
            for (auto& q : ptIn) g.fillRect (c.x - q.x * k - 0.6f, c.y - q.y * k - 0.6f, 1.2f, 1.2f);
            if (! byp) {
                g.setColour (withA (C::accent, 0.6f));
                for (auto& q : ptOut) g.fillRect (c.x - q.x * k - 0.7f, c.y - q.y * k - 0.7f, 1.4f, 1.4f);
            }
        }
        // correlation meter  -1 .. +1
        Rectangle<float> bar (r.getX() + 30.f, r.getY() + 212.f, r.getWidth() - 60.f, 4.f);
        g.setColour (Colours::white.withAlpha (0.08f)); g.fillRoundedRectangle (bar, 2.f);
        g.setColour (Colours::white.withAlpha (0.25f)); g.fillRect (bar.getCentreX() - 0.5f, bar.getY() - 3.f, 1.f, 10.f);
        auto xAt = [&] (float v) { return bar.getX() + bar.getWidth() * (0.5f + 0.5f * jlimit (-1.f, 1.f, v)); };
        if (live) {
            const float co = byp ? corrIn : corrOut;
            const float x0 = bar.getCentreX(), x1 = xAt (co);
            g.setColour (C::accent);
            g.fillRoundedRectangle (Rectangle<float> (jmin (x0, x1), bar.getY(), std::abs (x1 - x0), bar.getHeight()), 2.f);
            g.setColour (C::dispInk2); g.fillRect (xAt (corrIn) - 1.f, bar.getY() - 2.f, 2.f, 8.f);
        }
        auto f9b = fonts().get (600, 9.5f);
        text (g, kMinus + "1", f9b, C::dispInk2, { r.getX() + 8.f, r.getY() + 206.f, 20.f, 14.f }, Justification::centredRight);
        text (g, "+1", f9b, C::dispInk2, { bar.getRight() + 4.f, r.getY() + 206.f, 20.f, 14.f });
        text (g, "CORRELAZIONE", fonts().get (500, 10.f, 0.6f), C::dispInk2, { r.getX() + 16.f, r.getY() + 222.f, 120.f, 14.f });
        text (g, live ? sgn2 (byp ? corrIn : corrOut) : String ("--"), fonts().get (600, 12.f), C::dispInk, { r.getRight() - 96.f, r.getY() + 222.f, 80.f, 14.f }, Justification::centredRight);
    }

    // ---- right display: big values + per-band correlation
    void paintValuesDisplay (Graphics& g)
    {
        auto r = valuesR;
        const bool byp = bypass.getToggleState(), show = live && ! byp;
        // header row
        auto f9 = fonts().get (400, 9.f);
        text (g, "SPATIAL INTEGRITY", fonts().get (500, 11.f, 0.6f), C::dispInk2, { r.getX() + 16.f, r.getY() + 16.f, 160.f, 13.f });
        Rectangle<float> led (r.getX() + 150.f, r.getY() + 19.5f, 6.f, 6.f);
        if (show) { g.setColour (withA (C::accent, 0.35f)); g.fillEllipse (led.expanded (3.f)); g.setColour (C::accent); } else g.setColour (Colour (0xff2a323c));
        g.fillEllipse (led);
        text (g, "ANALYSIS LIVE", f9, C::dispInk2, { r.getX() + 163.f, r.getY() + 16.f, 90.f, 13.f });
        // four value columns
        struct Col { const char* t; String v, sub; Colour c; float px; };
        const String dash ("--");
        Col cols[4] = {
            { "INTEGRIT\xc3\x80", show ? String (roundToInt (integOut)) : dash, live ? "prima " + String (roundToInt (integIn)) : String(), C::dispInk, 64.f },
            { "MONO LOSS", show ? dB (mlOut) : dash, live ? "prima " + dB (mlIn) : String(), C::dispInk, 40.f },
            { "RITARDO CORRETTO", show ? String (std::abs (appliedMs), 2) + " ms" : dash, live ? "rilevato " + String (std::abs (lagMs), 2) + " ms" : String(), C::dispInk, 40.f },
            { "MONO SAFETY", show ? String (roundToInt (reduction * 100)) + "%" : dash, String ("riduzione movimento"), C::dispInk, 40.f } };
        const float colW = (r.getWidth() - 32.f) / 4.f;
        for (int i = 0; i < 4; ++i) {
            float x = r.getX() + 16.f + i * colW;
            if (i) { g.setColour (Colours::white.withAlpha (0.06f)); g.fillRect (x - 12.f, r.getY() + 44.f, 1.f, 90.f); }
            text (g, U (cols[i].t), fonts().get (700, 10.f, 1.2f), C::dispInk2, { x, r.getY() + 44.f, colW - 16.f, 12.f });
            text (g, cols[i].v, fonts().get (400, cols[i].px, cols[i].px > 50.f ? -2.f : -1.f), cols[i].c, { x, r.getY() + 58.f, colW - 16.f, 60.f });
            text (g, cols[i].sub, fonts().get (400, 12.f), C::dispInk2, { x, r.getY() + 118.f, colW - 16.f, 16.f });
        }
        // status
        String st;
        if (byp) st = "Bypass" + kDot + "segnale originale";
        else if (deltaOn && live) st = U ("DELTA: ascolti solo ci\xc3\xb2 che il plugin aggiunge o toglie");
        else if (! live) st = "In attesa di segnale";
        else if (reduction > 0.10f && (corrIn < 0.5f || mlIn < -1.3f)) st = U ("Stereo gi\xc3\xa0 largo \xc2\xb7 mono safety trattiene WIDTH/MOTION");
        else if (reduction > 0.10f) st = "Mono safety: movimento ridotto del " + String (roundToInt (reduction * 100)) + "%";
        else if (std::abs (appliedMs) > 0.02f) st = "Fase allineata" + kDot + String (std::abs (appliedMs), 2) + " ms";
        else if (integOut < 60.f) st = "Campo instabile" + kDot + "correlazione bassa";
        else st = U ("Centro stabile \xc2\xb7 profondit\xc3\xa0 leggibile");
        text (g, st, fonts().get (400, 12.5f), C::dispInk, { r.getX() + 16.f, r.getY() + 142.f, r.getWidth() - 32.f, 20.f });
        // per-band correlation  -1 .. +1  (input marker, TSR fill)
        const char* bn[3] = { "LOW", "MID", "HIGH" };
        const float bx = r.getX() + 64.f, bw = 420.f;
        for (int b = 0; b < 3; ++b) {
            const float y = r.getY() + 176.f + b * 20.f;
            text (g, bn[b], fonts().get (700, 9.5f, 1.f), C::dispInk2, { r.getX() + 16.f, y - 6.f, 44.f, 12.f });
            Rectangle<float> bar (bx, y - 2.f, bw, 4.f);
            g.setColour (Colours::white.withAlpha (0.08f)); g.fillRoundedRectangle (bar, 2.f);
            g.setColour (Colours::white.withAlpha (0.25f)); g.fillRect (bar.getCentreX() - 0.5f, y - 5.f, 1.f, 10.f);
            if (! live) continue;
            auto xAt = [&] (float v) { return bar.getX() + bar.getWidth() * (0.5f + 0.5f * jlimit (-1.f, 1.f, v)); };
            const float v = byp ? bandIn[b] : bandOut[b];
            const float x0 = bar.getCentreX(), x1 = xAt (v);
            g.setColour (C::accent);
            g.fillRoundedRectangle (Rectangle<float> (jmin (x0, x1), bar.getY(), std::abs (x1 - x0), bar.getHeight()), 2.f);
            g.setColour (C::dispInk2); g.fillRect (xAt (bandIn[b]) - 1.f, y - 4.f, 2.f, 8.f);
            text (g, sgn2 (v), fonts().get (600, 10.f), C::dispInk, { bx + bw + 8.f, y - 7.f, 44.f, 14.f });
        }
        text (g, U ("CORRELAZIONE PER BANDA \xc2\xb7 marker = prima"), f9, C::dispInk2, { bx + bw + 60.f, r.getY() + 170.f, 200.f, 12.f });
    }

    // ---- knob values (LCD for COHERENCE, text under the small knobs)
    void paintKnobValues (Graphics& g)
    {
        auto pct = [] (double v) { return String (roundToInt ((float) v)) + "%"; };
        text (g, pct (knobs[0]->getValue()), fonts().get (600, 18.f), C::dispInk, { 103.f, 522.f, 80.f, 30.f }, Justification::centred);
        for (int i = 1; i < 9; ++i)
            text (g, pct (knobs[i]->getValue()), fonts().get (600, 14.f), C::ink, { smallLeft (i - 1), 492.f, 115.f, 17.f }, Justification::centred);
    }

    void paintInfo (Graphics& g)
    {
        Rectangle<float> r (1240.f - 120.f - 360.f, 70.f, 360.f, 0.f);
        AttributedString as;
        auto f = fonts().get (400, 12.f), fb = fonts().get (700, 12.f);
        as.append ("COME SI USA\n", fb, C::dispInk);
        as.append (U ("1. Inserisci il plugin su una traccia stereo: overhead, coppie microfoniche, room, synth stereo.\n"
                      "2. PHASE: COHERENCE allinea il ritardo tra L e R e la fase delle basse. SOLVE SPACE imposta valori sicuri.\n"
                      "3. DEPTH aggiunge riflessioni e profondit\xc3\xa0; EARLY/LATE ne sceglie il carattere; DEPTH GUARD limita il riverbero.\n"
                      "4. WIDTH e MOTION allargano e muovono l'immagine; MONO SAFETY li riduce se il mono peggiora; CENTER LOCK tiene fermo il centro.\n"
                      "VOCAL / DRUMS / MUSIC adattano il comportamento al materiale. DELTA (nel display) fa ascoltare solo la differenza; BYPASS = confronto."), f, C::dispInk);
        as.setLineSpacing (12.f * 0.45f);
        TextLayout tl; tl.createLayout (as, 360.f - 28.f);
        r.setHeight (tl.getHeight() + 24.f);
        DropShadow (Colours::black.withAlpha (0.5f), 20, { 0, 8 }).drawForRectangle (g, r.toNearestInt());
        g.setColour (C::disp); g.fillRoundedRectangle (r, 8.f);
        g.setColour (C::dispEdge); g.drawRoundedRectangle (r.reduced (0.5f), 8.f, 1.f);
        tl.draw (g, r.reduced (14.f, 12.f));
    }
};

} // namespace tsrui

// ------------------------------------------------------------------ editor
TSREditor::TSREditor (TSRProcessor& p) : AudioProcessorEditor (&p), proc (p)
{
    panel = std::make_unique<tsrui::Panel> (p);
    addAndMakeVisible (*panel);
    setResizable (true, true);
    getConstrainer()->setFixedAspectRatio ((double) baseW / baseH);
    setResizeLimits (baseW / 2, baseH / 2, baseW * 2, baseH * 2);
    setSize (baseW, baseH);
    startTimerHz (30);
}
TSREditor::~TSREditor() { stopTimer(); }
void TSREditor::paint (juce::Graphics& g) { g.fillAll (juce::Colour (0xff0b0d0f)); }
void TSREditor::resized()
{
    float s = (float) getWidth() / (float) baseW;
    panel->setBounds (0, 0, baseW, baseH);
    panel->setTransform (juce::AffineTransform::scale (s));
}
void TSREditor::timerCallback() { panel->pullMeters(); }
