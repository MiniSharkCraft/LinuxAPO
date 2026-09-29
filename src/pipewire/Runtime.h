#pragma once
#include <csignal>
#include <string>
void runPipeWire(const std::string &device, const std::string &config,
                 volatile sig_atomic_t &stopping);
