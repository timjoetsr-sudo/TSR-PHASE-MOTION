cmake_minimum_required(VERSION 3.22)
project(TSR_PHASE_DEPTH_MOTION VERSION 1.0.4)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
if(APPLE)
  set(CMAKE_OSX_DEPLOYMENT_TARGET "10.13" CACHE STRING "")
endif()

set(JUCE_DIR "${CMAKE_CURRENT_SOURCE_DIR}/JUCE" CACHE PATH "Path to JUCE")
add_subdirectory(${JUCE_DIR} JUCE)

if(APPLE)
  set(TSR_FORMATS AU VST3 Standalone)
else()
  set(TSR_FORMATS VST3 Standalone)
endif()

juce_add_plugin(TSRPhaseDepthMotion
  COMPANY_NAME "TSR Audio"
  PRODUCT_NAME "TSR PHASE DEPTH MOTION"
  BUNDLE_ID "com.treestudiorecord.tsrphasedepthmotion"
  VERSION ${PROJECT_VERSION}
  PLUGIN_MANUFACTURER_CODE Tsra
  PLUGIN_CODE Tpdm
  FORMATS ${TSR_FORMATS}
  IS_SYNTH FALSE
  NEEDS_MIDI_INPUT FALSE
  NEEDS_MIDI_OUTPUT FALSE
  IS_MIDI_EFFECT FALSE
  EDITOR_WANTS_KEYBOARD_FOCUS FALSE
  COPY_PLUGIN_AFTER_BUILD FALSE
  VST3_CATEGORIES Fx Spatial
  AU_MAIN_TYPE kAudioUnitType_Effect)

juce_add_binary_data(TSRPDMBinary SOURCES
  Resources/Barlow400.ttf Resources/Barlow500.ttf Resources/Barlow600.ttf Resources/Barlow700.ttf Resources/Inter700.ttf)

set(TSR_SOURCES Source/PluginProcessor.cpp Source/PluginEditor.cpp)
target_sources(TSRPhaseDepthMotion PRIVATE ${TSR_SOURCES})

set(TSR_DEFS JUCE_WEB_BROWSER=0 JUCE_USE_CURL=0 JUCE_VST3_CAN_REPLACE_VST2=0 JUCE_DISPLAY_SPLASH_SCREEN=0)
if(UNIX AND NOT APPLE)
  list(APPEND TSR_DEFS JUCE_ALSA=0 JUCE_JACK=0)
endif()
target_compile_definitions(TSRPhaseDepthMotion PUBLIC ${TSR_DEFS})

target_link_libraries(TSRPhaseDepthMotion PRIVATE TSRPDMBinary juce::juce_audio_utils juce::juce_dsp
  PUBLIC juce::juce_recommended_config_flags juce::juce_recommended_lto_flags juce::juce_recommended_warning_flags)

# ---- engine tests (pure C++, no JUCE): ./EngineTests  -> "RESULT: N passed, 0 failed"
add_executable(EngineTests tests/engine_tests.cpp)
# ---- CPU benchmark 1.0.2 (tests/ref) vs current engine, same binary: ./EngineBench
add_executable(EngineBench tests/bench.cpp)
# ---- DSP quality harness (headless): ./build/tests/tsr_dsp_tests > tsr_dsp_metrics.csv
add_executable(tsr_dsp_tests tests/dsp_metrics.cpp)
set_target_properties(tsr_dsp_tests PROPERTIES RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/tests)

# ---- host test tool (Linux/macOS): loads the BUILT VST3 through JUCE hosting and tests it
option(TSR_BUILD_HOSTTESTS "Build VST3 host test tool" OFF)
if(TSR_BUILD_HOSTTESTS)
  juce_add_console_app(HostTests PRODUCT_NAME "HostTests")
  target_sources(HostTests PRIVATE tests/host_tests.cpp)
  juce_generate_juce_header(HostTests)
  target_compile_definitions(HostTests PRIVATE JUCE_PLUGINHOST_VST3=1 JUCE_WEB_BROWSER=0 JUCE_USE_CURL=0 JUCE_ALSA=0 JUCE_JACK=0)
  target_link_libraries(HostTests PRIVATE juce::juce_audio_utils juce::juce_recommended_config_flags)
  set_target_properties(HostTests PROPERTIES ENABLE_EXPORTS ON)
endif()
