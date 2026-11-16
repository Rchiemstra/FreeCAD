// SPDX-License-Identifier: LGPL-2.1-or-later

#pragma once

#include <memory>
#include <vector>

namespace App { class DocumentObject; }
namespace MbD { class ASMTAssembly; }

namespace Assembly
{
/// Add passive, iteration-tracked coordinates to a disposable simulation store.
void recordMotionCoordinates(
    const std::shared_ptr<MbD::ASMTAssembly>& mbdAssembly, App::DocumentObject* simulation,
    const std::vector<App::DocumentObject*>& joints,
    const std::vector<App::DocumentObject*>& motions);
}
