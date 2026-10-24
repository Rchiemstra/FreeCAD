// SPDX-License-Identifier: LGPL-2.1-or-later

#pragma once

#include <algorithm>
#include <memory>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

#include <Mod/Sketcher/App/LinkedOffset.h>

#include <QCheckBox>
#include <QFormLayout>
#include <QLabel>
#include <QPushButton>
#include <QVBoxLayout>

#include <BRepBuilderAPI_MakeFace.hxx>
#include <BRepBuilderAPI_MakeWire.hxx>
#include <BRepTopAdaptor_FClass2d.hxx>
#include <TopoDS.hxx>
#include <gp_Pln.hxx>

#include <App/Document.h>
#include <Base/Exception.h>
#include <Gui/BitmapFactory.h>
#include <Gui/QuantitySpinBox.h>
#include <Gui/Selection/Selection.h>
#include <Gui/Selection/SelectionObject.h>
#include <Mod/Sketcher/App/BSplineOffset.h>
#include <Mod/Sketcher/App/GeometryFacade.h>
#include <Mod/Sketcher/App/OffsetChain.h>
#include <Mod/Sketcher/App/Sketch.h>
#include <Mod/Sketcher/App/SketchObject.h>

#include "DrawSketchHandler.h"

namespace SketcherGui
{

// Numeric snapshot adapter: previews and solves own detached geometry. Only Apply
// opens a transaction, after generation, certificates and solver stability checks.
class DrawSketchHandlerOffsetChain: public DrawSketchHandler
{
public:
    DrawSketchHandlerOffsetChain(Sketcher::SketchObject& sketch, const std::vector<int>& ids)
        : savedSelection(Gui::Selection().getSelectionEx())
    {
        if (ids.size() > 256) {
            initialReason = "Select at most 256 mixed edges.";
            return;
        }
        for (const int id : ids) {
            const auto* curve = dynamic_cast<const Part::GeomCurve*>(sketch.getGeometry(id));
            if (id < 0 || !curve || Sketcher::GeometryFacade::isInternalAligned(curve)) {
                initialReason = "External or unsupported selected geometry.";
                break;
            }
            if (const auto* spline = dynamic_cast<const Part::GeomBSplineCurve*>(curve)) {
                if (Sketcher::checkBSplineOffsetInput(*spline)
                    != Sketcher::BSplineOffsetStatus::Success) {
                    initialReason = "Unsupported source spline.";
                    break;
                }
            }
            sources.push_back({
                curve->getTag(), static_cast<size_t>(Sketcher::GeometryFacade::getId(curve)),
                std::unique_ptr<Part::GeomCurve>(static_cast<Part::GeomCurve*>(curve->clone()))
            });
        }
    }

    void quit() override
    {
        const auto selection = savedSelection;
        DrawSketchHandler::quit();
        restoreSelection(selection);
    }

    void deactivate() override
    {
        // Like the single-spline adapter, Cancel must not solve the live sketch.
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
        signalToolChanged();
    }
    std::string getToolName() const override
    {
        return exiting ? "DSH_None" : "DSH_OffsetChain";
    }
    bool isWidgetVisible() const override
    {
        return true;
    }
    QString getToolWidgetText() const override
    {
        return QObject::tr("Mixed-chain offset - one side");
    }
    QPixmap getToolIcon() const override
    {
        return Gui::BitmapFactory().pixmap("Sketcher_Offset");
    }

    std::unique_ptr<QWidget> createWidget() const override
    {
        auto panel = std::make_unique<QWidget>();
        panel->setObjectName(QStringLiteral("OffsetChainPanel"));
        panel->setProperty("computationCount", 0);
        auto layout = new QVBoxLayout(panel.get());
        auto description = new QLabel(QObject::tr(
            "Persistent G1 chain offset snapshot. Refresh is not automatic."), panel.get());
        description->setWordWrap(true);
        layout->addWidget(description);
        auto form = new QFormLayout();
        for (const char* name : {"Distance", "Tolerance"}) {
            auto spin = new Gui::QuantitySpinBox(panel.get());
            spin->setObjectName(QStringLiteral("OffsetChain") + QString::fromLatin1(name));
            spin->setDecimals(7);
            spin->setRange(name == std::string("Distance") ? -1e9 : 0.0, 1e9);
            spin->setUnit(Base::Unit::Length);
            spin->setValue(name == std::string("Distance") ? 1.0 : 0.01);
            form->addRow(name == std::string("Distance") ? QObject::tr("Signed distance")
                                                        : QObject::tr("Tolerance"), spin);
        }
        layout->addLayout(form);
        const std::vector<std::pair<const char*, QString>> checks {
            {"Reverse", QObject::tr("Reverse side")},
            {"Intersection", QObject::tr("Intersection (not supported)")},
            {"Clearance", QObject::tr("Constrained Clearance (not supported)")},
            {"Driving", QObject::tr("Driving offset constraints (not supported)")},
            {"Delete", QObject::tr("Delete original (not supported)")}
        };
        for (const auto& [name, text] : checks) {
            auto check = new QCheckBox(text, panel.get());
            check->setObjectName(QStringLiteral("OffsetChain") + QString::fromLatin1(name));
            layout->addWidget(check);
        }
        auto side = new QLabel(
            QObject::tr("Side follows the ordered source tangent."), panel.get());
        side->setObjectName(QStringLiteral("OffsetChainSide"));
        layout->addWidget(side);
        auto status = new QLabel(QObject::tr("Press Update preview to certify the offset."),
                                 panel.get());
        status->setObjectName(QStringLiteral("OffsetChainStatus"));
        status->setWordWrap(true);
        layout->addWidget(status);
        const std::vector<std::pair<const char*, QString>> buttons {
            {"Update", QObject::tr("Update preview")},
            {"Apply", QObject::tr("Apply")},
            {"Cancel", QObject::tr("Cancel")}
        };
        for (const auto& [name, text] : buttons) {
            auto button = new QPushButton(text, panel.get());
            button->setObjectName(QStringLiteral("OffsetChain") + QString::fromLatin1(name));
            layout->addWidget(button);
        }
        return panel;
    }

    template<class T> T* control(const char* suffix) const
    {
        return toolwidget->findChild<T*>(QStringLiteral("OffsetChain")
                                        + QString::fromLatin1(suffix));
    }

    void onWidgetChanged() override
    {
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
        for (const char* name : {"Distance", "Tolerance"}) {
            QObject::connect(control<Gui::QuantitySpinBox>(name),
                             qOverload<double>(&Gui::QuantitySpinBox::valueChanged),
                             connections.get(), invalidate);
        }
        for (const char* name : {"Reverse", "Intersection", "Clearance", "Driving", "Delete"}) {
            QObject::connect(control<QCheckBox>(name), &QCheckBox::toggled,
                             connections.get(), invalidate);
        }
        QObject::connect(control<QPushButton>("Update"), &QPushButton::clicked,
                         connections.get(), [this]() { updatePreview(); });
        QObject::connect(control<QPushButton>("Apply"), &QPushButton::clicked,
                         connections.get(), [this]() { apply(); });
        QObject::connect(control<QPushButton>("Cancel"), &QPushButton::clicked,
                         connections.get(), [this]() { quit(); });
    }

    static void restoreSelection(const std::vector<Gui::SelectionObject>& selection)
    {
        Gui::Selection().clearSelection();
        for (const auto& item : selection) {
            Gui::Selection().addSelection(item);
        }
    }

    void setStatus(const QString& text)
    {
        control<QLabel>("Status")->setText(text);
    }

    void fail(const QString& reason)
    {
        preview = {};
        orientation = 0;
        updateSide();
        clearEdit();
        setStatus(QObject::tr("Mixed-chain offset could not be certified: %1").arg(reason));
        restoreSelection(savedSelection);
    }

    static bool sameGeometry(const Part::Geometry& a, const Part::Geometry& b)
    {
        if (a.getTypeId() != b.getTypeId() || !a.isSame(b, 0.0, 0.0)) {
            return false;
        }
        // isSame does not compare spline multiplicities or oriented endpoints.
        if (const auto* spline = dynamic_cast<const Part::GeomBSplineCurve*>(&a)) {
            const auto& other = static_cast<const Part::GeomBSplineCurve&>(b);
            if (spline->getMultiplicities() != other.getMultiplicities()) {
                return false;
            }
        }
        if (const auto* curve = dynamic_cast<const Part::GeomCurve*>(&a)) {
            const auto& other = static_cast<const Part::GeomCurve&>(b);
            return curve->getFirstParameter() == other.getFirstParameter()
                && curve->getLastParameter() == other.getLastParameter()
                && curve->value(curve->getFirstParameter())
                    == other.value(other.getFirstParameter())
                && curve->value(curve->getLastParameter()) == other.value(other.getLastParameter());
        }
        return true;
    }

    void checkSources()
    {
        if (!initialReason.empty()) {
            throw std::runtime_error(initialReason);
        }
        const auto& geometry = getSketchObject()->Geometry.getValues();
        for (const auto& source : sources) {
            const auto found = std::find_if(geometry.begin(), geometry.end(), [&](const auto* g) {
                return g->getTag() == source.tag;
            });
            if (found != geometry.end() && Sketcher::GeometryFacade::isInternalAligned(*found)) {
                throw std::runtime_error("Internal helper geometry is not supported.");
            }
            if (found != geometry.end() && Sketcher::GeometryFacade::getConstruction(*found)) {
                throw std::runtime_error("Construction geometry is not supported.");
            }
            if (found == geometry.end() || !sameGeometry(*source.geometry, **found)) {
                throw std::runtime_error("The selected source was removed or changed.");
            }
        }
    }

    double distance() const
    {
        const auto value = control<Gui::QuantitySpinBox>("Distance")->rawValue();
        return control<QCheckBox>("Reverse")->isChecked() ? -value : value;
    }

    void updateSide()
    {
        auto side = control<QLabel>("Side");
        if (orientation != 0 && distance() != 0) {
            side->setText(orientation * distance() > 0 ? QObject::tr("Side: Outside")
                                                      : QObject::tr("Side: Inside"));
        }
        else {
            side->setText(QObject::tr("Side follows the ordered source tangent; "
                                      "closed chains show inside / outside after certification."));
        }
    }

    bool updatePreview()
    {
        try {
            checkSources();
            for (const char* name : {"Intersection", "Clearance", "Driving", "Delete"}) {
                if (control<QCheckBox>(name)->isChecked()) {
                    throw std::runtime_error("The selected option is not supported.");
                }
            }
            for (const char* name : {"Distance", "Tolerance"}) {
                if (!control<Gui::QuantitySpinBox>(name)->hasValidInput()) {
                    throw std::runtime_error("Invalid numeric input.");
                }
            }
            if (preview.pieces.empty()) {
                std::vector<Sketcher::OffsetChainPiece> input;
                for (const auto& source : sources) {
                    input.push_back({source.geometry.get(), source.identity});
                }
                auto ordered = Sketcher::orderOffsetChain(input);
                if (ordered.status != Sketcher::OffsetChainStatus::Success) {
                    throw std::runtime_error(ordered.reason);
                }
                input.clear();
                orderedSources.clear();
                linkedClosed = ordered.closed;
                for (const auto& piece : ordered.pieces) {
                    const auto original = std::find_if(sources.begin(), sources.end(),
                        [&](const auto& source) { return source.identity == piece.identity; });
                    orderedSources.emplace_back(piece.identity,
                        !sameGeometry(*original->geometry, *piece.geometry));
                    input.push_back({piece.geometry.get(), piece.identity});
                }
                Sketcher::OffsetChainOptions options;
                options.closed = ordered.closed;
                options.tolerance = control<Gui::QuantitySpinBox>("Tolerance")->rawValue();
                toolwidget->setProperty("computationCount",
                                       toolwidget->property("computationCount").toInt() + 1);
                preview = Sketcher::makeOffsetChain(input, distance(), options);
                if (preview.status != Sketcher::OffsetChainStatus::Success) {
                    throw std::runtime_error(preview.reason);
                }
                if (ordered.closed) {
                    // Classify the already certified simple planar wire on the sketch plane.
                    // Inside=false preserves its supplied orientation (including clockwise).
                    BRepBuilderAPI_MakeWire wire;
                    for (const auto& piece : ordered.pieces) {
                        wire.Add(TopoDS::Edge(piece.geometry->toShape()));
                    }
                    BRepBuilderAPI_MakeFace face(gp_Pln(gp::XOY()), wire.Wire(), false);
                    BRepTopAdaptor_FClass2d classifier(face.Face(), Precision::Confusion());
                    const auto state = classifier.PerformInfinitePoint();
                    if (state != TopAbs_IN && state != TopAbs_OUT) {
                        throw std::runtime_error("Closed chain orientation is unresolved.");
                    }
                    orientation = state == TopAbs_OUT ? 1 : -1;
                }
            }
            updateSide();
            std::vector<Part::Geometry*> geometry;
            for (const auto& piece : preview.pieces) {
                geometry.push_back(piece.geometry.get());
            }
            drawEdit(geometry);
            setStatus(QObject::tr("Certified mixed-chain offset within tolerance."));
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
            fail(QObject::tr("Unable to compute the offset."));
        }
        return false;
    }

    void apply()
    {
        if (!updatePreview()) {
            return;
        }
        bool transactionOpen = false;
        try {
            auto* sketch = getSketchObject();
            const int first = sketch->Geometry.getSize();
            auto geometry = sketch->getCompleteGeometry();
            std::vector<Sketcher::Constraint*> constraints;
            std::vector<std::unique_ptr<Sketcher::Constraint>> validationConstraints;
            for (const auto* constraint : sketch->Constraints.getValues()) {
                // solve() writes reference dimensions, even in a detached Sketch.
                validationConstraints.emplace_back(constraint->clone());
                constraints.push_back(validationConstraints.back().get());
            }
            std::vector<std::unique_ptr<Sketcher::Constraint>> blocks;
            std::vector<Part::Geometry*> output;
            for (const auto& piece : preview.pieces) {
                auto block = std::make_unique<Sketcher::Constraint>();
                block->Type = Sketcher::Block;
                block->First = first + static_cast<int>(output.size());
                constraints.push_back(block.get());
                blocks.push_back(std::move(block));
                output.push_back(piece.geometry.get());
            }
            geometry.insert(geometry.begin() + first, output.begin(), output.end());
            Sketcher::Sketch validation;
            const int dof = validation.setUpSketch(geometry, constraints,
                                                  sketch->getExternalGeometryCount());
            if (dof < 0 || validation.hasConflicts() || validation.hasRedundancies()
                || validation.hasMalformedConstraints()
                || validation.solve() != GCS::SolveStatus::Success
                || validation.solve() != GCS::SolveStatus::Success) {
                throw std::runtime_error("Unable to solve and block the offset chain.");
            }
            validation.toShape();
            const auto solved = validation.extractGeometry();
            std::vector<std::unique_ptr<Part::Geometry>> owned;
            for (auto* curve : solved) {
                owned.emplace_back(curve);
            }
            for (size_t i = 0; i < solved.size(); ++i) {
                if (!sameGeometry(*geometry.at(i), *solved[i])) {
                    throw std::runtime_error(
                        "Solving would change geometry; solve the sketch first.");
                }
            }
            // Prepare the complete property lists before opening a transaction,
            // which clears redo. Only property installation belongs inside it.
            auto newGeometry = sketch->Geometry.getValues();
            std::vector<std::unique_ptr<Part::Geometry>> additions;
            for (auto*& curve : newGeometry) {
                additions.emplace_back(curve->clone());
                curve = additions.back().get();
            }
            auto newConstraints = sketch->Constraints.getValues();
            for (auto* curve : output) {
                additions.emplace_back(curve->copy());
                auto facade = Sketcher::GeometryFacade::getFacade(additions.back().get());
                facade->setId(0); // onGeometryChanged assigns a fresh sketch identity.
                facade->setConstruction(false);
                facade->setBlocked(true);
                newGeometry.push_back(additions.back().get());
            }
            std::vector<std::pair<int, bool>> linkedSources;
            for (const auto& [identity, reversed] : orderedSources) {
                const auto found = std::find_if(newGeometry.begin(), newGeometry.begin() + first,
                    [&](const auto* curve) {
                        return size_t(Sketcher::GeometryFacade::getId(curve)) == identity;
                    });
                if (found == newGeometry.begin() + first) {
                    throw std::runtime_error("Offset source is missing.");
                }
                linkedSources.emplace_back(int(found - newGeometry.begin()), reversed);
            }
            std::vector<int> linkedOutputs;
            for (int i = first; i < int(newGeometry.size()); ++i) {
                linkedOutputs.push_back(i);
            }
            Sketcher::LinkedOffsetParameters parameters;
            parameters.generator = "Chain";
            parameters.distance = distance();
            parameters.tolerance = control<Gui::QuantitySpinBox>("Tolerance")->rawValue();
            parameters.closed = linkedClosed;
            parameters.orientation = orientation;
            Sketcher::attachLinkedOffset(newGeometry, linkedSources, linkedOutputs, parameters);
            for (const auto& block : blocks) {
                newConstraints.push_back(block.get());
            }
            // A property notification can throw after taking ownership. Relinquish
            // only objects actually installed, including on the abort path.
            const auto releaseInstalled = [&]() {
                const auto& installedGeometry = sketch->Geometry.getValues();
                const auto& installedConstraints = sketch->Constraints.getValues();
                for (auto& curve : additions) {
                    if (std::find(installedGeometry.begin(), installedGeometry.end(), curve.get())
                        != installedGeometry.end()) {
                        curve.release();
                    }
                }
                for (auto& block : blocks) {
                    if (std::find(installedConstraints.begin(), installedConstraints.end(),
                                  block.get()) != installedConstraints.end()) {
                        block.release();
                    }
                }
            };
            checkSources();
            openCommand(QT_TRANSLATE_NOOP("Command", "Mixed-chain offset"));
            if (currentTransactionID == App::NullTransaction) {
                throw std::runtime_error("Unable to open the offset transaction.");
            }
            transactionOpen = true;
            try {
                sketch->Geometry.setValues(std::move(newGeometry));
                sketch->Constraints.setValues(std::move(newConstraints));
            }
            catch (...) {
                releaseInstalled();
                throw;
            }
            releaseInstalled();
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
            fail(QObject::tr("Unable to apply the offset chain."));
            return;
        }
        DrawSketchHandler::quit();
    }

    struct Source
    {
        boost::uuids::uuid tag;
        size_t identity;
        std::unique_ptr<Part::GeomCurve> geometry;
    };
    bool exiting = false;
    int orientation = 0;
    bool linkedClosed = false;
    std::vector<std::pair<size_t, bool>> orderedSources;
    std::string initialReason;
    std::vector<Source> sources;
    const std::vector<Gui::SelectionObject> savedSelection;
    std::unique_ptr<QObject> connections;
    Sketcher::OffsetChainResult preview;
};

}  // namespace SketcherGui
