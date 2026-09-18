// SPDX-License-Identifier: LGPL-2.1-or-later
// Evidence-only: the conversion gdb WP 358's Inf camera would have used.
// Compile: g++ -std=c++20 -fsanitize=undefined -O0 -g
// Observed locally: scaled is non-finite; short becomes 32767; UBSan does not trap.

#include <cmath>
#include <iostream>
#include <limits>

int main()
{
    const float inf = std::numeric_limits<float>::infinity();
    const float scaled = std::roundf(inf * 1024.0F);
    const short pixel = static_cast<short>(scaled);
    std::cout << "old_inf_to_short scaled_finite=" << std::isfinite(scaled)
              << " pixel=" << pixel << "\n";
    return std::isfinite(scaled) ? 1 : 0;
}
