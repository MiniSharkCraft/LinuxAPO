#include <cmath>
#include <cstdlib>
#include <iostream>
#include <sndfile.h>
#include <string>
#include <unistd.h>
#include <vector>

int main(int argc, char **argv) {
  if (argc != 3 && argc != 4)
    return 2;
  const double expectedDb = argc == 4 ? std::strtod(argv[3], nullptr) : -6.0;
  const auto stem = "/tmp/skyapo-render-test-" + std::to_string(getpid());
  const auto input = stem + "-in.wav", output = stem + "-out.wav";
  SF_INFO info{};
  info.samplerate = 44100;
  info.channels = 2;
  info.format = SF_FORMAT_WAV | SF_FORMAT_PCM_24;
  auto *file = sf_open(input.c_str(), SFM_WRITE, &info);
  if (!file) {
    std::cerr << sf_strerror(nullptr) << '\n';
    return 1;
  }
  std::vector<float> samples(1000 * 2);
  for (size_t i = 0; i < samples.size(); ++i)
    samples[i] = static_cast<float>(0.2 * std::sin(0.013 * i));
  if (sf_writef_float(file, samples.data(), 1000) != 1000) {
    sf_close(file);
    return 1;
  }
  sf_close(file);
  const std::string command = "\"" + std::string(argv[1]) + "\" --input \"" +
                              input + "\" --output \"" + output +
                              "\" --config \"" + argv[2] + "\"";
  if (std::system(command.c_str()) != 0)
    return 1;
  SF_INFO outInfo{};
  file = sf_open(output.c_str(), SFM_READ, &outInfo);
  if (!file)
    return 1;
  std::vector<float> actual(1000 * 2);
  const auto frames = sf_readf_float(file, actual.data(), 1000);
  sf_close(file);
  unlink(input.c_str());
  unlink(output.c_str());
  if (frames != 1000 || outInfo.samplerate != 44100 || outInfo.channels != 2) {
    std::cerr << "render format/frame count mismatch\n";
    return 1;
  }
  const float expected = std::pow(10.0f, static_cast<float>(expectedDb / 20.0));
  for (size_t i = 0; i < actual.size(); ++i) {
    const float source = static_cast<float>(0.2 * std::sin(0.013 * i));
    if (std::abs(actual[i] - source * expected) > 2e-5f) {
      std::cerr << "render gain mismatch at sample " << i << '\n';
      return 1;
    }
  }
  std::cout << "WAV render sample gain and format test passed at " << expectedDb
            << " dB\n";
  return 0;
}
