#include "Engine.h"
#include "BiQuad.h"
#include "LoudnessVolumeProvider.h"
#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <thread>
#include <sndfile.h>
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
      actionable = message.find(path + ":1:") != std::string::npos &&
                   (message.find("finite float audio range") !=
                        std::string::npos ||
                    message.find("Nyquist") != std::string::npos ||
                    message.find("finite and in range") !=
                        std::string::npos);
      if (!actionable)
        std::cerr << "numeric range diagnostic was not actionable: "
                  << message << '\n';
    }
    if (!actionable) {
      std::cerr << "out-of-range DSP parameter was not diagnosed\n";
      return 1;
    }
  }

  if (!write(path,
             "# Windows endpoint-volume dependent upstream filter\n"
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
  float clapBlock[8] = {.2f, -.4f, .6f, -.8f, 1.0f, -1.0f, .5f, -.5f};
  const float clapExpected[8] = {.1f, -.2f, .3f, -.4f, .5f, -.5f, .25f, -.25f};
  clapPlugin.process(clapBlock, 4);
  for (unsigned i = 0; i < 8; ++i)
    if (std::abs(clapBlock[i] - clapExpected[i]) > 1e-5f) {
      std::cerr << "CLAP test plugin output mismatch at " << i << '\n';
      return 1;
    }

  if (!write(path, "Plugin: CLAP org.skyapo.test.gain Gain=0.25\n"))
    return 1;
  Engine clapOverride(48000, 2, 128, {L"L", L"R"});
  clapOverride.loadConfig(path);
  float clapOverriddenBlock[8] = {.2f, -.4f, .6f, -.8f, 1.0f, -1.0f, .5f, -.5f};
  const float overriddenExpected[8] = {.05f, -.1f, .15f, -.2f,
                                       .25f, -.25f, .125f, -.125f};
  clapOverride.process(clapOverriddenBlock, 4);
  for (unsigned i = 0; i < 8; ++i)
    if (std::abs(clapOverriddenBlock[i] - overriddenExpected[i]) > 1e-5f) {
      std::cerr << "CLAP parameter override output mismatch at " << i << '\n';
      return 1;
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
    BiQuad upstreamReference(BiQuad::PEAKING, 6.0, 1000.0, rate, 1.0,
                             false);
    std::vector<float> tone(static_cast<size_t>(totalFrames) * 2);
    std::vector<float> referenceOutput(static_cast<size_t>(totalFrames) * 2);
    constexpr double twoPi = 6.28318530717958647692;
    for (int frame = 0; frame < totalFrames; ++frame) {
      const float sample = static_cast<float>(
          std::sin(twoPi * frequency * frame / rate));
      tone[2 * frame] = sample;
      tone[2 * frame + 1] = sample;
      const float expected = static_cast<float>(upstreamReference.process(sample));
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
        if (std::abs(tone[2 * (frame + i)] -
                     referenceOutput[2 * (frame + i)]) > 2e-6f) {
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
    return std::pair<double, double>{
        std::sqrt(outputEnergy / inputEnergy),
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
#define SKYAPO_TEST_CONDITIONAL_ELSEIF \
  "ElseIf: inputChannelCount == 2; sampleRate == 48000\n"
#else
#define SKYAPO_TEST_CONDITIONAL_ELSEIF \
  "ElseIf: inputChannelCount == 2 && sampleRate == 48000\n"
#endif
  const auto conditional = dir / "conditional.txt";
  if (!write(conditional,
             "If: sampleRate < 0\n"
             "If: invalid (\n"
             "UnsupportedInsideFalseBranch: skipped\n"
             "Include: missing-only-in-false-branch.txt\n"
             "EndIf:\n"
             SKYAPO_TEST_CONDITIONAL_ELSEIF
             "If: 1\n"
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

  if (!write(conditional,
             "If: sampleRate >>> 1\nPreamp: -6 dB\nEndIf:\n"))
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
