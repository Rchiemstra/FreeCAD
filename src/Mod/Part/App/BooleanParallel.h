// SPDX-License-Identifier: LGPL-2.1-or-later

/***************************************************************************
 *   Copyright (c) 2026 Rchiemstra <rchiemstra.rh@gmail.com>               *
 *                                                                         *
 *   This file is part of FreeCAD.                                         *
 *                                                                         *
 *   FreeCAD is free software: you can redistribute it and/or modify it    *
 *   under the terms of the GNU Lesser General Public License as           *
 *   published by the Free Software Foundation, either version 2.1 of the  *
 *   License, or (at your option) any later version.                       *
 *                                                                         *
 *   FreeCAD is distributed in the hope that it will be useful, but        *
 *   WITHOUT ANY WARRANTY; without even the implied warranty of            *
 *   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the GNU      *
 *   Lesser General Public License for more details.                       *
 *                                                                         *
 *   You should have received a copy of the GNU Lesser General Public      *
 *   License along with FreeCAD. If not, see                               *
 *   <https://www.gnu.org/licenses/>.                                      *
 *                                                                         *
 **************************************************************************/

#pragma once

#include <Mod/Part/PartGlobal.h>

namespace Part
{

/**
 * Whether a Boolean-family OCCT algorithm may use OCCT's internal thread pool.
 *
 * Temporary mitigation. OCCT 8.0.x has a data race on TopoDS_TShape::myState
 * (TopoDS_TShape.hxx:186), a uint16_t whose bits 0-3 store the shape type and
 * whose remaining bits store the Free, Modified and Checked flags. setBit
 * (TopoDS_TShape.hxx:176-182) is a non-atomic read-modify-write. With
 * RunParallel enabled, OSD_ThreadPool workers call TopoDS_Builder::Add, which
 * clears Free, and BRep_Tool::Range, which sets Modified, on shared sub-shapes
 * while other workers read ShapeType(). An OCC-only program reports that race
 * under ThreadSanitizer for parallel cut, fuse and common, and does not report
 * it when the same operations run serially.
 *
 * OCCT >= 8.0.0 (OCC_VERSION_HEX >= 0x080000) always returns false. There is
 * no upper bound: the check stays in force until a release that contains a fix
 * has been verified. OCCT < 8.0.0 returns @p requested, which is today's
 * behaviour (parallel where the caller asks for it). OCCT 7.x keeps the same
 * flags in Standard_Integer myFlags and updates them with a non-atomic
 * setFlag; ShapeType() is virtual there. That is the same read-modify-write
 * pattern by inspection. OCCT 7.x is outside this mitigation and has not been
 * verified safe.
 *
 * OCCT issue: TBD
 *
 * Retirement: delete the OCCT 8 branch of this function only after an OCCT
 * release that contains a fix is verified with the OCC-only ThreadSanitizer
 * program (60 parallel runs, zero myState warnings) and with the FreeCAD
 * ThreadSanitizer proof (Part Cut, Fuse and Common, a PartDesign Pocket, a
 * generalFuse, and a Shape.check(True) BOP check, with no warning whose stack
 * has a BOPAlgo_, BOPTools_ or IntTools_ frame or a TopoDS_TShape frame). Then
 * bound the version check to versions below that fixed release.
 *
 * @param requested The flag the caller would have passed before this
 *        mitigation. False stays serial on every OCCT version. On OCCT >= 8
 *        the result is false for every value of @p requested.
 */
PartExport bool booleanRunParallel(bool requested = true);

/**
 * Process-wide BOPAlgo_Options parallel default.
 *
 * Every later-constructed BOP algorithm, including algorithms OCCT builds
 * internally (offset, fillet, feature), copies this default into its own
 * RunParallel flag. This is the only FreeCAD call site of
 * BOPAlgo_Options::SetParallelMode. On OCCT >= 8 the default is set from
 * booleanRunParallel(), so it is never turned on. See the comment on
 * booleanRunParallel() for the race, the version range, OCCT issue: TBD,
 * and the retirement rule.
 */
PartExport void setBooleanParallelMode(bool requested = true);

}  // namespace Part
