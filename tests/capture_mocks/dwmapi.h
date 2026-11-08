#pragma once
#include <cstdint>
using HRESULT = std::int32_t;
#define FAILED(value) ((value) < 0)
HRESULT DwmFlush() noexcept;
