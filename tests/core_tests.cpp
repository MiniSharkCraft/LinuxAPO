#include "Engine.h"
#include "BiQuad.h"
#include "LoudnessVolumeProvider.h"
#ifdef SKYAPO_TEST_CLAP
#include "CLAPPluginHost.h"
#include "IPluginParameterControl.h"
#include <dlfcn.h>
#endif
#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <iterator>
#include <limits>
#include <thread>
#include <sndfile.h>
#include <sys/stat.h>
#include <unistd.h>
#include <utility>
#include <vector>

namespace fs = std::filesystem;
bool write(const fs::path &path, const std::string &contents) {
  std::ofstream out(path);
  out << contents;
  return bool(out);
}
bool setTestPluginPath(const char *name, const char *value) {
  if (setenv(name, value, 1) == 0)
    return true;
  std::cerr << "cannot set fixture-only plugin path " << name << '\n';
  return false;
}
int main() {
#ifdef SKYAPO_TEST_LV2
  if (!setTestPluginPath("LV2_PATH", SKYAPO_TEST_LV2_PATH))
    return 1;
#endif
#ifdef SKYAPO_TEST_CLAP
  if (!setTestPluginPath("CLAP_PATH", SKYAPO_TEST_CLAP_PATH) ||
      !setTestPluginPath("SKYAPO_CLAP_PATHS_ONLY", "1"))
    return 1;
#endif
#ifdef SKYAPO_TEST_VST3
  if (!setTestPluginPath("VST3_PATH", SKYAPO_TEST_VST3_PATH) ||
      !setTestPluginPath("SKYAPO_VST3_PATHS_ONLY", "1"))
    return 1;
#endif

  const std::string path =
      "/tmp/skyapo-test-" + std::to_string(getpid()) + ".txt";
  if (!write(path, "Preamp: -6 dB\n"))
    return 1;
  Engine e(48000, 2, 128);
  e.loadConfig(path);
  if (e.filterDescriptions().size() != 1 ||
      e.filterDescriptions()[0].find("Preamp") == std::string::npos ||
      e.filterDescriptions()[0].find(":1") == std::string::npos) {
    std::cerr << "active filter source-line description mismatch\n";
    return 1;
  }
  float data[8] = {1, 1, .5f, .5f, -1, -1, .25f, .25f};
  e.process(data, 4);
  const float gain = std::pow(10.0f, -6.0f / 20.0f);
  const float expected[8] = {gain,  gain,  .5f * gain,  .5f * gain,
                             -gain, -gain, .25f * gain, .25f * gain};
  for (int i = 0; i < 8; ++i)
    if (std::abs(data[i] - expected[i]) > 1e-5f) {
      std::cerr << "preamp amplitude mismatch at " << i << ": " << data[i]
                << '\n';
      return 1;
    }

  for (const auto &invalid : {
           std::string("Preamp: 10000 dB\n"),
           std::string("Filter: ON PK Fc 30000 Hz Gain 6 dB Q 1\n"),
           std::string("Filter: ON PK Fc 1000 Hz Gain 10000 dB Q 1\n"),
           std::string("Filter: ON PK Fc 1000 Hz Gain 6 dB Q -1\n"),
       }) {
    if (!write(path, invalid))
      return 1;
    bool actionable = false;
    try {
      Engine invalidNumeric(48000, 2, 128);
      invalidNumeric.loadConfig(path);
    } catch (const std::exception &ex) {
      const std::string message = ex.what();
      actionable =
          message.find(path + ":1:") != std::string::npos &&
          (message.find("finite float audio range") != std::string::npos ||
           message.find("Nyquist") != std::string::npos ||
           message.find("finite and in range") != std::string::npos);
      if (!actionable)
        std::cerr << "numeric range diagnostic was not actionable: " << message
                  << '\n';
    }
    if (!actionable) {
      std::cerr << "out-of-range DSP parameter was not diagnosed\n";
      return 1;
    }
  }

  if (!write(path, "# Windows endpoint-volume dependent upstream filter\n"
                   "LoudnessCorrection: State 1 ReferenceLevel 0 "
                   "ReferenceOffset 0 Attenuation 1.0\n"))
    return 1;
  bool loudnessSemanticsDiagnosed = false;
  try {
    Engine unsupportedLoudness(48000, 2, 128);
    unsupportedLoudness.loadConfig(path);
  } catch (const std::exception &ex) {
    const std::string message = ex.what();
    loudnessSemanticsDiagnosed =
        message.find(path + ":2:") != std::string::npos &&
        message.find("LoudnessCorrection") != std::string::npos &&
        message.find("live PipeWire endpoint-volume snapshot") !=
            std::string::npos;
  }
  if (!loudnessSemanticsDiagnosed) {
    std::cerr << "LoudnessCorrection did not explain its missing live volume "
                 "snapshot\n";
    return 1;
  }

  skyapo::platform::LoudnessVolumeProvider::publish(true, 12.0f);
  if (!write(path, "LoudnessCorrection: State 1 ReferenceLevel 0 "
                   "ReferenceOffset 0 Attenuation 1.0\n"))
    return 1;
  {
    Engine loudness(48000, 2, 1024, {L"L", L"R"});
    loudness.loadConfig(path);
    if (loudness.filterCount() != 1) {
      std::cerr << "upstream LoudnessCorrection filter was not loaded\n";
      return 1;
    }
    std::vector<float> tone(1024 * 2);
    for (unsigned i = 0; i < 1024; ++i) {
      const float sample =
          0.2f * std::sin(2.0 * 3.141592653589793 * 100.0 * i / 48000.0);
      tone[2 * i] = tone[2 * i + 1] = sample;
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(30));
    double inputRms = 0.0;
    double outputRms = 0.0;
    for (unsigned block = 0; block < 12; ++block) {
      loudness.process(tone.data(), 1024);
      if (block >= 8)
        for (float sample : tone)
          outputRms += static_cast<double>(sample) * sample;
      for (unsigned i = 0; i < 1024; ++i) {
        const double sample =
            0.2 * std::sin(2.0 * 3.141592653589793 * 100.0 * i / 48000.0);
        if (block >= 8)
          inputRms += 2.0 * sample * sample;
      }
      for (unsigned i = 0; i < 1024; ++i) {
        const float sample =
            0.2f * std::sin(2.0 * 3.141592653589793 * 100.0 * i / 48000.0);
        tone[2 * i] = tone[2 * i + 1] = sample;
      }
    }
    const double ratio = std::sqrt(outputRms / inputRms);
    std::cout << "LoudnessCorrection upstream 100 Hz amplitude ratio: " << ratio
              << '\n';
    if (!(ratio > 0.4 && ratio < 0.9)) {
      std::cerr << "upstream LoudnessCorrection produced unexpected 100 Hz "
                   "amplitude ratio: "
                << ratio << '\n';
      return 1;
    }
  }
  skyapo::platform::LoudnessVolumeProvider::publish(false, 0.0f);

#ifdef SKYAPO_TEST_LV2
  if (!write(path, "Plugin: LV2 https://skyapo.example/plugins/test-gain\n"))
    return 1;
  Engine plugin(48000, 2, 128, {L"L", L"R"});
  plugin.loadConfig(path);
  float pluginBlock[8] = {.2f, -.4f, .6f, -.8f, 1.0f, -1.0f, .5f, -.5f};
  const float pluginExpected[8] = {.1f, -.2f, .3f,  -.4f,
                                   .5f, -.5f, .25f, -.25f};
  plugin.process(pluginBlock, 4);
  for (unsigned i = 0; i < 8; ++i)
    if (std::abs(pluginBlock[i] - pluginExpected[i]) > 1e-5f) {
      std::cerr << "LV2 test plugin output mismatch at " << i << '\n';
      return 1;
    }
  plugin.setPluginBypass("https://skyapo.example/plugins/test-gain", true);
  float lv2Bypassed[8] = {.2f, -.4f, .6f, -.8f, 1.0f, -1.0f, .5f, -.5f};
  const float lv2Dry[8] = {.2f, -.4f, .6f, -.8f, 1.0f, -1.0f, .5f, -.5f};
  plugin.process(lv2Bypassed, 4);
  for (unsigned i = 0; i < 8; ++i)
    if (std::abs(lv2Bypassed[i] - lv2Dry[i]) > 1e-6f) {
      std::cerr << "LV2 host bypass did not preserve dry samples\n";
      return 1;
    }
  plugin.setPluginBypass("https://skyapo.example/plugins/test-gain", false);
  float lv2Reenabled[8] = {.2f, -.4f, .6f, -.8f, 1.0f, -1.0f, .5f, -.5f};
  plugin.process(lv2Reenabled, 4);
  for (unsigned i = 0; i < 8; ++i)
    if (std::abs(lv2Reenabled[i] - pluginExpected[i]) > 1e-5f) {
      std::cerr << "LV2 host unbypass did not resume plugin processing\n";
      return 1;
    }

  if (!write(
          path,
          "Plugin: LV2 https://skyapo.example/plugins/test-gain gain=0.25\n"))
    return 1;
  Engine overridden(48000, 2, 128, {L"L", L"R"});
  overridden.loadConfig(path);
  float overriddenBlock[8] = {.2f, -.4f, .6f, -.8f, 1.0f, -1.0f, .5f, -.5f};
  overridden.process(overriddenBlock, 4);
  for (unsigned i = 0; i < 8; ++i)
    if (std::abs(overriddenBlock[i] - pluginBlock[i] * 0.5f) > 1e-5f) {
      std::cerr << "LV2 control override output mismatch at " << i << '\n';
      return 1;
    }

  for (const auto &invalid : {
           "Plugin: LV2 https://skyapo.example/plugins/test-gain missing=0.5\n",
           "Plugin: LV2 https://skyapo.example/plugins/test-gain gain=2\n",
       }) {
    if (!write(path, invalid))
      return 1;
    bool rejectedOverride = false;
    try {
      Engine invalidPlugin(48000, 2, 128, {L"L", L"R"});
      invalidPlugin.loadConfig(path);
    } catch (const std::exception &) {
      rejectedOverride = true;
    }
    if (!rejectedOverride) {
      std::cerr << "invalid LV2 parameter override was accepted\n";
      return 1;
    }
  }
#endif

#ifdef SKYAPO_TEST_VST2
  const std::string vst2Config =
      "Plugin: VST2 \"" SKYAPO_TEST_VST2_PATH "\" 0=0.25\n";
  if (!write(path, vst2Config))
    return 1;
  Engine vst2Plugin(48000, 2, 128, {L"L", L"R"});
  vst2Plugin.loadConfig(path);
  float vst2Samples[8] = {.2f, -.4f, .6f, -.8f, 1.0f, -1.0f, .5f, -.5f};
  const float vst2Expected[8] = {.05f, -.1f,  .15f,  -.2f,
                                 .25f, -.25f, .125f, -.125f};
  vst2Plugin.process(vst2Samples, 4);
  for (unsigned i = 0; i < 8; ++i)
    if (std::abs(vst2Samples[i] - vst2Expected[i]) > 1e-6f) {
      std::cerr << "VST2 config plugin parameter/output mismatch at " << i
                << '\n';
      return 1;
    }
  vst2Plugin.setPluginBypass(SKYAPO_TEST_VST2_PATH, true);
  float vst2Dry[8] = {.2f, -.4f, .6f, -.8f, 1.0f, -1.0f, .5f, -.5f};
  vst2Plugin.process(vst2Dry, 4);
  for (unsigned i = 0; i < 8; ++i)
    if (std::abs(vst2Dry[i] - vst2Expected[i] * 4.0f) > 1e-6f) {
      std::cerr << "VST2 host bypass did not preserve dry samples at " << i
                << '\n';
      return 1;
    }
  vst2Plugin.setPluginBypass(SKYAPO_TEST_VST2_PATH, false);
  float vst2Reenabled[8] = {.2f, -.4f, .6f, -.8f, 1.0f, -1.0f, .5f, -.5f};
  vst2Plugin.process(vst2Reenabled, 4);
  for (unsigned i = 0; i < 8; ++i)
    if (std::abs(vst2Reenabled[i] - vst2Expected[i]) > 1e-6f) {
      std::cerr << "VST2 plugin did not resume after host bypass at " << i
                << '\n';
      return 1;
    }
  // Multiple control writes before one process block coalesce to the latest
  // value. Both numeric index and unique parameter-name lookup are supported.
  vst2Plugin.setPluginParameter(SKYAPO_TEST_VST2_PATH, "0", 0.75f);
  float vst2Live[8] = {.2f, -.4f, .6f, -.8f, 1.0f, -1.0f, .5f, -.5f};
  const float vst2LiveExpected[8] = {.15f, -.3f,  .45f,  -.6f,
                                     .75f, -.75f, .375f, -.375f};
  vst2Plugin.process(vst2Live, 4);
  for (unsigned i = 0; i < 8; ++i)
    if (std::abs(vst2Live[i] - vst2LiveExpected[i]) > 1e-6f) {
      std::cerr << "VST2 live numeric parameter update mismatch at " << i
                << '\n';
      return 1;
    }
  vst2Plugin.setPluginParameter(SKYAPO_TEST_VST2_PATH, "0", 0.5f);
  vst2Plugin.setPluginParameter(SKYAPO_TEST_VST2_PATH, "Gain", 0.25f);
  float vst2Latest[8] = {.2f, -.4f, .6f, -.8f, 1.0f, -1.0f, .5f, -.5f};
  const float vst2LatestExpected[8] = {.05f, -.1f,  .15f,  -.2f,
                                       .25f, -.25f, .125f, -.125f};
  vst2Plugin.process(vst2Latest, 4);
  for (unsigned i = 0; i < 8; ++i)
    if (std::abs(vst2Latest[i] - vst2LatestExpected[i]) > 1e-6f) {
      std::cerr << "VST2 latest live parameter update mismatch at " << i
                << '\n';
      return 1;
    }
  for (const auto &invalid :
       {std::pair<const char *, float>{"missing", 0.5f},
        {"0", -0.01f},
        {"0", 1.01f},
        {"Gain", std::numeric_limits<float>::quiet_NaN()}}) {
    bool rejected = false;
    try {
      vst2Plugin.setPluginParameter(SKYAPO_TEST_VST2_PATH, invalid.first,
                                    invalid.second);
    } catch (const std::exception &) {
      rejected = true;
    }
    if (!rejected) {
      std::cerr << "invalid VST2 live parameter was accepted: " << invalid.first
                << '\n';
      return 1;
    }
  }
  if (!write(path, std::string("Plugin: VST2 \"") + SKYAPO_TEST_VST2_MONO_PATH +
                       "\"\n"))
    return 1;
  Engine vst2Mono(48000, 1, 128, {L"C"});
  vst2Mono.loadConfig(path);
  float vst2MonoSamples[4] = {.2f, -.4f, .6f, -.8f};
  vst2Mono.process(vst2MonoSamples, 4);
  const float vst2MonoExpected[4] = {.1f, -.2f, .3f, -.4f};
  for (unsigned i = 0; i < 4; ++i)
    if (std::abs(vst2MonoSamples[i] - vst2MonoExpected[i]) > 1e-6f) {
      std::cerr << "VST2 mono config processing mismatch at " << i << '\n';
      return 1;
    }
  for (const auto *invalid :
       {"Plugin: VST2 /not/a/plugin.so\n",
        "Plugin: VST2 \"" SKYAPO_TEST_VST2_PATH "\" 1=0.25\n",
        "Plugin: VST2 \"" SKYAPO_TEST_VST2_PATH "\" 0=1.25\n"}) {
    if (!write(path, invalid))
      return 1;
    bool rejected = false;
    try {
      Engine invalidVst2(48000, 2, 128, {L"L", L"R"});
      invalidVst2.loadConfig(path);
    } catch (const std::exception &) {
      rejected = true;
    }
    if (!rejected) {
      std::cerr << "invalid VST2 module/parameter was accepted\n";
      return 1;
    }
  }
#endif

#ifdef SKYAPO_TEST_VST3
  const std::string vst3Uid = "534B5941504F00010000000000000001";
  if (!write(path, "Plugin: VST3 " + vst3Uid + "\n"))
    return 1;
  Engine vst3Plugin(48000, 2, 128, {L"L", L"R"});
  vst3Plugin.loadConfig(path);
  float vst3Block[8] = {.2f, -.4f, .6f, -.8f, 1.0f, -1.0f, .5f, -.5f};
  const float vst3Expected[8] = {.1f, -.2f, .3f, -.4f, .5f, -.5f, .25f, -.25f};
  vst3Plugin.process(vst3Block, 4);
  for (unsigned i = 0; i < 8; ++i)
    if (std::abs(vst3Block[i] - vst3Expected[i]) > 1e-5f) {
      std::cerr << "VST3 fixture default gain mismatch at " << i << '\n';
      return 1;
    }
  const auto checkVst3GainBlock = [&](float gain, const char *context) {
    float block[8] = {.2f, -.4f, .6f, -.8f, 1.0f, -1.0f, .5f, -.5f};
    vst3Plugin.process(block, 4);
    for (unsigned i = 0; i < 8; ++i) {
      const float expected = vst3Expected[i] * (gain / 0.5f);
      if (std::abs(block[i] - expected) > 1e-5f) {
        std::cerr << "VST3 " << context << " gain mismatch at " << i
                  << ": expected " << expected << ", got " << block[i] << '\n';
        return false;
      }
    }
    return true;
  };
  if (!checkVst3GainBlock(0.5f, "second-block default"))
    return 1;
  vst3Plugin.setPluginParameter(vst3Uid, "7", 0.75f);
  if (!checkVst3GainBlock(0.75f, "live update") ||
      !checkVst3GainBlock(0.75f, "persistent following-block"))
    return 1;
  vst3Plugin.setPluginParameter(vst3Uid, "7", 0.25f);
  if (!checkVst3GainBlock(0.25f, "second live update"))
    return 1;
  vst3Plugin.setPluginBypass(vst3Uid, true);
  float vst3Dry[8] = {.2f, -.4f, .6f, -.8f, 1.0f, -1.0f, .5f, -.5f};
  const float vst3DryExpected[8] = {.2f,  -.4f,  .6f, -.8f,
                                    1.0f, -1.0f, .5f, -.5f};
  vst3Plugin.process(vst3Dry, 4);
  for (unsigned i = 0; i < 8; ++i)
    if (std::abs(vst3Dry[i] - vst3DryExpected[i]) > 1e-6f) {
      std::cerr << "VST3 host bypass did not preserve dry samples\n";
      return 1;
    }
  vst3Plugin.setPluginBypass(vst3Uid, false);
  if (!checkVst3GainBlock(0.25f, "unbypassed"))
    return 1;
  for (const auto &invalid : {std::pair<const char *, float>{"999", 0.5f},
                              {"bad", 0.5f},
                              {"7", -0.01f},
                              {"7", 1.01f},
                              {"7", std::numeric_limits<float>::quiet_NaN()}}) {
    bool rejected = false;
    try {
      vst3Plugin.setPluginParameter(vst3Uid, invalid.first, invalid.second);
    } catch (const std::exception &) {
      rejected = true;
    }
    if (!rejected) {
      std::cerr << "invalid VST3 live parameter was accepted: " << invalid.first
                << '\n';
      return 1;
    }
  }
  if (vst3Plugin.pluginLatencySamples() != 0) {
    std::cerr
        << "zero-latency VST3 fixture was reported with nonzero latency\n";
    return 1;
  }
  if (!write(path, "Plugin: VST3 534B5941504F00010000000000000003\n"))
    return 1;
  Engine vst3Latency(48000, 2, 128, {L"L", L"R"});
  vst3Latency.loadConfig(path);
  if (vst3Latency.pluginLatencySamples() != 64) {
    std::cerr << "VST3-reported latency was not included in Engine snapshot\n";
    return 1;
  }
  float delayInput[128]{};
  delayInput[0] = 0.25f;
  vst3Latency.process(delayInput, 64);
  if (std::any_of(std::begin(delayInput), std::end(delayInput),
                  [](float sample) { return std::abs(sample) > 1e-6f; })) {
    std::cerr << "VST3 latency fixture did not delay its first block\n";
    return 1;
  }
  float delayedOutput[2] = {0.5f, -0.5f};
  vst3Latency.process(delayedOutput, 1);
  if (std::abs(delayedOutput[0] - 0.25f) > 1e-6f ||
      std::abs(delayedOutput[1]) > 1e-6f) {
    std::cerr << "VST3 latency fixture sample offset mismatch\n";
    return 1;
  }
  if (!write(path, "Plugin: VST3 " + vst3Uid + " 7=0.25\n"))
    return 1;
  Engine vst3Override(48000, 2, 128, {L"L", L"R"});
  vst3Override.loadConfig(path);
  float vst3OverrideBlock[8] = {.2f, -.4f, .6f, -.8f, 1.0f, -1.0f, .5f, -.5f};
  vst3Override.process(vst3OverrideBlock, 4);
  for (unsigned i = 0; i < 8; ++i)
    if (std::abs(vst3OverrideBlock[i] - .5f * vst3Expected[i]) > 1e-5f) {
      std::cerr << "VST3 parameter override mismatch at " << i << '\n';
      return 1;
    }
  for (const auto *invalid : {
           "Plugin: VST3 534B5941504F00010000000000000001 999=0.25\n",
           "Plugin: VST3 534B5941504F00010000000000000001 7=1.25\n",
           "Plugin: VST3 534B5941504F00010000000000000001 bad=0.25\n",
       }) {
    if (!write(path, invalid))
      return 1;
    bool rejected = false;
    try {
      Engine invalidVst3(48000, 2, 128, {L"L", L"R"});
      invalidVst3.loadConfig(path);
    } catch (const std::exception &) {
      rejected = true;
    }
    if (!rejected) {
      std::cerr << "invalid VST3 parameter override was accepted\n";
      return 1;
    }
  }
#endif

#ifdef SKYAPO_TEST_CLAP
  if (!write(path, "Plugin: CLAP org.skyapo.test.gain\n"))
    return 1;
  Engine clapPlugin(48000, 2, 128, {L"L", L"R"});
  clapPlugin.loadConfig(path);
  if (clapPlugin.pluginLatencySamples() != 0) {
    std::cerr << "zero-latency CLAP extension was not reported accurately\n";
    return 1;
  }
  float clapBlock[8] = {.2f, -.4f, .6f, -.8f, 1.0f, -1.0f, .5f, -.5f};
  const float clapExpected[8] = {.1f, -.2f, .3f, -.4f, .5f, -.5f, .25f, -.25f};
  clapPlugin.process(clapBlock, 4);
  for (unsigned i = 0; i < 8; ++i)
    if (std::abs(clapBlock[i] - clapExpected[i]) > 1e-5f) {
      std::cerr << "CLAP test plugin output mismatch at " << i << '\n';
      return 1;
    }

  if (!write(path, "Plugin: CLAP org.skyapo.test.latency\n"))
    return 1;
  Engine clapLatency(48000, 2, 128, {L"L", L"R"});
  clapLatency.loadConfig(path);
  if (clapLatency.pluginLatencySamples() != 64) {
    std::cerr << "CLAP-reported latency was not included in Engine snapshot\n";
    return 1;
  }
  float clapDelayInput[128]{};
  clapDelayInput[0] = 0.25f;
  clapLatency.process(clapDelayInput, 64);
  if (std::any_of(std::begin(clapDelayInput), std::end(clapDelayInput),
                  [](float sample) { return std::abs(sample) > 1e-6f; })) {
    std::cerr << "CLAP latency fixture did not delay its first block\n";
    return 1;
  }
  float clapDelayedOutput[2] = {0.5f, -0.5f};
  clapLatency.process(clapDelayedOutput, 1);
  if (std::abs(clapDelayedOutput[0] - 0.125f) > 1e-6f ||
      std::abs(clapDelayedOutput[1]) > 1e-6f) {
    std::cerr << "CLAP latency fixture sample offset mismatch\n";
    return 1;
  }
  if (!write(path, "Plugin: CLAP org.skyapo.test.latency\n"
                   "Plugin: CLAP org.skyapo.test.latency\n"))
    return 1;
  Engine clapLatencySum(48000, 2, 128, {L"L", L"R"});
  clapLatencySum.loadConfig(path);
  if (clapLatencySum.pluginLatencySamples() != 128) {
    std::cerr << "serial plugin latency snapshots were not summed\n";
    return 1;
  }
  float clapSumInput[256]{};
  clapSumInput[0] = 0.25f;
  clapLatencySum.process(clapSumInput, 128);
  if (std::any_of(std::begin(clapSumInput), std::end(clapSumInput),
                  [](float sample) { return std::abs(sample) > 1e-6f; })) {
    std::cerr << "serial CLAP latency fixtures emitted audio too early\n";
    return 1;
  }
  float clapSumOutput[2] = {0.5f, -0.5f};
  clapLatencySum.process(clapSumOutput, 1);
  if (std::abs(clapSumOutput[0] - 0.0625f) > 1e-6f ||
      std::abs(clapSumOutput[1]) > 1e-6f) {
    std::cerr << "serial CLAP latency fixture output mismatch\n";
    return 1;
  }

  if (!write(path, "Plugin: CLAP org.skyapo.test.gain Gain=0.25\n"))
    return 1;
  Engine clapOverride(48000, 2, 128, {L"L", L"R"});
  clapOverride.loadConfig(path);
  float clapOverriddenBlock[8] = {.2f, -.4f, .6f, -.8f, 1.0f, -1.0f, .5f, -.5f};
  const float overriddenExpected[8] = {.05f, -.1f,  .15f,  -.2f,
                                       .25f, -.25f, .125f, -.125f};
  clapOverride.process(clapOverriddenBlock, 4);
  for (unsigned i = 0; i < 8; ++i)
    if (std::abs(clapOverriddenBlock[i] - overriddenExpected[i]) > 1e-5f) {
      std::cerr << "CLAP parameter override output mismatch at " << i << '\n';
      return 1;
    }
  clapOverride.setPluginBypass("org.skyapo.test.gain", true);
  float clapDry[8] = {.2f, -.4f, .6f, -.8f, 1.0f, -1.0f, .5f, -.5f};
  const float clapDryExpected[8] = {.2f,  -.4f,  .6f, -.8f,
                                    1.0f, -1.0f, .5f, -.5f};
  clapOverride.process(clapDry, 4);
  for (unsigned i = 0; i < 8; ++i)
    if (std::abs(clapDry[i] - clapDryExpected[i]) > 1e-6f) {
      std::cerr << "CLAP host bypass did not preserve dry samples\n";
      return 1;
    }
  clapOverride.setPluginBypass("org.skyapo.test.gain", false);
  float clapWetAgain[8] = {.2f, -.4f, .6f, -.8f, 1.0f, -1.0f, .5f, -.5f};
  clapOverride.process(clapWetAgain, 4);
  for (unsigned i = 0; i < 8; ++i)
    if (std::abs(clapWetAgain[i] - overriddenExpected[i]) > 1e-5f) {
      std::cerr << "CLAP host unbypass did not resume plugin processing\n";
      return 1;
    }

  // Exercise the format-neutral live control API directly while the actual
  // CLAP instance is processing. Its mailbox and event storage are allocated
  // before this first process call.
  CLAPPluginHost clapHost;
  auto clapInstance = clapHost.create("org.skyapo.test.gain", 48000, 128,
                                      {L"L", L"R"}, {{"Gain", 0.25f}});
  auto *clapControl =
      dynamic_cast<IPluginParameterControl *>(clapInstance.get());
  if (!clapControl ||
      clapControl->pluginIdentifier() != "org.skyapo.test.gain") {
    std::cerr << "CLAP live parameter control interface is unavailable\n";
    return 1;
  }
  float liveInputLeft[4] = {1, 1, 1, 1};
  float liveInputRight[4] = {1, 1, 1, 1};
  float liveOutputLeft[4]{};
  float liveOutputRight[4]{};
  float *liveInput[2] = {liveInputLeft, liveInputRight};
  float *liveOutput[2] = {liveOutputLeft, liveOutputRight};
  clapInstance->process(liveOutput, liveInput, 4);
  if (std::abs(liveOutputLeft[0] - 0.25f) > 1e-6f) {
    std::cerr << "CLAP static parameter override was not preserved\n";
    return 1;
  }
  clapControl->setParameterValue("7", 0.75f);
  std::fill(std::begin(liveOutputLeft), std::end(liveOutputLeft), 0.0f);
  std::fill(std::begin(liveOutputRight), std::end(liveOutputRight), 0.0f);
  clapInstance->process(liveOutput, liveInput, 4);
  if (std::abs(liveOutputLeft[0] - 0.75f) > 1e-6f ||
      std::abs(liveOutputRight[3] - 0.75f) > 1e-6f) {
    std::cerr << "live CLAP parameter update did not change processed audio\n";
    return 1;
  }

  // Persist a real CLAP plugin state through its extension, verify the exact
  // fixture payload bytes, then restore it into a fresh Engine and measure the
  // resulting audio. Corrupt state must reject the candidate graph without
  // replacing the already active graph; a missing sidecar means plugin default.
  char stateHomeTemplate[] = "/tmp/skyapo-clap-state-XXXXXX";
  char *stateHomeRaw = mkdtemp(stateHomeTemplate);
  if (!stateHomeRaw) {
    std::cerr << "cannot create isolated CLAP state test directory\n";
    return 1;
  }
  const fs::path stateHome(stateHomeRaw);
  const char *previousStateHomeValue = std::getenv("XDG_STATE_HOME");
  const std::string previousStateHome =
      previousStateHomeValue ? previousStateHomeValue : "";
  const bool hadPreviousStateHome = previousStateHomeValue != nullptr;
  if (!setTestPluginPath("XDG_STATE_HOME", stateHome.string().c_str()))
    return 1;
  const fs::path stateConfig = stateHome / "state-chain.txt";
  const fs::path stateStore = stateHome / "skyapo" / "clap-state";
  if (!write(stateConfig, "Plugin: CLAP org.skyapo.test.gain\n"))
    return 1;
  void *clapTestLibrary = dlopen(
      (fs::path(SKYAPO_TEST_CLAP_PATH) / "skyapo-test-clap.clap").c_str(),
      RTLD_NOW | RTLD_LOCAL);
  using DestroyCount = uint32_t (*)();
  auto destroyedCount = reinterpret_cast<DestroyCount>(
      clapTestLibrary ? dlsym(clapTestLibrary,
                              "skyapo_test_clap_destroyed_plugin_count")
                      : nullptr);
  if (!destroyedCount) {
    std::cerr << "cannot inspect CLAP fixture cleanup counter\n";
    return 1;
  }
  {
    Engine stateSource(48000, 2, 128, {L"L", L"R"});
    stateSource.loadConfig(stateConfig.string());
    stateSource.setPluginParameter("org.skyapo.test.gain", "7", 0.75f);
    float stateInput[8] = {1, 1, 1, 1, 1, 1, 1, 1};
    stateSource.process(stateInput, 4);
    stateSource.setPluginParameter("org.skyapo.test.gain", "7", 0.875f);
    if (std::abs(stateInput[0] - 0.75f) > 1e-6f ||
        stateSource.savePersistentPluginStates() != 1) {
      std::cerr << "CLAP state save did not retain the active gain\n";
      return 1;
    }
  }
  std::vector<fs::path> sidecars;
  for (const auto &entry : fs::directory_iterator(stateStore))
    if (entry.is_regular_file())
      sidecars.push_back(entry.path());
  if (sidecars.size() != 1) {
    std::cerr << "expected exactly one CLAP state sidecar\n";
    return 1;
  }
  struct stat stateStat {};
  if (stat(sidecars.front().c_str(), &stateStat) < 0 ||
      (stateStat.st_mode & 0077) != 0) {
    std::cerr << "CLAP state sidecar is not private (mode 0600)\n";
    return 1;
  }
  struct stat stateDirStat {};
  if (stat(stateStore.c_str(), &stateDirStat) < 0 ||
      (stateDirStat.st_mode & 0077) != 0) {
    std::cerr << "CLAP state directory is not private (mode 0700)\n";
    return 1;
  }
  std::ifstream stateFile(sidecars.front(), std::ios::binary);
  std::vector<uint8_t> stateBytes((std::istreambuf_iterator<char>(stateFile)),
                                  std::istreambuf_iterator<char>());
  const auto canonicalStateConfig = fs::weakly_canonical(stateConfig).string();
  const std::string expectedIdentity =
      canonicalStateConfig + "\n1\norg.skyapo.test.gain";
  const size_t payloadOffset = 36 + expectedIdentity.size();
  unsigned char expectedPayload[20]{};
  std::memcpy(expectedPayload, "SKYGAIN1", 8);
  const double expectedSavedGain = 0.875;
  std::memcpy(expectedPayload + 8, &expectedSavedGain,
              sizeof(expectedSavedGain));
  expectedPayload[16] = 0x53;
  expectedPayload[17] = 0x4b;
  expectedPayload[18] = 0x59;
  expectedPayload[19] = 0x31;
  if (stateBytes.size() != payloadOffset + sizeof(expectedPayload) ||
      std::string(stateBytes.begin() + 36,
                  stateBytes.begin() + payloadOffset) != expectedIdentity ||
      !std::equal(expectedPayload, expectedPayload + sizeof(expectedPayload),
                  stateBytes.begin() + payloadOffset)) {
    std::cerr << "CLAP state envelope/payload bytes did not round-trip\n";
    return 1;
  }
  Engine stateRestored(48000, 2, 128, {L"L", L"R"});
  stateRestored.loadConfig(stateConfig.string());
  float restoredBlock[8] = {1, 1, 1, 1, 1, 1, 1, 1};
  stateRestored.process(restoredBlock, 4);
  if (std::abs(restoredBlock[0] - 0.875f) > 1e-6f ||
      std::abs(restoredBlock[7] - 0.875f) > 1e-6f) {
    std::cerr << "CLAP state restore did not numerically recover plugin gain\n";
    return 1;
  }
  {
    std::fstream corrupt(sidecars.front(),
                         std::ios::binary | std::ios::in | std::ios::out);
    corrupt.seekp(-1, std::ios::end);
    char damaged = '\0';
    corrupt.write(&damaged, 1);
  }
  bool corruptRejected = false;
  const auto destroyedBeforeCorruptLoad = destroyedCount();
  try {
    stateRestored.loadConfig(stateConfig.string());
  } catch (const std::exception &error) {
    corruptRejected = std::string(error.what()).find("checksum") !=
                      std::string::npos;
  }
  float retainedBlock[8] = {1, 1, 1, 1, 1, 1, 1, 1};
  stateRestored.process(retainedBlock, 4);
  if (!corruptRejected || std::abs(retainedBlock[0] - 0.875f) > 1e-6f) {
    std::cerr << "corrupt CLAP state did not reject candidate/retain graph\n";
    return 1;
  }
  if (destroyedCount() != destroyedBeforeCorruptLoad + 1) {
    std::cerr << "corrupt-state candidate did not destroy its initialized CLAP plugin\n";
    return 1;
  }
  auto pluginRejectedBytes = stateBytes;
  pluginRejectedBytes[payloadOffset + 16] ^= 0x01;
  uint64_t checksum = 14695981039346656037ull;
  const auto hashByte = [&checksum](uint8_t byte) {
    checksum ^= byte;
    checksum *= 1099511628211ull;
  };
  for (const unsigned char byte : expectedIdentity)
    hashByte(byte);
  hashByte(0xff);
  for (size_t i = payloadOffset; i < pluginRejectedBytes.size(); ++i)
    hashByte(pluginRejectedBytes[i]);
  for (unsigned i = 0; i < 8; ++i)
    pluginRejectedBytes[28 + i] =
        static_cast<uint8_t>(checksum >> (i * 8));
  {
    std::ofstream replacement(sidecars.front(), std::ios::binary | std::ios::trunc);
    replacement.write(reinterpret_cast<const char *>(pluginRejectedBytes.data()),
                      pluginRejectedBytes.size());
  }
  bool pluginLoadRejected = false;
  const auto destroyedBeforePluginLoad = destroyedCount();
  try {
    stateRestored.loadConfig(stateConfig.string());
  } catch (const std::exception &error) {
    pluginLoadRejected = std::string(error.what()).find(
                             "plugin rejected saved state") !=
                         std::string::npos;
  }
  std::fill(std::begin(retainedBlock), std::end(retainedBlock), 1.0f);
  stateRestored.process(retainedBlock, 4);
  if (!pluginLoadRejected || std::abs(retainedBlock[0] - 0.875f) > 1e-6f) {
    std::cerr << "CLAP load-false did not reject candidate/retain graph\n";
    return 1;
  }
  if (destroyedCount() != destroyedBeforePluginLoad + 1) {
    std::cerr << "load-false candidate did not destroy its initialized CLAP plugin\n";
    return 1;
  }
  fs::remove(sidecars.front());
  Engine stateMissing(48000, 2, 128, {L"L", L"R"});
  stateMissing.loadConfig(stateConfig.string());
  float defaultState[8] = {1, 1, 1, 1, 1, 1, 1, 1};
  stateMissing.process(defaultState, 4);
  if (std::abs(defaultState[0] - 0.5f) > 1e-6f) {
    std::cerr << "missing CLAP state did not use plugin default\n";
    return 1;
  }
  fs::remove_all(stateHome);
  if (hadPreviousStateHome)
    setenv("XDG_STATE_HOME", previousStateHome.c_str(), 1);
  else
    unsetenv("XDG_STATE_HOME");
  dlclose(clapTestLibrary);

  for (const auto &invalid : std::vector<std::pair<std::string, float>>{
           {"missing", 0.5f}, {"Gain", 3.0f}, {"Gain", INFINITY}}) {
    bool rejectedLiveValue = false;
    try {
      clapControl->setParameterValue(invalid.first, invalid.second);
    } catch (const std::exception &) {
      rejectedLiveValue = true;
    }
    if (!rejectedLiveValue) {
      std::cerr << "invalid live CLAP parameter update was accepted\n";
      return 1;
    }
  }
  for (const auto *invalid : {
           "Plugin: CLAP org.skyapo.test.gain missing=0.25\n",
           "Plugin: CLAP org.skyapo.test.gain 7=3\n",
       }) {
    if (!write(path, invalid))
      return 1;
    bool rejectedOverride = false;
    try {
      Engine invalidClap(48000, 2, 128, {L"L", L"R"});
      invalidClap.loadConfig(path);
    } catch (const std::exception &) {
      rejectedOverride = true;
    }
    if (!rejectedOverride) {
      std::cerr << "invalid CLAP parameter override was accepted\n";
      return 1;
    }
  }
#endif

  if (!write(path, "Filter: ON PK Fc 1000 Hz Gain 6 dB Q 1.0\n"))
    return 1;
  Engine eq(48000, 2, 128);
  eq.loadConfig(path);
  float impulse[32] = {1.0f};
  eq.process(impulse, 16);
  bool changed = false;
  for (int i = 1; i < 16; ++i)
    changed |= std::abs(impulse[i]) > 1e-5f;
  if (!changed) {
    std::cerr << "upstream BiQuad did not produce a filter tail\n";
    return 1;
  }

  const auto measureToneGain = [&](double frequency) {
    constexpr int rate = 48000;
    constexpr int totalFrames = rate;
    constexpr int settleFrames = 12000;
    if (!write(path, "Filter: ON PK Fc 1000 Hz Gain 6 dB Q 1.0\n"))
      return std::pair<double, double>{-1.0, 0.0};
    Engine measured(rate, 2, 256);
    measured.loadConfig(path);
    if (measured.filterCount() != 1)
      return std::pair<double, double>{-1.0, 0.0};
    BiQuad upstreamReference(BiQuad::PEAKING, 6.0, 1000.0, rate, 1.0, false);
    std::vector<float> tone(static_cast<size_t>(totalFrames) * 2);
    std::vector<float> referenceOutput(static_cast<size_t>(totalFrames) * 2);
    constexpr double twoPi = 6.28318530717958647692;
    for (int frame = 0; frame < totalFrames; ++frame) {
      const float sample =
          static_cast<float>(std::sin(twoPi * frequency * frame / rate));
      tone[2 * frame] = sample;
      tone[2 * frame + 1] = sample;
      const float expected =
          static_cast<float>(upstreamReference.process(sample));
      referenceOutput[2 * frame] = expected;
      referenceOutput[2 * frame + 1] = expected;
    }
    constexpr unsigned blocks[] = {1, 17, 128, 256};
    unsigned blockIndex = 0;
    for (int frame = 0; frame < totalFrames;) {
      const unsigned count = std::min<unsigned>(
          blocks[blockIndex++ % std::size(blocks)], totalFrames - frame);
      measured.process(tone.data() + 2 * frame, count);
      for (unsigned i = 0; i < count; ++i)
        if (std::abs(tone[2 * (frame + i)] - referenceOutput[2 * (frame + i)]) >
            2e-6f) {
          std::cerr << "Engine BiQuad differs from upstream BiQuad at frame "
                    << frame + i << ", frequency " << frequency << " Hz\n";
          return std::pair<double, double>{-1.0, 0.0};
        }
      frame += count;
    }

    double inputEnergy = 0.0, outputEnergy = 0.0;
    for (int frame = settleFrames; frame < totalFrames; ++frame) {
      const double reference = std::sin(twoPi * frequency * frame / rate);
      inputEnergy += reference * reference;
      outputEnergy += static_cast<double>(tone[2 * frame]) * tone[2 * frame];
    }
    return std::pair<double, double>{std::sqrt(outputEnergy / inputEnergy),
                                     upstreamReference.gainAt(frequency, rate)};
  };
  const auto centerResponse = measureToneGain(1000.0);
  const auto remoteResponse = measureToneGain(100.0);
  const double expectedCenterGain = std::pow(10.0, 6.0 / 20.0);
  if (centerResponse.first < 0.0 || remoteResponse.first < 0.0 ||
      std::abs(centerResponse.second - 6.0) > 1e-6 ||
      std::abs(20.0 * std::log10(centerResponse.first) -
               centerResponse.second) > 0.02 ||
      std::abs(20.0 * std::log10(remoteResponse.first) -
               remoteResponse.second) > 0.02 ||
      std::abs(centerResponse.first - expectedCenterGain) > 0.01) {
    std::cerr << "upstream BiQuad response mismatch: center="
              << centerResponse.first << " (expected " << expectedCenterGain
              << "), 100Hz=" << remoteResponse.first << " (expected "
              << remoteResponse.second << " dB)\n";
    return 1;
  }

  if (!write(path, "NotACommand: 1\n"))
    return 1;
  bool rejected = false;
  try {
    Engine bad(48000, 2, 128);
    bad.loadConfig(path);
  } catch (const std::exception &) {
    rejected = true;
  }
  if (!rejected) {
    std::cerr << "unsupported config command was not rejected\n";
    return 1;
  }

  if (!write(path, "Filter: ON IIR Order 1 Coefficients 1 0 1 0\n"))
    return 1;
  Engine iir(48000, 2, 128);
  iir.loadConfig(path);
  float unity[6] = {.1f, -.2f, .3f, -.4f, .5f, -.6f};
  const float original[6] = {.1f, -.2f, .3f, -.4f, .5f, -.6f};
  iir.process(unity, 3);
  for (int i = 0; i < 6; ++i)
    if (std::abs(unity[i] - original[i]) > 1e-5f) {
      std::cerr << "IIR unity mismatch\n";
      return 1;
    }

  if (!write(path, "Delay: 1 Samples\n"))
    return 1;
  Engine delay(48000, 2, 128);
  delay.loadConfig(path);
  float samples[6] = {1, 10, 2, 20, 3, 30};
  delay.process(samples, 1);
  delay.process(samples + 2, 2);
  const float delayed[6] = {0, 0, 1, 10, 2, 20};
  for (int i = 0; i < 6; ++i)
    if (std::abs(samples[i] - delayed[i]) > 1e-5f) {
      std::cerr << "Delay mismatch at " << i << " got " << samples[i] << '\n';
      return 1;
    }

  if (!write(path, "Channel: L\nPreamp: -6 dB\n"))
    return 1;
  Engine selected(48000, 2, 128, {L"L", L"R"});
  selected.loadConfig(path);
  float selectedSamples[2] = {1.0f, 10.0f};
  selected.process(selectedSamples, 1);
  if (std::abs(selectedSamples[0] - gain) > 1e-5f ||
      std::abs(selectedSamples[1] - 10.0f) > 1e-5f) {
    std::cerr << "upstream Channel selection did not limit Preamp to L\n";
    return 1;
  }
  if (!write(path, "Copy: L=R R=L\n"))
    return 1;
  Engine copied(48000, 2, 128, {L"L", L"R"});
  copied.loadConfig(path);
  float copiedSamples[2] = {1.0f, 10.0f};
  copied.process(copiedSamples, 1);
  if (std::abs(copiedSamples[0] - 10.0f) > 1e-5f ||
      std::abs(copiedSamples[1] - 1.0f) > 1e-5f) {
    std::cerr << "upstream Copy channel swap mismatch\n";
    return 1;
  }
  if (!write(path, "Copy: L=R R=L\nChannel: C\nPreamp: -6 dB\n"))
    return 1;
  Engine routed(48000, 3, 128, {L"R", L"C", L"L"});
  routed.loadConfig(path);
  float routedSamples[6] = {10.0f, 100.0f, 1.0f, 20.0f, 200.0f, 2.0f};
  routed.process(routedSamples, 2);
  const float routedExpected[6] = {1.0f, 100.0f * gain, 10.0f,
                                   2.0f, 200.0f * gain, 20.0f};
  for (size_t sample = 0; sample < 6; ++sample)
    if (std::abs(routedSamples[sample] - routedExpected[sample]) > 1e-4f) {
      std::cerr << "upstream Copy/Channel routing mismatch at sample " << sample
                << ": got " << routedSamples[sample] << ", expected "
                << routedExpected[sample] << '\n';
      return 1;
    }
  if (!write(path, "Copy: L=unknown\n"))
    return 1;
  bool badCopyRejected = false;
  try {
    copied.loadConfig(path);
  } catch (const std::exception &ex) {
    badCopyRejected = std::string(ex.what()).find("unknown source channel") !=
                      std::string::npos;
  }
  if (!badCopyRejected) {
    std::cerr << "Copy with an unknown source channel was not rejected\n";
    return 1;
  }
  float retainedCopy[2] = {1.0f, 10.0f};
  copied.process(retainedCopy, 1);
  if (std::abs(retainedCopy[0] - 10.0f) > 1e-5f ||
      std::abs(retainedCopy[1] - 1.0f) > 1e-5f) {
    std::cerr << "failed Copy reload replaced the last valid graph\n";
    return 1;
  }

  char dirTemplate[] = "/tmp/skyapo-include-XXXXXX";
  char *dirName = mkdtemp(dirTemplate);
  if (!dirName)
    return 1;
  fs::path dir(dirName);
#ifdef SKYAPO_TEST_CONVOLUTION
  SF_INFO irInfo{};
  irInfo.samplerate = 48000;
  irInfo.channels = 1;
  irInfo.format = SF_FORMAT_WAV | SF_FORMAT_FLOAT;
  auto irPath = dir / "impulse.wav";
  SNDFILE *irFile = sf_open(irPath.c_str(), SFM_WRITE, &irInfo);
  if (!irFile)
    return 1;
  const float impulseResponse[3] = {0.5f, -0.25f, 0.125f};
  if (sf_writef_float(irFile, impulseResponse, 3) != 3) {
    sf_close(irFile);
    return 1;
  }
  sf_close(irFile);
  auto convolutionConfig = dir / "convolution.txt";
  if (!write(convolutionConfig, "Convolution: impulse.wav\n"))
    return 1;
  Engine convolution(48000, 2, 256);
  convolution.loadConfig(convolutionConfig.string());
  if (!convolution.requiresFixedBlock())
    return 1;
  bool rejectedVariableBlock = false;
  try {
    float rejectedBlock[510]{};
    convolution.process(rejectedBlock, 255);
  } catch (const std::exception &) {
    rejectedVariableBlock = true;
  }
  if (!rejectedVariableBlock)
    return 1;
  std::vector<float> convolutionInput(3 * 256 * 2);
  std::vector<float> convolutionExpected(convolutionInput.size());
  for (unsigned frame = 0; frame < 3 * 256; ++frame) {
    convolutionInput[2 * frame] =
        0.1f * static_cast<float>(static_cast<int>(frame % 19) - 9);
    convolutionInput[2 * frame + 1] =
        -0.07f * static_cast<float>(static_cast<int>(frame % 13) - 6);
    for (unsigned channel = 0; channel < 2; ++channel) {
      for (unsigned tap = 0; tap < 3 && tap <= frame; ++tap)
        convolutionExpected[2 * frame + channel] +=
            impulseResponse[tap] *
            convolutionInput[2 * (frame - tap) + channel];
    }
  }
  for (unsigned block = 0; block < 3; ++block) {
    float convBlock[512];
    std::copy_n(convolutionInput.data() + block * 512, 512, convBlock);
    convolution.process(convBlock, 256);
    for (unsigned sample = 0; sample < 512; ++sample) {
      const float expected = convolutionExpected[block * 512 + sample];
      if (std::abs(convBlock[sample] - expected) > 1e-4f) {
        std::cerr << "upstream Convolution FIR mismatch at block " << block
                  << ", sample " << sample << ": got " << convBlock[sample]
                  << ", expected " << expected << '\n';
        return 1;
      }
    }
  }
  auto graphicConfig = dir / "graphic-eq.txt";
  if (!write(graphicConfig, "GraphicEQ: 100 0 1000 0\n"))
    return 1;
  Engine graphic(48000, 2, 256);
  graphic.loadConfig(graphicConfig.string());
  float graphicBlock[512]{};
  graphicBlock[0] = 1.0f;
  graphicBlock[1] = -1.0f;
  graphic.process(graphicBlock, 256);
  float graphicEnergy = 0.0f;
  for (float sample : graphicBlock) {
    if (!std::isfinite(sample)) {
      std::cerr << "upstream GraphicEQ produced a non-finite sample\n";
      return 1;
    }
    graphicEnergy += sample * sample;
  }
  if (graphicEnergy < 0.1f) {
    std::cerr << "upstream GraphicEQ produced no output\n";
    return 1;
  }

  if (!write(graphicConfig, "GraphicEQ: 1000 -6\n"))
    return 1;
  Engine graphicGain(48000, 2, 256);
  graphicGain.loadConfig(graphicConfig.string());
  double graphicInputEnergy = 0.0;
  double graphicOutputEnergy = 0.0;
  for (unsigned block = 0; block < 64; ++block) {
    float tone[512]{};
    for (unsigned frame = 0; frame < 256; ++frame) {
      const float sample =
          static_cast<float>(0.2 * std::sin(2.0 * 3.141592653589793 * 1000.0 *
                                            (block * 256 + frame) / 48000.0));
      tone[2 * frame] = tone[2 * frame + 1] = sample;
      if (block >= 32)
        graphicInputEnergy += 2.0 * sample * sample;
    }
    graphicGain.process(tone, 256);
    if (block >= 32)
      for (float sample : tone)
        graphicOutputEnergy += static_cast<double>(sample) * sample;
  }
  const double graphicRatio =
      std::sqrt(graphicOutputEnergy / graphicInputEnergy);
  const double graphicGainDb = 20.0 * std::log10(graphicRatio);
  std::cout << "upstream GraphicEQ 1 kHz gain: " << graphicGainDb << " dB\n";
  if (std::abs(graphicGainDb + 6.0) > 0.2) {
    std::cerr << "upstream GraphicEQ 1 kHz gain mismatch: " << graphicGainDb
              << " dB (expected -6 dB)\n";
    return 1;
  }
#endif
  fs::create_directories(dir / "sub#dir");
  const auto root = dir / "root.txt";
  if (!write(root, "Preamp: -6 dB\nInclude: \"sub#dir/child # file.txt\"\n") ||
      !write(dir / "sub#dir/child # file.txt",
             "Preamp: 6 dB\nInclude: ../nested.txt\n") ||
      !write(dir / "nested.txt", "Preamp: 0 dB\n"))
    return 1;
  Engine included(48000, 2, 128);
  included.loadConfig(root.string());
  if (included.filterCount() != 3) {
    std::cerr << "nested Include did not preserve directive order/count\n";
    return 1;
  }
  const std::vector<fs::path> expectedConfigFiles = {
      fs::weakly_canonical(root),
      fs::weakly_canonical(dir / "sub#dir/child # file.txt"),
      fs::weakly_canonical(dir / "nested.txt")};
  if (included.configFiles() != expectedConfigFiles) {
    std::cerr << "Engine did not expose all active Include dependencies\n";
    return 1;
  }
  float includedSample[2] = {1, -1};
  included.process(includedSample, 1);
  if (std::abs(includedSample[0] - 1.0f) > 1e-5f ||
      std::abs(includedSample[1] + 1.0f) > 1e-5f) {
    std::cerr << "nested relative Include processing mismatch\n";
    return 1;
  }

  if (!write(dir / "cycle-a.txt", "Include: cycle-b.txt\n") ||
      !write(dir / "cycle-b.txt", "Include: cycle-a.txt\n"))
    return 1;
  bool cycleRejected = false;
  try {
    included.loadConfig((dir / "cycle-a.txt").string());
  } catch (const std::exception &ex) {
    cycleRejected =
        std::string(ex.what()).find("include cycle") != std::string::npos;
  }
  if (!cycleRejected) {
    std::cerr << "Include cycle was not diagnosed\n";
    return 1;
  }
  if (included.configFiles() != expectedConfigFiles) {
    std::cerr << "failed config changed the active Include dependencies\n";
    return 1;
  }
  float retained[2] = {1, -1};
  included.process(retained, 1);
  if (std::abs(retained[0] - 1.0f) > 1e-5f ||
      std::abs(retained[1] + 1.0f) > 1e-5f) {
    std::cerr << "failed config replaced the last valid filter graph\n";
    return 1;
  }

  if (!write(root, "Include: broken.txt\n") ||
      !write(dir / "broken.txt", "# comment\nUnsupported: true\n"))
    return 1;
  bool sourceLocationReported = false;
  try {
    included.loadConfig(root.string());
  } catch (const std::exception &ex) {
    sourceLocationReported =
        std::string(ex.what()).find((dir / "broken.txt").string() + ":2:") !=
        std::string::npos;
  }
  if (!sourceLocationReported) {
    std::cerr << "included-file error did not identify file and line\n";
    return 1;
  }

  const auto stageRoot = dir / "stage-root.txt";
  if (!write(stageRoot,
             "Stage: capture\nInclude: staged-child.txt\nPreamp: 6 dB\n") ||
      !write(dir / "staged-child.txt",
             "Stage: post-mix\nUnsupportedInThisStage: ignored\n"
             "Preamp: -60 dB\nStage: capture\nPreamp: -6 dB\n"))
    return 1;
  Engine staged(48000, 2, 128);
  staged.loadConfig(stageRoot.string());
  if (staged.filterCount() != 2) {
    std::cerr << "Stage filtering or Include-local stage scope mismatch\n";
    return 1;
  }
  float stageSample[2] = {1, -1};
  staged.process(stageSample, 1);
  if (std::abs(stageSample[0] - 1.0f) > 1e-5f ||
      std::abs(stageSample[1] + 1.0f) > 1e-5f) {
    std::cerr << "Include changed its parent Stage state\n";
    return 1;
  }

#ifdef SKYAPO_TEST_MUPARSER
#ifdef SKYAPO_TEST_MUPARSERX
#define SKYAPO_TEST_CONDITIONAL_ELSEIF                                         \
  "ElseIf: inputChannelCount == 2; sampleRate == 48000\n"
#else
#define SKYAPO_TEST_CONDITIONAL_ELSEIF                                         \
  "ElseIf: inputChannelCount == 2 && sampleRate == 48000\n"
#endif
  const auto conditional = dir / "conditional.txt";
  if (!write(conditional, "If: sampleRate < 0\n"
                          "If: invalid (\n"
                          "UnsupportedInsideFalseBranch: skipped\n"
                          "Include: missing-only-in-false-branch.txt\n"
                          "EndIf:\n" SKYAPO_TEST_CONDITIONAL_ELSEIF "If: 1\n"
                          "Preamp: -6 dB\n"
                          "Else:\n"
                          "UnsupportedInsideNestedElse: skipped\n"
                          "EndIf:\n"
                          "Else:\n"
                          "UnsupportedInsideElse: skipped\n"
                          "EndIf:\n"))
    return 1;
  Engine conditionalEngine(48000, 2, 128);
  conditionalEngine.loadConfig(conditional.string());
  float conditionalSamples[2] = {1.0f, -1.0f};
  conditionalEngine.process(conditionalSamples, 1);
  const float expectedConditionalGain = std::pow(10.0f, -6.0f / 20.0f);
  if (conditionalEngine.filterCount() != 1 ||
      std::abs(conditionalSamples[0] - expectedConditionalGain) > 1e-5f ||
      std::abs(conditionalSamples[1] + expectedConditionalGain) > 1e-5f) {
    std::cerr << "nested If/ElseIf config selected the wrong branch\n";
    return 1;
  }

  if (!write(conditional, "If: sampleRate >>> 1\nPreamp: -6 dB\nEndIf:\n"))
    return 1;
  bool conditionErrorHasLocation = false;
  try {
    conditionalEngine.loadConfig(conditional.string());
  } catch (const std::exception &ex) {
    const auto message = std::string(ex.what());
    conditionErrorHasLocation =
        message.find(conditional.string() + ":1:") != std::string::npos &&
        message.find("invalid If expression") != std::string::npos;
  }
  if (!conditionErrorHasLocation) {
    std::cerr << "invalid conditional expression lacked a source location\n";
    return 1;
  }
  float conditionalRetained[2] = {1.0f, -1.0f};
  conditionalEngine.process(conditionalRetained, 1);
  if (std::abs(conditionalRetained[0] - expectedConditionalGain) > 1e-5f ||
      std::abs(conditionalRetained[1] + expectedConditionalGain) > 1e-5f) {
    std::cerr << "invalid conditional config replaced the active graph\n";
    return 1;
  }
#undef SKYAPO_TEST_CONDITIONAL_ELSEIF

  if (!write(conditional, "ElseIf: 1\n"))
    return 1;
  bool malformedConditionalRejected = false;
  try {
    conditionalEngine.loadConfig(conditional.string());
  } catch (const std::exception &ex) {
    malformedConditionalRejected =
        std::string(ex.what()).find(conditional.string() + ":1:") !=
        std::string::npos;
  }
  if (!malformedConditionalRejected) {
    std::cerr << "unmatched ElseIf directive was not diagnosed\n";
    return 1;
  }

#ifdef SKYAPO_TEST_MUPARSERX
  const auto upstreamIirExample =
      fs::path(SKYAPO_TEST_SOURCE_DIR) /
      "upstream/equalizerapo/Setup/config/iir_lowpass.txt";
  const auto measureUpstreamIirTone = [&](double frequency) {
    Engine upstreamIir(48000, 2, 128);
    upstreamIir.loadConfig(upstreamIirExample.string());
    double inputEnergy = 0.0;
    double outputEnergy = 0.0;
    std::vector<float> block(128 * 2);
    for (unsigned blockIndex = 0; blockIndex < 16; ++blockIndex) {
      for (unsigned frame = 0; frame < 128; ++frame) {
        const float sample =
            static_cast<float>(std::sin(2.0 * 3.141592653589793 * frequency *
                                        (blockIndex * 128 + frame) / 48000.0));
        block[2 * frame] = block[2 * frame + 1] = sample;
      }
      upstreamIir.process(block.data(), 128);
      if (blockIndex >= 8) {
        for (float sample : block)
          outputEnergy += static_cast<double>(sample) * sample;
        for (unsigned frame = 0; frame < 128; ++frame) {
          const double sample = std::sin(2.0 * 3.141592653589793 * frequency *
                                         (blockIndex * 128 + frame) / 48000.0);
          inputEnergy += 2.0 * sample * sample;
        }
      }
    }
    return std::sqrt(outputEnergy / inputEnergy);
  };
  const double upstreamIirPassband = measureUpstreamIirTone(1000.0);
  const double upstreamIirStopband = measureUpstreamIirTone(12000.0);
  if (upstreamIirPassband < 0.8 || upstreamIirStopband > 0.15 ||
      upstreamIirStopband >= upstreamIirPassband * 0.15) {
    std::cerr << "official Equalizer APO iir_lowpass.txt response mismatch: "
              << "1 kHz=" << upstreamIirPassband
              << ", 12 kHz=" << upstreamIirStopband << '\n';
    return 1;
  }
  std::cout << "official Equalizer APO iir_lowpass.txt: 1 kHz="
            << upstreamIirPassband << ", 12 kHz=" << upstreamIirStopband
            << '\n';

  const auto expressionRoot = dir / "expression-root.txt";
  if (!write(expressionRoot,
             "Eval: inlineGain = -6\nInclude: expression-child.txt\n") ||
      !write(dir / "expression-child.txt", "Preamp: `inlineGain` dB\n"))
    return 1;
  Engine expressionEngine(48000, 2, 128);
  expressionEngine.loadConfig(expressionRoot.string());
  float expressionSamples[2] = {1.0f, -1.0f};
  expressionEngine.process(expressionSamples, 1);
  if (expressionEngine.filterCount() != 1 ||
      std::abs(expressionSamples[0] - expectedConditionalGain) > 1e-5f ||
      std::abs(expressionSamples[1] + expectedConditionalGain) > 1e-5f) {
    std::cerr << "MuParserX Eval variable did not expand through Include\n";
    return 1;
  }

  if (!write(expressionRoot,
             "Preamp: `(sizeof(regexSearch(\"freq ([0-9]+)\", \"freq 43\")) "
             "> 1) && (regexReplace(\"a\", \"a-b\", \"x\") == \"x-b\") "
             "? -6 : 0` dB\n"))
    return 1;
  expressionEngine.loadConfig(expressionRoot.string());
  float regexSamples[2] = {1.0f, -1.0f};
  expressionEngine.process(regexSamples, 1);
  if (std::abs(regexSamples[0] - expectedConditionalGain) > 1e-5f ||
      std::abs(regexSamples[1] + expectedConditionalGain) > 1e-5f) {
    std::cerr << "upstream regexSearch/regexReplace functions mismatched\n";
    return 1;
  }

  if (!write(expressionRoot,
             "If: not ((\"sky\" + \"apo\") != \"skyapo\") && not false\n"
             "Preamp: -6 dB\nElse:\nPreamp: 0 dB\nEndIf:\n"))
    return 1;
  expressionEngine.loadConfig(expressionRoot.string());
  float upstreamOperators[2] = {1.0f, -1.0f};
  expressionEngine.process(upstreamOperators, 1);
  if (std::abs(upstreamOperators[0] - expectedConditionalGain) > 1e-5f ||
      std::abs(upstreamOperators[1] + expectedConditionalGain) > 1e-5f) {
    std::cerr << "upstream string concatenation/not operators mismatched\n";
    return 1;
  }

  if (!write(expressionRoot, "Preamp: `1 +` dB\n"))
    return 1;
  bool inlineExpressionErrorHasLocation = false;
  try {
    expressionEngine.loadConfig(expressionRoot.string());
  } catch (const std::exception &ex) {
    const auto message = std::string(ex.what());
    inlineExpressionErrorHasLocation =
        message.find(expressionRoot.string() + ":1:") != std::string::npos &&
        message.find("invalid inline expression") != std::string::npos;
  }
  if (!inlineExpressionErrorHasLocation) {
    std::cerr << "invalid inline expression lacked a source location\n";
    return 1;
  }
  float expressionRetained[2] = {1.0f, -1.0f};
  expressionEngine.process(expressionRetained, 1);
  if (std::abs(expressionRetained[0] - expectedConditionalGain) > 1e-5f ||
      std::abs(expressionRetained[1] + expectedConditionalGain) > 1e-5f) {
    std::cerr << "invalid inline expression replaced the active graph\n";
    return 1;
  }
#endif
#ifndef SKYAPO_TEST_MUPARSERX
  if (!write(conditional, "Preamp: `-6` dB\n"))
    return 1;
  bool fallbackExpressionDiagnosed = false;
  try {
    Engine fallbackExpressions(48000, 2, 128);
    fallbackExpressions.loadConfig(conditional.string());
  } catch (const std::exception &ex) {
    const auto message = std::string(ex.what());
    fallbackExpressionDiagnosed =
        message.find(conditional.string() + ":1:") != std::string::npos &&
        message.find("require MuParserX support") != std::string::npos;
  }
  if (!fallbackExpressionDiagnosed) {
    std::cerr << "classic muParser fallback silently accepted inline syntax\n";
    return 1;
  }
#endif
#endif

  unlink(path.c_str());
  fs::remove_all(dir);
  std::cout
      << "Upstream Preamp/BiQuad/IIR/Delay; nested Include, Stage, cycle, "
         "rollback, line diagnostics and unsupported-command tests passed\n";
  return 0;
}
