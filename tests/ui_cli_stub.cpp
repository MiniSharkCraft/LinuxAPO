#include <chrono>
#include <iostream>
#include <string>
#include <thread>

int main(int argc, char **argv) {
  if (argc >= 3 && std::string(argv[1]) == "device" &&
      std::string(argv[2]) == "list") {
    std::this_thread::sleep_for(std::chrono::milliseconds(800));
    std::cout << "ID\tNODE NAME\tDESCRIPTION\tSELECTED\tCHANNELS\tSAMPLE RATE\n"
                 "41\tfixture.capture\tDelayed Test Input\tyes\t2\t48000\n";
    return 0;
  }
  if (argc >= 2 && std::string(argv[1]) == "status") {
    std::this_thread::sleep_for(std::chrono::milliseconds(800));
    std::cout << "Daemon: not reachable\n";
    return 0;
  }
  std::cerr << "unexpected CLI fixture command\n";
  return 2;
}
