// SPDX-License-Identifier: LGPL-2.1-or-later
/****************************************************************************
 *                                                                          *
 *   Copyright (c) 2026 The FreeCAD Project Association AISBL              *
 *                                                                          *
 *   Private test harness for ViewProviderReviewNote fault injection.       *
 *                                                                          *
 ***************************************************************************/

#include "ViewProviderReviewNoteTestHarness.h"

#ifdef ASSEMBLY_ENABLE_TEST_HOOKS

namespace AssemblyGui
{
namespace ReviewNoteTestHarness
{

int injectThrowAfterCoords = 0;
int injectNestedCamera = 0;
int nestedDirtyMarked = 0;
int applyExceptionsCaught = 0;

void reset()
{
    injectThrowAfterCoords = 0;
    injectNestedCamera = 0;
    nestedDirtyMarked = 0;
    applyExceptionsCaught = 0;
}

void setInjectThrowAfterCoords(int count)
{
    injectThrowAfterCoords = count;
}

void setInjectNestedCamera(int count)
{
    injectNestedCamera = count;
}

int nestedDirtyMarkedCount()
{
    return nestedDirtyMarked;
}

int applyExceptionsCaughtCount()
{
    return applyExceptionsCaught;
}

}  // namespace ReviewNoteTestHarness
}  // namespace AssemblyGui

#endif  // ASSEMBLY_ENABLE_TEST_HOOKS
