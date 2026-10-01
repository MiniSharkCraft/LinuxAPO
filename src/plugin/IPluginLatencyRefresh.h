#pragma once

// Control-thread interface for servicing plugin latency-change notifications.
// Call refresh only after the realtime graph has been quiesced and drained.
class IPluginLatencyRefresh {
public:
  virtual ~IPluginLatencyRefresh() = default;
  virtual bool latencyRefreshPending() const noexcept = 0;
  virtual bool refreshPluginLatency() = 0;
};
