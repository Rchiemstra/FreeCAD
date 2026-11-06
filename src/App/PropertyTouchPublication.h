// SPDX-License-Identifier: LGPL-2.1-or-later
#pragma once

#include <FCGlobal.h>

namespace App
{

class Property;

/**
 * Skips semantic publication of one property's synthetic touch on this thread.
 *
 * Property::touch() of that property does not publish. Observers still run.
 * A value write, purgeTouched, or status change, including on that same
 * property, still publishes, as does a touch of any other property. The
 * previous property is restored on destruction so nested suppressors work.
 * The pointer is thread-local: the document owner on another thread still
 * publishes.
 *
 * Presentation code uses this for the unchanged label touch after a Python
 * view-proxy attach, so that notification does not move a revision a prepared
 * recompute is about to validate.
 */
class AppExport PropertyTouchPublicationSuppressor
{
public:
    explicit PropertyTouchPublicationSuppressor(const Property& property) noexcept;
    ~PropertyTouchPublicationSuppressor();

    PropertyTouchPublicationSuppressor(const PropertyTouchPublicationSuppressor&) = delete;
    PropertyTouchPublicationSuppressor& operator=(const PropertyTouchPublicationSuppressor&) = delete;

private:
    const Property* _previous;
};

}  // namespace App
