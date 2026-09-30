/*
 * This adapter hosts the realtime processing methods from Equalizer APO's
 * FilterEngine.cpp without importing its Win32-owned loader/watcher lifecycle.
 * The method bodies are generated from the upstream source at configure time.
 */
#pragma once

#include <atomic>

class FilterConfiguration;

class UpstreamFilterEngineProcess {
public:
  UpstreamFilterEngineProcess(unsigned realChannels, unsigned outputChannels,
                              unsigned transitionLength) noexcept
      : realChannelCount(realChannels), outputChannelCount(outputChannels),
        transitionLength(transitionLength) {}

  void setConfigurations(FilterConfiguration *current,
                        FilterConfiguration *next) noexcept {
    currentConfig = current;
    nextConfig = next;
    previousConfig = nullptr;
    transitionCounter = 0;
    completedTransitionCounter = 0;
    transitionComplete.store(false, std::memory_order_relaxed);
  }

  void setTransitionCounter(unsigned counter) noexcept {
    transitionCounter = counter;
  }

  unsigned getTransitionCounter() const noexcept { return transitionCounter; }

  unsigned getCompletedTransitionCounter() const noexcept {
    return completedTransitionCounter;
  }

  FilterConfiguration *takePreviousConfiguration() noexcept {
    auto *previous = previousConfig;
    previousConfig = nullptr;
    return previous;
  }

  bool takeTransitionComplete() noexcept {
    return transitionComplete.exchange(false, std::memory_order_acq_rel);
  }

  void process(float *output, float *input, unsigned frameCount);
  void process(float **output, float **input, unsigned frameCount);

private:
  unsigned realChannelCount;
  unsigned outputChannelCount;
  unsigned transitionCounter{};
  unsigned completedTransitionCounter{};
  unsigned transitionLength;
  FilterConfiguration *currentConfig{};
  FilterConfiguration *nextConfig{};
  FilterConfiguration *previousConfig{};
  std::atomic<bool> transitionComplete{false};
};
