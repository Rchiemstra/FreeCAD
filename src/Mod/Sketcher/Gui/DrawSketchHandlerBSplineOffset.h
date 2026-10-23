// SPDX-License-Identifier: LGPL-2.1-or-later

#pragma once

#include <algorithm>
#include <memory>
#include <stdexcept>
#include <utility>
#include <vector>

#include <QCheckBox>
#include <QFormLayout>
#include <QLabel>
#include <QPushButton>
#include <QVBoxLayout>

#include <Geom_BSplineCurve.hxx>
#include <Standard_Failure.hxx>

#include <App/Document.h>
#include <Base/Exception.h>
#include <Gui/BitmapFactory.h>
#include <Gui/Notifications.h>
#include <Gui/QuantitySpinBox.h>
#include <Gui/Selection/Selection.h>
#include <Gui/Selection/SelectionObject.h>
#include <Mod/Sketcher/App/BSplineOffset.h>
#include <Mod/Sketcher/App/Sketch.h>
#include <Mod/Sketcher/App/SketchObject.h>

#include "DrawSketchHandler.h"

namespace SketcherGui
{

// This snapshot tool deliberately has no mouse-move computation or document preview objects.
class DrawSketchHandlerBSplineOffset: public DrawSketchHandler
{
public:
    explicit DrawSketchHandlerBSplineOffset(Sketcher::SketchObject& sketch, int sourceId)
        : sourceTag(sketch.getGeometry(sourceId)->getTag())
        , savedSelection(Gui::Selection().getSelectionEx())
    {
        const auto* curve = sketch.getGeometry<Part::GeomBSplineCurve>(sourceId);
        // Reject excessive input before copying, even at activation. Keep this
        // baseline across preview updates so source edits never silently retarget.
        if (Sketcher::checkBSplineOffsetInput(*curve) == Sketcher::BSplineOffsetStatus::Success) {
            sourceCopy.reset(static_cast<Part::GeomBSplineCurve*>(curve->clone()));
        }
    }

    void quit() override
    {
        // purgeHandler destroys this handler and then clears the selection.
        const auto selection = savedSelection;
        DrawSketchHandler::quit();
        restoreSelection(selection);
    }

    void deactivate() override
    {
        // The base implementation solves/recomputes the document even on Cancel.
        // This snapshot tool has no pending document changes to flush.
        abortCommand();
        Gui::ToolHandler::deactivate();
        clearEdit();
        clearEditMarkers();
        clearLineExtensionAutoConstraintHintDrawing();
        resetParallelPerpendicularHint();
        resetPositionText();
        setAngleSnapping(false);
        exiting = true;
        signalToolChanged();
    }

    void mouseMove(SnapManager::SnapHandle) override
    {}

    bool pressButton(Base::Vector2d) override
    {
        return true;
    }

    bool releaseButton(Base::Vector2d) override
    {
        return true;
    }

private:
    void preActivated() override
    {
        // Numeric-only input needs neither the origin marker nor disabled
        // constraint picking. Leave both alone, matching our mutation-free exit.
        signalToolChanged();
    }

    std::string getToolName() const override
    {
        return exiting ? "DSH_None" : "DSH_BSplineOffset";
    }

    bool isWidgetVisible() const override
    {
        return true;
    }

    QString getToolWidgetText() const override
    {
        return QObject::tr("Approximate B-spline - one side");
    }

    QPixmap getToolIcon() const override
    {
        return Gui::BitmapFactory().pixmap("Sketcher_Offset");
    }

    std::unique_ptr<QWidget> createWidget() const override
    {
        auto panel = std::make_unique<QWidget>();
        panel->setObjectName(QStringLiteral("BSplineOffsetPanel"));
        // Also useful to verify that Apply reuses a validated preview from the real UI.
        panel->setProperty("computationCount", 0);
        auto layout = new QVBoxLayout(panel.get());
        auto description = new QLabel(
            QObject::tr("Independent approximation. The source is preserved; the result is blocked."),
            panel.get()
        );
        description->setWordWrap(true);
        layout->addWidget(description);
        auto form = new QFormLayout();
        auto distance = new Gui::QuantitySpinBox(panel.get());
        distance->setObjectName(QStringLiteral("BSplineOffsetDistance"));
        distance->setDecimals(7);
        distance->setRange(-1e9, 1e9);
        distance->setUnit(Base::Unit::Length);
        distance->setValue(1.0);
        form->addRow(QObject::tr("Signed distance"), distance);
        auto tolerance = new Gui::QuantitySpinBox(panel.get());
        tolerance->setObjectName(QStringLiteral("BSplineOffsetTolerance"));
        tolerance->setDecimals(7);
        tolerance->setRange(0.0, 1e9);
        tolerance->setUnit(Base::Unit::Length);
        tolerance->setValue(0.01);
        form->addRow(QObject::tr("Tolerance"), tolerance);
        layout->addLayout(form);
        const Sketcher::BSplineOffsetOptions limits;
        auto limitText = new QLabel(
            QObject::tr("Limits: %1 input poles, %2 output poles, %3 spans, output degree %4 "
                        "(input degree 14), %5 validation spans, refinement depth %6. "
                        "Tolerance must be at least %7 and less than half the distance.")
                .arg(limits.maxInputPoles)
                .arg(limits.maxOutputPoles)
                .arg(limits.maxSegments)
                .arg(limits.maxDegree)
                .arg(limits.maxValidationSpans)
                .arg(limits.maxRefinementDepth)
                .arg(lengthText(1e-7)),
            panel.get()
        );
        limitText->setWordWrap(true);
        layout->addWidget(limitText);
        auto reverse = new QCheckBox(QObject::tr("Reverse side"), panel.get());
        reverse->setObjectName(QStringLiteral("BSplineOffsetReverse"));
        layout->addWidget(reverse);
        auto side = new QLabel(panel.get());
        side->setObjectName(QStringLiteral("BSplineOffsetSide"));
        if (closedSource()) {
            side->setText(
                QObject::tr("Inside / outside: press Update preview to determine the side.")
            );
        }
        layout->addWidget(side);
        auto status = new QLabel(
            QObject::tr("Press Update preview to compute the approximation."),
            panel.get()
        );
        status->setObjectName(QStringLiteral("BSplineOffsetStatus"));
        status->setWordWrap(true);
        layout->addWidget(status);
        for (const auto& control :
             {std::make_pair("BSplineOffsetUpdate", QObject::tr("Update preview")),
              std::make_pair("BSplineOffsetApply", QObject::tr("Apply")),
              std::make_pair("BSplineOffsetCancel", QObject::tr("Cancel"))}) {
            auto button = new QPushButton(control.second, panel.get());
            button->setObjectName(QString::fromLatin1(control.first));
            layout->addWidget(button);
        }
        return panel;
    }

    template<class T> T* control(const char* name) const
    {
        return toolwidget->findChild<T*>(QString::fromLatin1(name));
    }

    void onWidgetChanged() override
    {
        // TaskSketcherTool retains the hidden panel after the handler quits.
        // Disconnect its callbacks when this handler or its widget binding ends.
        connections = std::make_unique<QObject>();
        if (!toolwidget) {
            return;
        }
        auto invalidate = [this]() {
            preview = {};
            clearEdit();
            updateSide();
            setStatus(QObject::tr("Inputs changed. Press Update preview."));
        };
        for (const char* name : {"BSplineOffsetDistance", "BSplineOffsetTolerance"}) {
            QObject::connect(
                control<Gui::QuantitySpinBox>(name),
                qOverload<double>(&Gui::QuantitySpinBox::valueChanged),
                connections.get(),
                invalidate
            );
        }
        QObject::connect(
            control<QCheckBox>("BSplineOffsetReverse"),
            &QCheckBox::toggled,
            connections.get(),
            invalidate
        );
        QObject::connect(
            control<QPushButton>("BSplineOffsetUpdate"),
            &QPushButton::clicked,
            connections.get(),
            [this]() { updatePreview(); }
        );
        QObject::connect(
            control<QPushButton>("BSplineOffsetApply"),
            &QPushButton::clicked,
            connections.get(),
            [this]() { apply(); }
        );
        QObject::connect(
            control<QPushButton>("BSplineOffsetCancel"),
            &QPushButton::clicked,
            connections.get(),
            [this]() { quit(); }
        );
    }

    void setStatus(const QString& message)
    {
        control<QLabel>("BSplineOffsetStatus")->setText(message);
    }

    static void restoreSelection(const std::vector<Gui::SelectionObject>& selection)
    {
        Gui::Selection().clearSelection();
        for (const auto& item : selection) {
            Gui::Selection().addSelection(item);
        }
    }

    const Part::GeomBSplineCurve* source()
    {
        for (const auto* geometry : getSketchObject()->Geometry.getValues()) {
            if (geometry->getTag() == sourceTag) {
                return dynamic_cast<const Part::GeomBSplineCurve*>(geometry);
            }
        }
        return nullptr;
    }

    static QString lengthText(double value)
    {
        Base::Quantity quantity(value, Base::Unit::Length);
        quantity.setFormat(Base::QuantityFormat(Base::QuantityFormat::Scientific, 7));
        return QString::fromStdString(quantity.getSafeUserString());
    }

    static QString statusMessage(Sketcher::BSplineOffsetStatus status)
    {
        using Status = Sketcher::BSplineOffsetStatus;
        switch (status) {
            case Status::Success:
                return QObject::tr(
                    "Independent approximate B-spline offset verified within tolerance."
                );
            case Status::SeamCorner:
                return QObject::tr("The closed B-spline has a G0-only corner at the seam; "
                                   "a G1 tangent is required.");
            case Status::InvalidOptions:
                return QObject::tr(
                           "Use finite distance and tolerance at least %1 and less than half "
                           "the absolute distance."
                )
                    .arg(lengthText(1e-7));
            case Status::UnsupportedCurve:
                return QObject::tr("Select one finite, planar, open, nonperiodic C2 B-spline with "
                                   "positive weights.");
            case Status::ComplexityExceeded:
                return QObject::tr("The spline exceeds the displayed complexity limits; simplify "
                                   "it or relax tolerance.");
            case Status::ZeroTangent:
                return QObject::tr("A zero tangent or unresolved regularity prevents this offset.");
            case Status::SingularOffset:
                return QObject::tr("A singular offset cannot be excluded; reduce the distance.");
            case Status::SourceSelfIntersection:
                return QObject::tr(
                    "Source spline crossing or near-contact cannot be excluded; "
                    "inside/outside is undefined."
                );
            case Status::SelfIntersection:
                return QObject::tr(
                    "Offset crossing or near-contact cannot be excluded; reduce the distance."
                );
            case Status::VerificationFailed:
                return QObject::tr(
                    "The offset error could not be bounded within tolerance; relax tolerance."
                );
            case Status::ApproximationFailed:
                return QObject::tr(
                    "Unable to fit the offset within tolerance and complexity limits."
                );
        }
        return QObject::tr("Unable to compute the B-spline offset.");
    }

    static bool sameSource(const Part::GeomBSplineCurve& a, const Part::GeomBSplineCurve& b)
    {
        // Compare the full oriented representation exactly, including knot multiplicities.
        // A source edit smaller than the fit tolerance must still invalidate the preview.
        const auto aPoles = a.getPoles();
        const auto bPoles = b.getPoles();
        const bool samePoles = std::equal(
            aPoles.begin(),
            aPoles.end(),
            bPoles.begin(),
            bPoles.end(),
            [](const auto& p, const auto& q) { return p.x == q.x && p.y == q.y && p.z == q.z; }
        );
        return a.getDegree() == b.getDegree() && a.isPeriodic() == b.isPeriodic() && samePoles
            && a.getWeights() == b.getWeights() && a.getKnots() == b.getKnots()
            && a.getMultiplicities() == b.getMultiplicities();
    }

    bool closedSource() const
    {
        return sourceCopy && Handle(Geom_BSplineCurve)::DownCast(sourceCopy->handle())->IsClosed();
    }

    void updateSide()
    {
        if (!closedSource()) {
            return;
        }
        double distance = control<Gui::QuantitySpinBox>("BSplineOffsetDistance")->rawValue();
        if (control<QCheckBox>("BSplineOffsetReverse")->isChecked()) {
            distance = -distance;
        }
        auto label = control<QLabel>("BSplineOffsetSide");
        if (orientation == 0 || distance == 0.0) {
            label->setText(
                QObject::tr("Inside / outside: press Update preview to determine the side.")
            );
        }
        else {
            label->setText(
                orientation * distance > 0.0 ? QObject::tr("Side: Outside")
                                             : QObject::tr("Side: Inside")
            );
        }
    }

    bool updatePreview()
    {
        try {
            const auto* current = source();
            if (!current) {
                Gui::NotifyUserError(
                    getSketchObject(),
                    QT_TRANSLATE_NOOP("Notifications", "B-spline offset cancelled"),
                    QT_TRANSLATE_NOOP("Notifications", "The selected B-spline was removed or changed.")
                );
                quit();
                return false;
            }
            Sketcher::BSplineOffsetOptions options;
            options.distance = control<Gui::QuantitySpinBox>("BSplineOffsetDistance")->rawValue();
            options.tolerance = control<Gui::QuantitySpinBox>("BSplineOffsetTolerance")->rawValue();
            if (control<QCheckBox>("BSplineOffsetReverse")->isChecked()) {
                options.distance = -options.distance;
            }
            const auto check = Sketcher::checkBSplineOffsetInput(*current, options);
            if (!control<Gui::QuantitySpinBox>("BSplineOffsetDistance")->hasValidInput()
                || !control<Gui::QuantitySpinBox>("BSplineOffsetTolerance")->hasValidInput()) {
                fail(statusMessage(Sketcher::BSplineOffsetStatus::InvalidOptions));
                return false;
            }
            if (check != Sketcher::BSplineOffsetStatus::Success) {
                fail(statusMessage(check));
                return false;
            }
            if (sourceCopy && !sameSource(*current, *sourceCopy)) {
                Gui::NotifyUserError(
                    getSketchObject(),
                    QT_TRANSLATE_NOOP("Notifications", "B-spline offset cancelled"),
                    QT_TRANSLATE_NOOP("Notifications", "The selected B-spline was removed or changed.")
                );
                quit();
                return false;
            }
            if (!sourceCopy) {
                // Oversized input was rejected at activation without a copy.
                fail(statusMessage(Sketcher::BSplineOffsetStatus::ComplexityExceeded));
                return false;
            }
            if (!preview.curve || options.distance != previewDistance
                || options.tolerance != previewTolerance) {
                clearEdit();
                preview = {};
                toolwidget->setProperty(
                    "computationCount",
                    toolwidget->property("computationCount").toInt() + 1
                );
                preview = Sketcher::makeBSplineOffset(*sourceCopy, options);
                previewDistance = options.distance;
                previewTolerance = options.tolerance;
            }
            if (!preview.curve) {
                fail(statusMessage(preview.status));
                return false;
            }
            orientation = preview.orientation;
            updateSide();
            drawEdit(std::vector<Part::Geometry*> {preview.curve.get()});
            setStatus(QObject::tr("Achieved maximum error: %1 (tolerance: %2)")
                          .arg(lengthText(preview.achievedMaxError))
                          .arg(lengthText(options.tolerance)));
            return true;
        }
        catch (const Standard_Failure& e) {
            fail(QString::fromUtf8(e.GetMessageString()));
        }
        catch (const Base::Exception& e) {
            fail(QString::fromUtf8(e.what()));
        }
        catch (const std::exception& e) {
            fail(QString::fromUtf8(e.what()));
        }
        catch (...) {
            fail(QObject::tr("Unable to compute the B-spline offset."));
        }
        return false;
    }

    void fail(const QString& message)
    {
        preview = {};
        orientation = 0;
        updateSide();
        clearEdit();
        setStatus(message);
        restoreSelection(savedSelection);
    }

    void apply()
    {
        // Fitting and verification finish before opening the only document transaction.
        if (!updatePreview()) {
            return;
        }
        bool transactionOpen = false;
        try {
            auto sketch = getSketchObject();
            const int resultId = sketch->Geometry.getSize();
            auto block = std::make_unique<Sketcher::Constraint>();
            block->Type = Sketcher::Block;
            block->First = resultId;
            // Detached solver owns clones, including all external references.
            // Validate both solves and source/result stability before any transaction.
            auto geometry = sketch->getCompleteGeometry();
            geometry.insert(geometry.begin() + resultId, preview.curve.get());
            auto constraints = sketch->Constraints.getValues();
            constraints.push_back(block.get());
            Sketcher::Sketch validation;
            const int dof
                = validation.setUpSketch(geometry, constraints, sketch->getExternalGeometryCount());
            if (dof < 0 || validation.hasConflicts() || validation.hasRedundancies()
                || validation.hasMalformedConstraints()
                || validation.solve() != GCS::SolveStatus::Success
                || validation.solve() != GCS::SolveStatus::Success) {
                throw std::runtime_error(
                    QObject::tr("Unable to solve and block the offset curve.").toStdString()
                );
            }
            // Exercise shape generation here as well, without a document recompute.
            validation.toShape();
            auto solved = validation.extractGeometry();
            std::vector<std::unique_ptr<Part::Geometry>> owned;
            for (auto* curve : solved) {
                owned.emplace_back(curve);
            }
            for (int i = 0; i <= resultId; ++i) {
                const auto* before = dynamic_cast<const Part::GeomBSplineCurve*>(geometry[i]);
                const auto* after = dynamic_cast<const Part::GeomBSplineCurve*>(solved.at(i));
                if (before && (!after || !sameSource(*before, *after))) {
                    throw std::runtime_error(
                        QObject::tr("Solving would change a B-spline; solve the sketch first.")
                            .toStdString()
                    );
                }
            }
            openCommand(QT_TRANSLATE_NOOP("Command", "Approximate B-spline offset"));
            if (currentTransactionID == App::NullTransaction) {
                throw std::runtime_error(
                    QObject::tr("Unable to open the offset transaction.").toStdString()
                );
            }
            transactionOpen = true;
            // These are the only document mutations. Core clears redo on the
            // first mutation; an exceptional allocation/add failure must abort,
            // but cannot restore redo without changing App::Document.
            const int id = sketch->addGeometry(preview.curve.get(), false);
            if (id != resultId || sketch->addConstraint(std::move(block)) < 0) {
                throw std::runtime_error(
                    QObject::tr("Unable to add and block the offset curve.").toStdString()
                );
            }
            commitCommand();
            transactionOpen = false;
        }
        catch (const Standard_Failure& e) {
            if (transactionOpen) {
                abortCommand();
            }
            fail(QString::fromUtf8(e.GetMessageString()));
            return;
        }
        catch (const Base::Exception& e) {
            if (transactionOpen) {
                abortCommand();
            }
            fail(QString::fromUtf8(e.what()));
            return;
        }
        catch (const std::exception& e) {
            if (transactionOpen) {
                abortCommand();
            }
            fail(QString::fromUtf8(e.what()));
            return;
        }
        catch (...) {
            if (transactionOpen) {
                abortCommand();
            }
            fail(QObject::tr("Unable to apply the B-spline offset."));
            return;
        }
        // No document solve/recompute is needed: the detached validation is complete.
        DrawSketchHandler::quit();
    }

    bool exiting = false;
    int orientation = 0;
    const boost::uuids::uuid sourceTag;
    const std::vector<Gui::SelectionObject> savedSelection;
    std::unique_ptr<QObject> connections;
    std::unique_ptr<Part::GeomBSplineCurve> sourceCopy;
    Sketcher::BSplineOffsetResult preview;
    double previewDistance = 0.0;
    double previewTolerance = 0.0;
};

}  // namespace SketcherGui
