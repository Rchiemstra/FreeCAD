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

#include "BooleanParallel.h"

#include <BOPAlgo_Options.hxx>
#include <Standard_Version.hxx>

// Version policy: BooleanParallel.h. On OCCT >= 8 the result is always serial.

#if OCC_VERSION_HEX >= 0x080000
bool Part::booleanRunParallel(bool)
{
    return false;
}
#else
bool Part::booleanRunParallel(bool requested)
{
    return requested;
}
#endif

void Part::setBooleanParallelMode(bool requested)
{
    BOPAlgo_Options::SetParallelMode(booleanRunParallel(requested));
}
