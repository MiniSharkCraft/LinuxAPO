#pragma once

// Construction-time metadata only. Factories may inspect this object during
// initialize(), but must not retain its address beyond that call.
class IFilterFactoryContext {
public:
  virtual ~IFilterFactoryContext() = default;

  virtual unsigned getRealChannelCount() const noexcept = 0;
  virtual unsigned getOutputChannelCount() const noexcept = 0;
  virtual unsigned getMaxFrameCount() const noexcept = 0;
};
