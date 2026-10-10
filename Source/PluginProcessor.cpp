#include "PluginProcessor.h"
#include "PluginEditor.h"

using APF = juce::AudioParameterFloat;

juce::AudioProcessorValueTreeState::ParameterLayout TSRProcessor::createLayout()
{
    juce::AudioProcessorValueTreeState::ParameterLayout l;
    auto pct = [] (const juce::String& id, const juce::String& name, float max, float def) {
        return std::make_unique<APF> (juce::ParameterID { id, 1 }, name, juce::NormalisableRange<float> (0.f, max, 0.1f), def,
                                      juce::AudioParameterFloatAttributes().withLabel ("%"));
    };
    l.add (pct (ids::coherence, "Coherence", 100.f, 86.f));
    l.add (pct (ids::depth, "Depth", 100.f, 58.f));
    l.add (pct (ids::earlyLate, "Early/Late", 100.f, 42.f));
    l.add (pct (ids::width, "Width", 200.f, 112.f));
    l.add (pct (ids::motion, "Motion", 100.f, 34.f));
    l.add (pct (ids::mix, "Mix", 100.f, 100.f));
    l.add (pct (ids::monoSafety, "Mono Safety", 100.f, 94.f));
    l.add (pct (ids::centerLock, "Center Lock", 100.f, 89.f));
    l.add (pct (ids::depthGuard, "Depth Guard", 100.f, 83.f));
    l.add (std::make_unique<juce::AudioParameterChoice> (juce::ParameterID { ids::profile, 1 }, "Profile",
                                                         juce::StringArray { "VOCAL", "DRUMS", "MUSIC" }, 2));
    l.add (std::make_unique<juce::AudioParameterBool> (juce::ParameterID { ids::delta, 1 }, "Delta", false));
    l.add (std::make_unique<juce::AudioParameterBool> (juce::ParameterID { ids::bypass, 1 }, "Bypass", false));
    return l;
}

TSRProcessor::TSRProcessor()
    : AudioProcessor (BusesProperties().withInput ("Input", juce::AudioChannelSet::stereo(), true)
                                       .withOutput ("Output", juce::AudioChannelSet::stereo(), true)),
      apvts (*this, nullptr, "TSRPDM", createLayout())
{
    const juce::String order[] = { ids::coherence, ids::depth, ids::earlyLate, ids::width, ids::motion, ids::mix,
                                   ids::monoSafety, ids::centerLock, ids::depthGuard, ids::profile, ids::delta, ids::bypass };
    for (int i = 0; i < 12; ++i) p[i] = apvts.getRawParameterValue (order[i]);
    bypassParam = apvts.getParameter (ids::bypass);
    for (int i = 0; i < Scope::N; ++i) { sc.inM[i] = 0.f; sc.inS[i] = 0.f; sc.outM[i] = 0.f; sc.outS[i] = 0.f; }
}

bool TSRProcessor::isBusesLayoutSupported (const BusesLayout& layouts) const
{
    const auto in = layouts.getMainInputChannelSet();
    return (in == juce::AudioChannelSet::stereo() || in == juce::AudioChannelSet::mono())
        && layouts.getMainOutputChannelSet() == juce::AudioChannelSet::stereo();
}

void TSRProcessor::prepareToPlay (double sampleRate, int samplesPerBlock)
{
    engine.prepare (sampleRate, samplesPerBlock);
    setLatencySamples (engine.latencySamples());
}

tsr::Params TSRProcessor::readParams() const
{
    tsr::Params q;
    q.coherence = p[0]->load() * 0.01f; q.depth = p[1]->load() * 0.01f; q.earlyLate = p[2]->load() * 0.01f;
    q.width = p[3]->load() * 0.01f; q.motion = p[4]->load() * 0.01f; q.mix = p[5]->load() * 0.01f;
    q.monoSafety = p[6]->load() * 0.01f; q.centerLock = p[7]->load() * 0.01f; q.depthGuard = p[8]->load() * 0.01f;
    { const int pr = (int) p[9]->load(); q.mode = pr == 0 ? tsr::MODE_VOCAL : pr == 1 ? tsr::MODE_DRUMS : tsr::MODE_MUSIC; } q.monitor = p[10]->load() > 0.5f ? tsr::MON_DELTA : tsr::MON_TSR; q.bypass = p[11]->load() > 0.5f;
    return q;
}

void TSRProcessor::runEngine (juce::AudioBuffer<float>& buffer, bool forceBypass)
{
    juce::ScopedNoDenormals noDenormals;
    const int nIn = getTotalNumInputChannels();
    if (nIn == 1 && buffer.getNumChannels() >= 2)       // mono -> stereo: duplicate input
        buffer.copyFrom (1, 0, buffer, 0, 0, buffer.getNumSamples());
    for (int ch = juce::jmax (nIn, 2); ch < getTotalNumOutputChannels(); ++ch)
        buffer.clear (ch, 0, buffer.getNumSamples());
    if (buffer.getNumChannels() < 2) return;

    auto q = readParams();
    if (forceBypass) q.bypass = true;
    if (auto* ph = getPlayHead())
        if (auto pos = ph->getPosition())
            if (auto bpm = pos->getBpm()) q.bpm = *bpm;

    float* L = buffer.getWritePointer (0); float* R = buffer.getWritePointer (1); const int n = buffer.getNumSamples();
    // scope: same ring positions for input (before) and output (after)
    const int w0 = sc.w.load (std::memory_order_relaxed), ph0 = scPhase;
    int w = w0, ph = ph0;
    for (int i = 0; i < n; ++i)
        if (++ph >= 2) { ph = 0; const int k = (w++) & (Scope::N - 1);
                         sc.inM[k].store (0.5f * (L[i] + R[i]), std::memory_order_relaxed); sc.inS[k].store (0.5f * (L[i] - R[i]), std::memory_order_relaxed); }
    engine.process (L, R, n, q);
    w = w0; ph = ph0;
    for (int i = 0; i < n; ++i)
        if (++ph >= 2) { ph = 0; const int k = (w++) & (Scope::N - 1);
                         sc.outM[k].store (0.5f * (L[i] + R[i]), std::memory_order_relaxed); sc.outS[k].store (0.5f * (L[i] - R[i]), std::memory_order_relaxed); }
    scPhase = ph; sc.w.store (w, std::memory_order_release);
}

void TSRProcessor::processBlock (juce::AudioBuffer<float>& buffer, juce::MidiBuffer&) { runEngine (buffer, false); }
void TSRProcessor::processBlockBypassed (juce::AudioBuffer<float>& buffer, juce::MidiBuffer&) { runEngine (buffer, true); }

bool TSRProcessor::solveSpace()
{
    auto q = readParams();
    auto s = engine.solveSpace (q);
    if (! s.changed) return false;
    auto set = [this] (const juce::String& id, float valuePct) {
        if (auto* prm = apvts.getParameter (id)) {
            prm->beginChangeGesture();
            prm->setValueNotifyingHost (prm->convertTo0to1 (valuePct));
            prm->endChangeGesture();
        }
    };
    set (ids::coherence, s.coherence * 100.f);
    set (ids::width, s.width * 100.f);
    set (ids::motion, s.motion * 100.f);
    return true;
}

void TSRProcessor::getStateInformation (juce::MemoryBlock& destData)
{
    if (auto xml = apvts.copyState().createXml()) copyXmlToBinary (*xml, destData);
}

void TSRProcessor::setStateInformation (const void* data, int sizeInBytes)
{
    if (auto xml = getXmlFromBinary (data, sizeInBytes))
        if (xml->hasTagName (apvts.state.getType()))
            apvts.replaceState (juce::ValueTree::fromXml (*xml));
}

juce::AudioProcessorEditor* TSRProcessor::createEditor() { return new TSREditor (*this); }

juce::AudioProcessor* JUCE_CALLTYPE createPluginFilter() { return new TSRProcessor(); }
