// SPDX-License-Identifier: LGPL-2.1-or-later
#pragma once

#include "DocumentPresentationState.h"
#include "PresentationRevision.h"

#include <FCGlobal.h>

#include <cstddef>
#include <cstdint>
#include <string>
#include <type_traits>
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

/** Coin-compatible material channels copied into the presentation cache. */
enum class PresentationMaterialBinding
{
    Overall,
    PerVertex,
    PerFace
};

/** Pointer-free appearance data for one render buffer batch. */
struct GuiExport PresentationRenderMaterial
{
    PresentationMaterialBinding binding {PresentationMaterialBinding::Overall};
    std::vector<float> ambientColor;
    std::vector<float> diffuseColor;
    std::vector<float> specularColor;
    std::vector<float> emissiveColor;
    std::vector<float> shininess;
    std::vector<float> transparency;
};

/**
 * Maps a contiguous topology range to a stable subelement identity for picking
 * and selection without live DocumentObject pointers.
 */
struct GuiExport PresentationRenderSubelementMapping
{
    std::string subelementName;
    std::string selectionPath;
    std::uint32_t firstTriangle {0};
    std::uint32_t triangleCount {0};
};

/**
 * Immutable CPU-side render payload uploaded incrementally by the GUI.
 *
 * Contains vertices, normals, triangle topology, materials, and subelement
 * mappings as copied values with no live model or Coin pointers.
 */
struct GuiExport PresentationRenderBuffer
{
    std::string stableObjectIdentity;
    std::vector<float> vertices;
    std::vector<float> normals;
    std::vector<std::uint32_t> indices;
    /** Triangle counts per face/part, matching SoBrepFaceSet::partIndex semantics. */
    std::vector<std::uint32_t> topology;
    PresentationRenderMaterial material;
    std::vector<PresentationRenderSubelementMapping> subelementMappings;
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

static_assert(!std::is_pointer_v<PresentationRenderBuffer>);
static_assert(std::is_copy_constructible_v<PresentationRenderBuffer>);
static_assert(!std::is_pointer_v<PresentationRenderMaterial>);
static_assert(!std::is_pointer_v<PresentationRenderSubelementMapping>);

}  // namespace Gui
