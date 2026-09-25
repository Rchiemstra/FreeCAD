// SPDX-License-Identifier: LGPL-2.1-or-later
#pragma once

#include "DocumentPresentationState.h"
#include "PresentationRevision.h"

#include <FCGlobal.h>

#include <cstdint>
#include <string>
#include <vector>

namespace Gui
{

/** One pointer-free tree node in a flat presentation tree descriptor. */
struct GuiExport PresentationTreeNode
{
    std::string stableObjectIdentity;
    std::string label;
    std::string iconKey;
    std::vector<std::size_t> childIndices;
    bool visible {true};
    bool expanded {false};
};

/** One copied display property value shown by the presentation cache. */
struct GuiExport PresentationPropertyValue
{
    std::string stableObjectIdentity;
    std::string propertyName;
    std::string displayValue;
    std::string statusAnnotation;
};

/** Stable selection mapping that does not depend on live DocumentObject pointers. */
struct GuiExport PresentationSelectionMapping
{
    std::string stableObjectIdentity;
    std::string subelementName;
    std::string selectionPath;
};

/** Immutable CPU-side render payload uploaded incrementally by the GUI. */
struct GuiExport PresentationRenderBuffer
{
    std::string stableObjectIdentity;
    std::vector<float> vertices;
    std::vector<float> normals;
    std::vector<std::uint32_t> indices;
    std::vector<float> diffuseColor;
};

/**
 * Pointer-free presentation packet delivered to the GUI cache.
 *
 * Tree descriptors, display property values, status and error data, selection
 * mappings, and immutable render buffers are copied values. No live Document,
 * DocumentObject, Property, or Coin node pointers cross this boundary.
 */
struct GuiExport PresentationDelta
{
    PresentationRevision revision;
    DocumentPresentationStatus status;
    std::vector<PresentationTreeNode> tree;
    std::vector<PresentationPropertyValue> properties;
    std::vector<PresentationSelectionMapping> selectionMappings;
    std::vector<PresentationRenderBuffer> renderBuffers;

    [[nodiscard]] bool pointerFree() const noexcept
    {
        return true;
    }
};

}  // namespace Gui
