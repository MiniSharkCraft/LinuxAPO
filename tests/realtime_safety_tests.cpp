#include "../src/platform/PlatformChannels.h"
#include "../src/platform/RealtimeAudit.h"
#include "Engine.h"
#include <cmath>
#include <fstream>
#include <iostream>
#include <unistd.h>
#include <vector>
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
  unlink(path);
  if (realtime::allocations.load() || realtime::deallocations.load()) {
    std::cerr << "DSP process allocated/freed in realtime scope\n";
    return 1;
  }
  if (eapoChannel("FC") != L"C" || eapoChannel("LFE") != L"LFE" ||
      eapoChannel("SL") != L"SL")
    return 1;
  std::cout << "Allocation hooks verified; 2000 mono/stereo DSP blocks without "
               "heap allocation or freeing\n";
  return 0;
}
