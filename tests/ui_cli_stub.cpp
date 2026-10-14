#include <chrono>
#include <cstdlib>
#include <fstream>
#include <iostream>
#include <string>
#include <thread>

int main(int argc, char **argv) {
  if (argc == 3 && std::string(argv[1]) == "plugin" &&
      std::string(argv[2]) == "list") {
    std::this_thread::sleep_for(std::chrono::milliseconds(80));
    std::cout << "LV2 plugins discovered: 1\n"
                 "LV2\thttps://example.test/plugins/gain\tTest Gain\n";
    return 0;
  }
  if (argc == 4 && std::string(argv[1]) == "plugin" &&
      std::string(argv[2]) == "info" &&
      std::string(argv[3]) == "https://example.test/plugins/gain") {
    std::this_thread::sleep_for(std::chrono::milliseconds(80));
    std::cout << "Test Gain\nFormat: LV2\n"
                 "URI: https://example.test/plugins/gain\n"
                 "gain\tGain\tdefault=0.5\trange=[0, 1]\n";
    return 0;
  }
  if (argc == 4 && std::string(argv[1]) == "plugin" &&
      std::string(argv[2]) == "info" &&
      std::string(argv[3]) == "test.clap.gain") {
    std::this_thread::sleep_for(std::chrono::milliseconds(80));
    std::cout << "Test CLAP Gain\nFormat: CLAP\nID: test.clap.gain\n"
                 "7\tGain\tdefault=0.5\trange=[0, 1]\n";
    return 0;
  }
  if (argc == 4 && std::string(argv[1]) == "plugin" &&
      std::string(argv[2]) == "info" &&
      std::string(argv[3]) == "test.vst3.uid") {
    std::this_thread::sleep_for(std::chrono::milliseconds(80));
    std::cout << "Test VST3 Gain\nFormat: VST3\n"
                 "Class UID: test.vst3.uid\n"
                 "42\tGain\tdefault=0.5\trange=[0, 1]\tvalue=0.5\n";
    return 0;
  }
  if (argc == 5 && std::string(argv[1]) == "config" &&
      std::string(argv[2]) == "check" && std::string(argv[3]) == "--json") {
    std::this_thread::sleep_for(std::chrono::milliseconds(250));
    std::ifstream input(argv[4]);
    const std::string contents((std::istreambuf_iterator<char>(input)),
                               std::istreambuf_iterator<char>());
    const bool valid = contents.find("Preamp: -3 dB") != std::string::npos;
    if (valid) {
      std::cout << "{\"valid\":true,\"filter_count\":1,"
                   "\"diagnostics\":[]}\n";
      return 0;
    }
    std::cout << "{\"valid\":false,\"filter_count\":null,"
                 "\"diagnostics\":[{\"file\":\""
              << argv[4]
              << "\",\"line\":1,\"reason\":\"fixture rejected unsaved "
                 "Preamp value\"}]}\n";
    return 1;
  }
  if (argc >= 3 && std::string(argv[1]) == "device" &&
      std::string(argv[2]) == "list") {
    std::this_thread::sleep_for(std::chrono::milliseconds(800));
    if (const char *fixture = std::getenv("SKYAPO_UI_TEST_DEVICE_LIST_FILE")) {
      std::ifstream input(fixture);
      std::cout << std::string((std::istreambuf_iterator<char>(input)),
                               std::istreambuf_iterator<char>());
      return input.bad() ? 1 : 0;
    }
    std::cout << "ID\tNODE NAME\tDESCRIPTION\tSELECTED\tCHANNELS\tSAMPLE RATE\t"
                 "STABLE PROPERTY\tDEVICE BUS ID\tALSA CARD\tALSA PATH\t"
                 "DEFAULT\n"
                 "41\tfixture.capture\tDelayed Test Input\t\t2\t48000\t"
                 "\t\t\t\tno\n"
                 "42\tfixture.usb-mic\tSelected USB Microphone\tyes\t1\t44100\t"
                 "\t\t\t\tyes\n";
    return 0;
  }
  if (argc == 4 && std::string(argv[1]) == "device" &&
      std::string(argv[2]) == "set") {
    std::this_thread::sleep_for(std::chrono::milliseconds(150));
    if (const char *log = std::getenv("SKYAPO_UI_TEST_DEVICE_LOG")) {
      std::ofstream output(log, std::ios::app);
      output << argv[3] << '\n';
    }
    std::cout << "Selected " << argv[3] << '\n';
    return 0;
  }
  if (argc >= 2 && std::string(argv[1]) == "status") {
    std::this_thread::sleep_for(std::chrono::milliseconds(800));
    if (const char *format = std::getenv("SKYAPO_UI_TEST_STREAMING_FORMAT")) {
      const std::string value(format);
      const auto separator = value.find(':');
      if (separator != std::string::npos) {
        std::cout << "Daemon: streaming\nSample rate: "
                  << value.substr(0, separator)
                  << " Hz\nChannels: " << value.substr(separator + 1) << "\n";
        return 0;
      }
    }
    std::cout << "Daemon: not reachable\n";
    return 0;
  }
  std::cerr << "unexpected CLI fixture command\n";
  return 2;
}
