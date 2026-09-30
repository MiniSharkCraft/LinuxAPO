#pragma once

#include "IFilterFactory.h"
#include <memory>

std::unique_ptr<IFilterFactory> makeVST2PluginFilterFactory();
