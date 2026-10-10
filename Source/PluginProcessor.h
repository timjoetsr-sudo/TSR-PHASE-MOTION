#pragma once
#include <juce_audio_processors/juce_audio_processors.h>
#include "dsp/SpatialEngine.h"

namespace ids {
    inline const juce::String coherence { "coherence" }, depth { "depth" }, earlyLate { "earlyLate" }, width { "width" },
        motion { "motion" }, mix { "mix" }, monoSafety { "monoSafety" }, centerLock { "centerLock" }, depthGuard { "depthGuard" },
        profile { "profile" }, delta { "delta" }, bypass { "bypass" };
}

class TSRProcessor : public juce::AudioProcessor
{
public:
    TSRProcessor();
    ~TSRProcessor() override = default;

    void prepareToPlay (double sampleRate, int samplesPerBlock) override;
    void releaseResources() override {}
    bool isBusesLayoutSupported (const BusesLayout& layouts) const override;
    void processBlock (juce::AudioBuffer<float>&, juce::MidiBuffer&) override;
    void processBlockBypassed (juce::AudioBuffer<float>&, juce::MidiBuffer&) override;

    juce::AudioProcessorEditor* createEditor() override;
    bool hasEditor() const override { return true; }

    const juce::String getName() const override { return JucePlugin_Name; }
    bool acceptsMidi() const override { return false; }
    bool producesMidi() const override { return false; }
    bool isMidiEffect() const override { return false; }
    double getTailLengthSeconds() const override { return 2.0; }

    int getNumPrograms() override { return 1; }
    int getCurrentProgram() override { return 0; }
    void setCurrentProgram (int) override {}
    const juce::String getProgramName (int) override { return "Default"; }
    void changeProgramName (int, const juce::String&) override {}

    void getStateInformation (juce::MemoryBlock& destData) override;
    void setStateInformation (const void* data, int sizeInBytes) override;

    juce::AudioProcessorParameter* getBypassParameter() const override { return bypassParam; }

    juce::AudioProcessorValueTreeState apvts;
    const tsr::Meters& meters() const { return engine.meters; }

    // Message thread: apply SOLVE SPACE recommendation. Returns true if anything changed.
    bool solveSpace();

    // Vectorscope feed (audio thread writes, UI reads): mid/side of input and output, every 2nd sample, lock-free ring
    struct Scope {
        static constexpr int N = 2048;
        std::atomic<float> inM[N], inS[N], outM[N], outS[N];
        std::atomic<int> w { 0 };
    };
    const Scope& scope() const { return sc; }

    static juce::AudioProcessorValueTreeState::ParameterLayout createLayout();

private:
    tsr::Params readParams() const;
    void runEngine (juce::AudioBuffer<float>&, bool forceBypass);

    tsr::SpatialEngine engine;
    juce::AudioProcessorParameter* bypassParam = nullptr;
    std::atomic<float>* p[12] {};
    Scope sc; int scPhase = 0;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (TSRProcessor)
};
