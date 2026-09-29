#include "Engine.h"
#include <cmath>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <unistd.h>
#include <sndfile.h>

namespace fs = std::filesystem;
bool write(const fs::path &path, const std::string &contents) {
  std::ofstream out(path);
  out << contents;
  return bool(out);
}
int main() {
  const std::string path =
      "/tmp/skyapo-test-" + std::to_string(getpid()) + ".txt";
  if (!write(path, "Preamp: -6 dB\n"))
    return 1;
  Engine e(48000, 2, 128);
  e.loadConfig(path);
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
  const float impulseResponse[4] = {1, 0, 0, 0};
  if (sf_writef_float(irFile, impulseResponse, 4) != 4) {
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
  float convBlock[512]{};
  for (int f = 0; f < 256; ++f) {
    convBlock[2 * f] = 0.1f * f;
    convBlock[2 * f + 1] = -0.05f * f;
  }
  bool rejectedVariableBlock = false;
  try {
    convolution.process(convBlock, 255);
  } catch (const std::exception &) {
    rejectedVariableBlock = true;
  }
  if (!rejectedVariableBlock)
    return 1;
  convolution.process(convBlock, 256);
  for (int i = 0; i < 512; ++i)
    if (std::abs(convBlock[i] - ((i % 2) ? -0.05f : 0.1f) * (i / 2)) >
        1e-4f) {
      std::cerr << "upstream Convolution impulse response mismatch at " << i
                << " got " << convBlock[i] << '\n';
      return 1;
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
#endif
  fs::create_directories(dir / "sub dir");
  const auto root = dir / "root.txt";
  if (!write(root, "Preamp: -6 dB\nInclude: \"sub dir/child file.txt\"\n") ||
      !write(dir / "sub dir/child file.txt",
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

  unlink(path.c_str());
  fs::remove_all(dir);
  std::cout
      << "Upstream Preamp/BiQuad/IIR/Delay; nested Include, cycle, rollback, "
         "line diagnostics and unsupported-command tests passed\n";
  return 0;
}
