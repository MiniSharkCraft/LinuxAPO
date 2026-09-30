#include "../src/platform/Settings.h"

#include <iostream>
#include <stdexcept>
#include <string>

namespace {
bool reject(const std::string &request, const std::string &reason) {
  const auto decoded = settings::ipc::decodeRequest(request);
  if (decoded.command != settings::ipc::Command::Invalid ||
      decoded.error.find(reason) == std::string::npos) {
    std::cerr << "request was not safely rejected: " << request << '\n';
    return false;
  }
  const auto errorResponse = settings::ipc::decodeResponse(
      settings::ipc::responseFrame(false, decoded.error + "\n"));
  if (errorResponse.ok || errorResponse.payload != decoded.error + "\n") {
    std::cerr << "invalid command did not produce a framed ERR response\n";
    return false;
  }
  return true;
}
bool responseRejected(const std::string &frame, const std::string &reason) {
  try {
    (void)settings::ipc::decodeResponse(frame);
  } catch (const std::exception &error) {
    if (std::string(error.what()).find(reason) != std::string::npos)
      return true;
    std::cerr << "unexpected response rejection: " << error.what() << '\n';
    return false;
  }
  std::cerr << "malformed/unsupported response was accepted\n";
  return false;
}
} // namespace

int main() {
  using settings::ipc::Command;
  for (const auto &[frame, expected] : {
           std::pair<std::string, Command>{"SKYAPO/1 STATUS\n", Command::Status},
           {"SKYAPO/1 RELOAD\n", Command::Reload},
           {"SKYAPO/1 STOP\n", Command::Stop},
       }) {
    if (settings::ipc::decodeRequest(frame).command != expected) {
      std::cerr << "valid IPC request rejected\n";
      return 1;
    }
  }
  if (!reject("STATUS\n", "protocol version") ||
      !reject("SKYAPO/2 STATUS\n", "protocol version") ||
      !reject("", "malformed") ||
      !reject("SKYAPO/1 STATUS", "malformed") ||
      !reject("SKYAPO/1 STATUS\nSTOP\n", "malformed") ||
      !reject("SKYAPO/1 DELETE\n", "unsupported daemon command"))
    return 1;

  const std::string payload("status\nnode\0tail", 16);
  const auto decoded = settings::ipc::decodeResponse(
      settings::ipc::responseFrame(true, payload));
  if (!decoded.ok || decoded.payload != payload) {
    std::cerr << "IPC response payload round-trip mismatch\n";
    return 1;
  }
  const auto oversized = settings::ipc::decodeResponse(
      settings::ipc::responseFrame(true,
                                   std::string(settings::ipc::MaxResponseBytes +
                                                   1,
                                               'x')));
  if (oversized.ok ||
      oversized.payload != "daemon IPC response exceeds size limit\n") {
    std::cerr << "oversized IPC response was not bounded\n";
    return 1;
  }
  if (!responseRejected("Daemon: streaming\n", "legacy") ||
      !responseRejected("SKYAPO/2 OK 0\n", "protocol version") ||
      !responseRejected("SKYAPO/1 OK 4\nabc", "length mismatch") ||
      !responseRejected("SKYAPO/1 MAYBE 0\n", "response status"))
    return 1;
  std::cout << "IPC v1 framing, command validation, and malformed frame tests passed\n";
  return 0;
}
