// SPDX-License-Identifier: LGPL-2.1-or-later

#pragma once

#include <FCGlobal.h>

namespace App
{
class Property;
}

namespace Gui
{
class ViewProvider;

/** Marks one synthetic property callback. The handler consumes it on entry,
 * so genuine notifications nested inside that callback are never marked.
 */
class GuiExport SyntheticPropertyReplay
{
public:
    SyntheticPropertyReplay(const ViewProvider* provider, const App::Property* property);
    ~SyntheticPropertyReplay();
    SyntheticPropertyReplay(const SyntheticPropertyReplay&) = delete;
    SyntheticPropertyReplay& operator=(const SyntheticPropertyReplay&) = delete;

    static bool consume(const ViewProvider* provider, const App::Property* property);

private:
    static thread_local SyntheticPropertyReplay* current;
    SyntheticPropertyReplay* previous;
    const ViewProvider* provider;
    const App::Property* property;
    bool consumed {false};
};
}  // namespace Gui
