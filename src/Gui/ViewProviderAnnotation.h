/***************************************************************************
 *   Copyright (c) 2008 Werner Mayer <wmayer[at]users.sourceforge.net>     *
 *                                                                         *
 *   This file is part of the FreeCAD CAx development system.              *
 *                                                                         *
 *   This library is free software; you can redistribute it and/or         *
 *   modify it under the terms of the GNU Library General Public           *
 *   License as published by the Free Software Foundation; either          *
 *   version 2 of the License, or (at your option) any later version.      *
 *                                                                         *
 *   This library  is distributed in the hope that it will be useful,      *
 *   but WITHOUT ANY WARRANTY; without even the implied warranty of        *
 *   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the         *
 *   GNU Library General Public License for more details.                  *
 *                                                                         *
 *   You should have received a copy of the GNU Library General Public     *
 *   License along with this library; see the file COPYING.LIB. If not,    *
 *   write to the Free Software Foundation, Inc., 59 Temple Place,         *
 *   Suite 330, Boston, MA  02111-1307, USA                                *
 *                                                                         *
 ***************************************************************************/


#pragma once

#include "ViewProviderDocumentObject.h"
#include <App/PropertyUnits.h>
#include <Base/Vector3D.h>
#include <optional>
#include "SoTextLabel.h"

class SoFont;
class SoText2;
class SoAsciiText;
class SoBaseColor;
class SoTranslation;
class SoTransform;
class SoRotationXYZ;
class SoImage;
class SoCoordinate3;
class SoDragger;

namespace Gui
{

class GuiExport ViewProviderAnnotation: public ViewProviderDocumentObject
{
    PROPERTY_HEADER_WITH_OVERRIDE(Gui::ViewProviderAnnotation);

public:
    /// Constructor
    ViewProviderAnnotation();
    ~ViewProviderAnnotation() override;

    // Display properties
    App::PropertyColor TextColor;
    App::PropertyEnumeration Justification;
    App::PropertyFloat FontSize;
    App::PropertyFont FontName;
    App::PropertyFloat LineSpacing;
    App::PropertyAngle Rotation;
    App::PropertyEnumeration RotationAxis;

    void attach(App::DocumentObject*) override;
    void updateData(const App::Property*) override;
    std::vector<std::string> getDisplayModes() const override;
    void setDisplayMode(const char* ModeName) override;

protected:
    void onChanged(const App::Property* prop) override;

private:
    SoFont* pFont;
    SoText2* pLabel;
    SoAsciiText* pLabel3d;
    SoBaseColor* pColor;
    SoTranslation* pTranslation;
    SoRotationXYZ* pRotationXYZ;

    static const char* JustificationEnums[];
    static const char* RotationAxisEnums[];
};

/**
 * This is a different implementation of an annotation object which uses an
 * SoImage node instead of an SoText2 or SoAsciiText node.
 * This approach gives a bit more flexibility since it can render arbitrary
 * annotations.
 */
class GuiExport ViewProviderAnnotationLabel: public ViewProviderDocumentObject
{
    PROPERTY_HEADER_WITH_OVERRIDE(Gui::ViewProviderAnnotationLabel);

public:
    /// Constructor
    ViewProviderAnnotationLabel();
    ~ViewProviderAnnotationLabel() override;

    // Display properties
    App::PropertyColor TextColor;
    App::PropertyColor BackgroundColor;
    App::PropertyEnumeration Justification;
    App::PropertyFloat FontSize;
    App::PropertyFont FontName;
    App::PropertyBool Frame;

    void attach(App::DocumentObject*) override;
    void updateData(const App::Property*) override;
    std::vector<std::string> getDisplayModes() const override;
    void setDisplayMode(const char* ModeName) override;

protected:
    struct DragState
    {
        Base::Vector3d basePosition;
        Base::Vector3d currentTextPosition;
        Base::Vector3d pickOffset;
        Base::Vector3d planePoint;
        Base::Vector3d planeNormal;
    };

    void onChanged(const App::Property* prop) override;
    virtual void drawImage(const std::vector<std::string>&);

    /** Map a world-space point into BasePosition/TextPosition space (identity by default). */
    virtual Base::Vector3d worldToAnnotationPoint(const Base::Vector3d& world) const;

    /** Leader end relative to BasePosition. Default is the text/image origin. */
    virtual Base::Vector3d leaderEndpoint(const Base::Vector3d& textPosition) const;

    /** Return false to cancel label drag (e.g. clickable @ref hit). */
    virtual bool acceptLabelDragStart(SoDragger* drag, DragState& state);

    /** Called after a completed label drag, before TextPosition is committed.

        Subclasses must sync any derived leader/visual properties from
        ``state.currentTextPosition`` here so observers of TextPosition never
        see a new text box with a stale leader endpoint.
     */
    virtual void onLabelDragFinished(const DragState& state);

    void previewTextPosition(DragState& state, const Base::Vector3d& textPosition);
    /** Update leader polyline for the given text position (override to sync handles). */
    virtual void setLeaderCoords(const Base::Vector3d& textPosition);

    SoCoordinate3* pCoords;
    SoImage* pImage;
    SoImage* pImageHitProxy;
    SoBaseColor* pColor;
    SoTranslation* pBaseTranslation;
    TranslateManip* pTextTranslation;
    std::optional<DragState> dragState;

    /** Last drawn image size in pixels (0 if hidden/empty). */
    int labelImageWidth = 0;
    int labelImageHeight = 0;

private:
    static void dragStartCallback(void* data, SoDragger* d);
    static void dragFinishCallback(void* data, SoDragger* d);
    static void dragMotionCallback(void* data, SoDragger* d);

    static const char* JustificationEnums[];
};

}  // namespace Gui
