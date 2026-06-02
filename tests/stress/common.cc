#include "common.h"

uint16_t g_oversub_pct = 150;

uint16_t contention_count()
{
    uint32_t hw = std::thread::hardware_concurrency();
    if (hw == 0)
    {
        hw = 4;
    }
    uint32_t total    = (hw * g_oversub_pct + 99) / 100;  // ceil(hw * pct / 100)
    uint32_t per_side = std::max<uint32_t>(2, (total + 1) / 2);
    return static_cast<uint16_t>(std::min<uint32_t>(per_side, UINT16_MAX));
}
