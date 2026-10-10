#pragma once
#include <juce_audio_processors/juce_audio_processors.h>
#include "PluginProcessor.h"

// UI: style "04 — Canna di fucile brunita blu" (TSR AUDIO hardware family)
namespace tsrui {

struct Fonts {
    juce::Typeface::Ptr w400, w500, w600, w700, inter700;
    Fonts();
    juce::Font get (int weight, float px, float trackingPx = 0.f) const;   // tracking in px, as CSS letter-spacing
    juce::Font inter (float px, float trackingPx = 0.f) const;
};
const Fonts& fonts();

class LookAndFeel : public juce::LookAndFeel_V4 {
public:
    void drawRotarySlider (juce::Graphics&, int x, int y, int w, int h, float pos, float a0, float a1, juce::Slider&) override;
};

// Row of "role" buttons (72 x 24, r 8, lit underline when on)
class RoleRow : public juce::Component {
public:
    RoleRow (juce::StringArray labels, std::function<bool (int)> isOn, std::function<void (int)> click);
    void paint (juce::Graphics&) override;
    void mouseDown (const juce::MouseEvent&) override;
    static constexpr float bw = 72.f, bh = 24.f, gap = 8.f;
    float preferredWidth() const { return labels.size() * bw + (labels.size() - 1) * gap; }
    int numButtons() const { return labels.size(); }
private:
    juce::StringArray labels; std::function<bool (int)> isOn; std::function<void (int)> click;
};

// Header hardware buttons: BYPASS (toggle), INFO (icon box), SOLVE SPACE
class HwButton : public juce::Button {
public:
    enum Kind { Bypass, Info, Solve };
    explicit HwButton (Kind k) : juce::Button ({}), kind (k) {}
    void paintButton (juce::Graphics&, bool over, bool down) override;
    Kind getKind() const { return kind; }
    bool flash = false;
private:
    Kind kind;
};

class Panel;
} // namespace tsrui

class TSREditor : public juce::AudioProcessorEditor, private juce::Timer
{
public:
    explicit TSREditor (TSRProcessor&);
    ~TSREditor() override;
    void paint (juce::Graphics&) override;
    void resized() override;
    static constexpr int baseW = 1240, baseH = 552;
private:
    void timerCallback() override;
    TSRProcessor& proc;
    std::unique_ptr<tsrui::Panel> panel;
    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (TSREditor)
};
