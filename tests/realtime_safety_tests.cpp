#include "../src/platform/PlatformChannels.h"
#include "../src/platform/RealtimeAudit.h"
#include "Engine.h"
#include <cmath>
#include <fstream>
#include <iostream>
#include <unistd.h>
#include <vector>
#ifdef SKYAPO_TEST_CONVOLUTION
#include <sndfile.h>
#endif
extern "C" void *__wrap_malloc(size_t);
extern "C" void __wrap_free(void *);
int main() {
  {
    realtime::Scope scope;
    auto *c = __wrap_malloc(8);
    __wrap_free(c);
    auto *cpp = ::operator new(16);
    ::operator delete(cpp);
  }
  if (realtime::allocations.load() != 2 ||
      realtime::deallocations.load() != 2) {
    std::cerr << "allocation instrumentation is not working\n";
    return 1;
  }
  realtime::allocations = 0;
  realtime::deallocations = 0;
  char path[] = "/tmp/skyapo-rt-test-XXXXXX";
  int fd = mkstemp(path);
  if (fd < 0)
    return 1;
  close(fd);
  {
    std::ofstream f(path);
    f << "Preamp: -6 dB\nFilter: ON PK Fc 1000 Hz Gain 2 dB Q 1\nDelay: 3 "
         "Samples\nFilter: ON IIR Order 1 Coefficients 1 0 1 0\n";
  }
  for (unsigned channels : {1u, 2u}) {
    std::vector<std::wstring> names =
        channels == 1
            ? std::vector<std::wstring>{eapoChannel("MONO")}
            : std::vector<std::wstring>{eapoChannel("FL"), eapoChannel("FR")};
    Engine engine(48000, channels, 8192, names);
    engine.loadConfig(path);
    std::vector<float> audio(channels * 8192);
    for (unsigned iteration = 0; iteration < 1000; ++iteration) {
      unsigned frames = (iteration * 71) % 8192 + 1;
      for (unsigned i = 0; i < frames * channels; ++i)
        audio[i] = .1f * std::sin(float(i));
      {
        realtime::Scope scope;
        engine.process(audio.data(), frames);
      }
    }
  }
  {
    char routePath[] = "/tmp/skyapo-route-test-XXXXXX";
    int routeFd = mkstemp(routePath);
    if (routeFd < 0)
      return 1;
    close(routeFd);
    {
      std::ofstream f(routePath);
      f << "Channel: L\nCopy: L=R R=L\nPreamp: -6 dB\n"
           "Filter: ON PK Fc 1000 Hz Gain 2 dB Q 1\nDelay: 3 Samples\n";
    }
    Engine route(48000, 2, 8192, {L"L", L"R"});
    route.loadConfig(routePath);
    std::vector<float> audio(2 * 8192);
    for (unsigned iteration = 0; iteration < 1000; ++iteration) {
      unsigned frames = (iteration * 43) % 8192 + 1;
      for (unsigned i = 0; i < frames * 2; ++i)
        audio[i] = .1f * std::cos(float(i));
      {
        realtime::Scope scope;
        route.process(audio.data(), frames);
      }
    }
    unlink(routePath);
  }
#ifdef SKYAPO_TEST_LV2
  {
    char pluginPath[] = "/tmp/skyapo-lv2-test-XXXXXX";
    int pluginFd = mkstemp(pluginPath);
    if (pluginFd < 0)
      return 1;
    close(pluginFd);
    {
      std::ofstream f(pluginPath);
      f << "Plugin: LV2 https://skyapo.example/plugins/test-gain\n";
    }
    Engine plugin(48000, 2, 8192, {L"L", L"R"});
    plugin.loadConfig(pluginPath);
    std::vector<float> audio(2 * 8192);
    for (unsigned iteration = 0; iteration < 1000; ++iteration) {
      const unsigned frames = (iteration * 59) % 8192 + 1;
      for (unsigned i = 0; i < frames * 2; ++i)
        audio[i] = .1f * std::sin(float(i + iteration));
      {
        realtime::Scope scope;
        plugin.process(audio.data(), frames);
      }
    }
    unlink(pluginPath);
  }
#endif
#ifdef SKYAPO_TEST_CONVOLUTION
  char irTemplate[] = "/tmp/skyapo-rt-ir-XXXXXX";
  int irFd = mkstemp(irTemplate);
  if (irFd < 0)
    return 1;
  close(irFd);
  SF_INFO info{};
  info.samplerate = 48000;
  info.channels = 1;
  info.format = SF_FORMAT_WAV | SF_FORMAT_FLOAT;
  SNDFILE *ir = sf_open(irTemplate, SFM_WRITE, &info);
  if (!ir)
    return 1;
  const float taps[] = {1.0f, 0.25f, -0.125f, 0.0f};
  if (sf_writef_float(ir, taps, 4) != 4) {
    sf_close(ir);
    unlink(irTemplate);
    return 1;
  }
  sf_close(ir);
  std::string configPath = std::string(irTemplate) + ".txt";
  {
    std::ofstream f(configPath);
    f << "Convolution: " << irTemplate << '\n';
  }
  Engine convolution(48000, 2, 256, {eapoChannel("FL"), eapoChannel("FR")});
  convolution.loadConfig(configPath);
  std::vector<float> convolutionBlock(256 * 2);
  for (unsigned iteration = 0; iteration < 1000; ++iteration) {
    for (unsigned i = 0; i < convolutionBlock.size(); ++i)
      convolutionBlock[i] = .1f * std::sin(float(i + iteration));
    {
      realtime::Scope scope;
      convolution.process(convolutionBlock.data(), 256);
    }
  }
  unlink(configPath.c_str());
  unlink(irTemplate);
#endif
  unlink(path);
  if (realtime::allocations.load() || realtime::deallocations.load()) {
    std::cerr << "DSP process allocated/freed in realtime scope\n";
    return 1;
  }
  if (eapoChannel("FC") != L"C" || eapoChannel("LFE") != L"LFE" ||
      eapoChannel("SL") != L"SL")
    return 1;
  std::cout << "Allocation hooks verified; mono/stereo, channel-routing"
#ifdef SKYAPO_TEST_CONVOLUTION
               ", and upstream convolution"
#endif
               " DSP blocks without heap allocation/freeing\n";
  return 0;
}
