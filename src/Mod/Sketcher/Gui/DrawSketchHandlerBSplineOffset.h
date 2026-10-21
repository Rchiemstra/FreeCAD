// SPDX-License-Identifier: LGPL-2.1-or-later

#pragma once

#include <algorithm>
#include <memory>
#include <stdexcept>
#include <utility>

#include <QCheckBox>
#include <QDoubleSpinBox>
#include <QFormLayout>
#include <QLabel>
#include <QPushButton>
#include <QVBoxLayout>

#include <Standard_Failure.hxx>

#include <App/Document.h>
#include <Base/Exception.h>
#include <Gui/BitmapFactory.h>
#include <Mod/Sketcher/App/BSplineOffset.h>
#include <Mod/Sketcher/App/SketchObject.h>

#include "DrawSketchHandler.h"

namespace SketcherGui
{

// This snapshot tool deliberately has no mouse-move computation or document preview objects.
class DrawSketchHandlerBSplineOffset: public DrawSketchHandler
{
public:
    explicit DrawSketchHandlerBSplineOffset(int sourceId)
        : sourceId(sourceId)
    {}

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
    std::string getToolName() const override
    {
        return "DSH_BSplineOffset";
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
        auto distance = new QDoubleSpinBox(panel.get());
        distance->setObjectName(QStringLiteral("BSplineOffsetDistance"));
        distance->setDecimals(7);
        distance->setRange(-1e9, 1e9);
        distance->setSuffix(QObject::tr(" mm"));
        distance->setValue(1.0);
        form->addRow(QObject::tr("Signed distance"), distance);
        auto tolerance = new QDoubleSpinBox(panel.get());
        tolerance->setObjectName(QStringLiteral("BSplineOffsetTolerance"));
        tolerance->setDecimals(7);
        tolerance->setRange(0.0, 1e9);
        tolerance->setSuffix(QObject::tr(" mm"));
        tolerance->setValue(0.01);
        form->addRow(QObject::tr("Tolerance"), tolerance);
        layout->addLayout(form);
        auto reverse = new QCheckBox(QObject::tr("Reverse side"), panel.get());
        reverse->setObjectName(QStringLiteral("BSplineOffsetReverse"));
        layout->addWidget(reverse);
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

    template<class T>
    T* control(const char* name) const
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
            sourceCopy.reset();
            clearEdit();
            setStatus(QObject::tr("Inputs changed. Press Update preview."));
        };
        for (const char* name : {"BSplineOffsetDistance", "BSplineOffsetTolerance"}) {
            QObject::connect(
                control<QDoubleSpinBox>(name),
                qOverload<double>(&QDoubleSpinBox::valueChanged),
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

    const Part::GeomBSplineCurve* source()
    {
        auto sketch = getSketchObject();
        if (sourceId < 0 || sourceId >= sketch->Geometry.getSize()) {
            return nullptr;
        }
        return dynamic_cast<const Part::GeomBSplineCurve*>(sketch->getGeometry(sourceId));
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

    bool updatePreview()
    {
        try {
            const auto* current = source();
            if (!current) {
                throw std::runtime_error(
                    QObject::tr("The selected B-spline is no longer available.").toStdString()
                );
            }
            Sketcher::BSplineOffsetOptions options;
            options.distance = control<QDoubleSpinBox>("BSplineOffsetDistance")->value();
            options.tolerance = control<QDoubleSpinBox>("BSplineOffsetTolerance")->value();
            if (control<QCheckBox>("BSplineOffsetReverse")->isChecked()) {
                options.distance = -options.distance;
            }
            if (!preview.curve || !sourceCopy || !sameSource(*current, *sourceCopy)
                || options.distance != previewDistance || options.tolerance != previewTolerance) {
                clearEdit();
                preview = {};
                sourceCopy.reset(static_cast<Part::GeomBSplineCurve*>(current->copy()));
                toolwidget->setProperty(
                    "computationCount",
                    toolwidget->property("computationCount").toInt() + 1
                );
                preview = Sketcher::makeBSplineOffset(*sourceCopy, options);
                previewDistance = options.distance;
                previewTolerance = options.tolerance;
            }
            if (!preview.curve) {
                setStatus(QObject::tr(preview.message.c_str()));
                return false;
            }
            drawEdit(std::vector<Part::Geometry*> {preview.curve.get()});
            setStatus(QObject::tr("Achieved maximum error: %1 mm (tolerance: %2 mm)")
                          .arg(preview.achievedMaxError, 0, 'g', 7)
                          .arg(options.tolerance, 0, 'g', 7));
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
        sourceCopy.reset();
        clearEdit();
        setStatus(message);
    }

    void apply()
    {
        // Fitting and verification finish before opening the only document transaction.
        if (!updatePreview()) {
            return;
        }
        bool transactionOpen = false;
        try {
            openCommand(QT_TRANSLATE_NOOP("Command", "Approximate B-spline offset"));
            if (currentTransactionID == App::NullTransaction) {
                throw std::runtime_error(
                    QObject::tr("Unable to open the offset transaction.").toStdString());
            }
            transactionOpen = true;
            auto sketch = getSketchObject();
            const int id = sketch->addGeometry(preview.curve.get(), false);
            if (id < 0) {
                throw std::runtime_error(QObject::tr("Unable to add the offset curve.").toStdString());
            }
            auto block = std::make_unique<Sketcher::Constraint>();
            block->Type = Sketcher::Block;
            block->First = id;
            if (sketch->addConstraint(std::move(block)) < 0
                || sketch->solve() != Sketcher::SketchSolveStatus::Success
                || sketch->solve() != Sketcher::SketchSolveStatus::Success) {
                throw std::runtime_error(
                    QObject::tr("Unable to block and solve the offset curve.").toStdString()
                );
            }
            if (!source() || !sameSource(*source(), *sourceCopy)) {
                throw std::runtime_error(
                    QObject::tr("The source changed while applying the offset.").toStdString()
                );
            }
            bool recomputeError = false;
            sketch->getDocument()->recompute({sketch}, false, &recomputeError);
            if (recomputeError || sketch->isError() || !source()
                || !sameSource(*source(), *sourceCopy)
                || !sameSource(*sketch->getGeometry<Part::GeomBSplineCurve>(id), *preview.curve)) {
                throw std::runtime_error(
                    QObject::tr("Unable to recompute the offset without changing the source.")
                        .toStdString()
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
        quit();
    }

    const int sourceId;
    std::unique_ptr<QObject> connections;
    std::unique_ptr<Part::GeomBSplineCurve> sourceCopy;
    Sketcher::BSplineOffsetResult preview;
    double previewDistance = 0.0;
    double previewTolerance = 0.0;
};

}  // namespace SketcherGui
