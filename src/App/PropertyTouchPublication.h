// SPDX-License-Identifier: LGPL-2.1-or-later
#pragma once

#include <FCGlobal.h>

namespace App
{

class Property;

/**
 * Skips semantic publication of one property on this thread.
 *
 * Observers still run, and a write to any other property still publishes.
 * The previous property is restored on destruction so nested suppressors work.
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
