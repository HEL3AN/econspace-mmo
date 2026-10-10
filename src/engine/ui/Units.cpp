#include "ui/Units.h"

#include <cmath>
#include <cstdio>

namespace Ui
{

std::string Distance(float units)
{
    char         buf[32];
    const double d = std::fabs((double)units);
    // Decided on the value as it will be printed, so 99 960 is "100k" and not "100.0k", and
    // 999 600 is "1.00M" and not "1000k".
    if (d < 9999.5)
        std::snprintf(buf, sizeof(buf), "%.0f", d);
    else if (d < 99950.0)
        std::snprintf(buf, sizeof(buf), "%.1fk", d / 1e3);
    else if (d < 999500.0)
        std::snprintf(buf, sizeof(buf), "%.0fk", d / 1e3);
    else if (d < 9995000.0)
        std::snprintf(buf, sizeof(buf), "%.2fM", d / 1e6);
    else if (d < 99950000.0)
        std::snprintf(buf, sizeof(buf), "%.1fM", d / 1e6);
    else
        std::snprintf(buf, sizeof(buf), "%.0fM", d / 1e6);
    return buf;
}

}  // namespace Ui
