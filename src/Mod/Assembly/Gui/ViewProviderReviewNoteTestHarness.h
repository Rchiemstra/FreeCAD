// SPDX-License-Identifier: LGPL-2.1-or-later
/****************************************************************************
 *                                                                          *
 *   Copyright (c) 2026 The FreeCAD Project Association AISBL              *
 *                                                                          *
 *   Private test harness for ViewProviderReviewNote fault injection.       *
 *   Not part of the production AssemblyGui public C++ API.                 *
 *   Compiled only when ASSEMBLY_ENABLE_TEST_HOOKS is defined.              *
 *                                                                          *
 ***************************************************************************/

#pragma once

#ifdef ASSEMBLY_ENABLE_TEST_HOOKS

namespace AssemblyGui
{
namespace ReviewNoteTestHarness
{

void reset();
void setInjectThrowAfterCoords(int count);
void setInjectNestedCamera(int count);
int nestedDirtyMarkedCount();
int applyExceptionsCaughtCount();

/// Counters used by ViewProviderReviewNote internals (test builds only).
extern int injectThrowAfterCoords;
extern int injectNestedCamera;
extern int nestedDirtyMarked;
extern int applyExceptionsCaught;

}  // namespace ReviewNoteTestHarness
}  // namespace AssemblyGui

#endif  // ASSEMBLY_ENABLE_TEST_HOOKS
