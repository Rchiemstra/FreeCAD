// SPDX-License-Identifier: LGPL-2.1-or-later
#pragma once

#include <Inventor/nodes/SoGroup.h>

namespace PartGui::PreviewExtensionInternal
{

/// Attach @p previewRoot below @p annotation unless it is already there.
inline void attachPreviewRoot(SoGroup* annotation, SoNode* previewRoot)
{
    if (annotation && previewRoot && annotation->findChild(previewRoot) < 0) {
        annotation->addChild(previewRoot);
    }
}

/// Detach @p previewRoot from @p annotation if it is attached.
///
/// Coin posts an error for removing a missing child, and features whose
/// preview was never shown are deleted routinely (undo, closing documents).
inline void detachPreviewRoot(SoGroup* annotation, SoNode* previewRoot)
{
    if (!annotation || !previewRoot) {
        return;
    }
    const int index = annotation->findChild(previewRoot);
    if (index >= 0) {
        annotation->removeChild(index);
    }
}

}  // namespace PartGui::PreviewExtensionInternal
