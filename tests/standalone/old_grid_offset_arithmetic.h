// SPDX-License-Identifier: LGPL-2.1-or-later
// Historical createGridPart offset formulas. Known-bad; for UBSan probes only.
#pragma once

namespace OldGridOffsetArithmetic
{

inline int offsetX(float minX, double gridValue)
{
    return static_cast<int>(static_cast<double>(minX) / gridValue);
}

inline int stepX(int lineIndex, int offset)
{
    return lineIndex + offset;
}

inline int offsetY(float minY, double gridValue, int vlines)
{
    return static_cast<int>(static_cast<double>(minY) / gridValue) - vlines;
}

}  // namespace OldGridOffsetArithmetic
