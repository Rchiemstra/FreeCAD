// SPDX-License-Identifier: LGPL-2.1-or-later
// Known-bad createGridPart arithmetic. Compile with:
//   g++ -std=c++20 -fsanitize=undefined,float-cast-overflow -fno-sanitize-recover=all
// Cases: 1 = large finite X cast, 2 = X add overflow, 3 = Y sub overflow.

#include "old_grid_offset_arithmetic.h"

#include <iostream>
#include <limits>
#include <string>

int main(int argc, char** argv)
{
    const int which = argc > 1 ? std::stoi(argv[1]) : 1;
    if (which == 1) {
        const float minX = static_cast<float>(10000000000.0) - 75.0F;
        const int offset = OldGridOffsetArithmetic::offsetX(minX, 1.0);
        std::cout << "large_x_offset=" << offset << "\n";
        return 0;
    }
    if (which == 2) {
        const int offsetX = 2147483520;
        const int step = OldGridOffsetArithmetic::stepX(149, offsetX);
        std::cout << "near_int_max_step=" << step << "\n";
        return 0;
    }
    if (which == 3) {
        const float minY = static_cast<float>(std::numeric_limits<int>::min() + 20);
        const int offset = OldGridOffsetArithmetic::offsetY(minY, 1.0, 150);
        std::cout << "near_int_min_y_offset=" << offset << "\n";
        return 0;
    }
    std::cerr << "usage: ubsan_old_grid_offsets 1|2|3\n";
    return 2;
}
