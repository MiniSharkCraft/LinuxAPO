#pragma once

#include "LoudnessVolumeProvider.h"

/** Linux replacement for upstream's Windows COM endpoint volume adapter. */
class VolumeController {
public:
  long getVolume(float &currentVolume) const noexcept {
    return skyapo::platform::LoudnessVolumeProvider::read(currentVolume) ? 0
                                                                         : 1;
  }
};
