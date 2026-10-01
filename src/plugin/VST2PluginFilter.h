#pragma once

#include "IFilterFactory.h"
#include <filesystem>
#include <memory>

std::unique_ptr<IFilterFactory> makeVST2PluginFilterFactory();
