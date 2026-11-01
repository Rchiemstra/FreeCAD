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
 * Temporary mitigation. OCCT 8 Boolean worker threads do a non-atomic
 * read-modify-write on TopoDS_TShape's flag word. That word is
 * TopoDS_TShape::myState (TopoDS_TShape.hxx), a uint16_t whose bits 0-3 store
 * the shape type and whose remaining bits store the Free, Modified, Checked
 * and other shape flags. setBit reads the word, updates one bit and writes it
 * back. With RunParallel enabled, OSD_ThreadPool workers call
 * TopoDS_Builder::Add, which clears Free, and BRep_Tool::Range, which sets
 * Modified, on shared sub-shapes while other workers read ShapeType().
 *
 * OCCT >= 8.0.0 (OCC_VERSION_HEX >= 0x080000) always returns false, so those
 * algorithms run serially. There is no upper bound yet. OCCT < 8.0.0 returns
 * @p requested, which is the previous behaviour (parallel where the caller
 * asks for it). OCCT 7.x keeps the same flags in Standard_Integer myFlags and
 * updates them with a non-atomic setFlag; ShapeType() is virtual there. That
 * is the same read-modify-write pattern. OCCT 7.x is outside this mitigation
 * and is not claimed safe.
 *
 * https://github.com/Open-Cascade-SAS/OCCT/pull/1548 makes TopoDS_TShape's
 * flag word atomic. Retire the OCCT >= 8 branch of this function once a
 * released OCCT that contains that change is verified, and then bound the
 * version check to releases below that fixed version.
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
 * RunParallel flag. This is the only production call of
 * BOPAlgo_Options::SetParallelMode. On OCCT >= 8 the default is set from
 * booleanRunParallel(), so it is never turned on. OCCT starts with a serial
 * default, so algorithms built without an explicit SetRunParallel call, such
 * as the eager BRepAlgoAPI constructors, also run serially. See booleanRunParallel()
 * for the race, the version range, and the OCCT change that lets this
 * policy be retired.
 */
PartExport void setBooleanParallelMode(bool requested = true);

}  // namespace Part
