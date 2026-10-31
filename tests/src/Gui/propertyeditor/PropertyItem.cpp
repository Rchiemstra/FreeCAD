// SPDX-License-Identifier: LGPL-2.1-or-later

#include <QAbstractTableModel>
#include <QDebug>
#include <QImage>
#include <QPainter>
#include <QLineEdit>
#include <QtTest/QTest>

#include <App/Application.h>
#include <App/Document.h>
#include <App/DocumentObject.h>
#include <Base/Quantity.h>

#include "Gui/QuantitySpinBox.h"
#include "Gui/propertyeditor/PropertyItem.h"
#include "Gui/propertyeditor/PropertyItemDelegate.h"
#include <src/App/InitApplication.h>
#include <src/LocaleTestHelpers.h>

// NOLINTBEGIN(readability-magic-numbers)

class MockPropertyItem final: public Gui::PropertyEditor::PropertyItem
{
public:
    MockPropertyItem() = default;

    void setTestValue(const QVariant& v)
    {
        testValue = v;
    }
    QVariant lastSetValue() const
    {
        return storedSetValue;
    }

protected:
    QVariant value(const App::Property* /*prop*/) const override
    {
        return testValue;
    }
    void setValue(const QVariant& v) override
    {
        storedSetValue = v;
    }

private:
    QVariant testValue;
    QVariant storedSetValue;
};

class TestPropertyUnitItem final: public Gui::PropertyEditor::PropertyUnitItem
{
public:
    TestPropertyUnitItem()
        : PropertyUnitItem()
    {}
};

// Count real ObjectIdentifier resolution, including healthy-expression painting.
class DiagnosticReadProbe: public App::DocumentObject
{
public:
    App::Property* getPropertyByName(const char* name) const override
    {
        ++reads;
        return App::DocumentObject::getPropertyByName(name);
    }
    mutable int reads = 0;
};

// Supply plain display/edit values so the paint assertion measures diagnostic
// presentation reads rather than the editor's unrelated live value conversion.
class DiagnosticPaintModel: public QAbstractTableModel
{
public:
    explicit DiagnosticPaintModel(Gui::PropertyEditor::PropertyItem& item)
        : item(item)
    {}
    int rowCount(const QModelIndex& = {}) const override { return 1; }
    int columnCount(const QModelIndex& = {}) const override { return 2; }
    QModelIndex index(int row, int column, const QModelIndex& = {}) const override
    {
        return createIndex(row, column, &item);
    }
    QVariant data(const QModelIndex& index, int role) const override
    {
        if (role == Qt::ForegroundRole || role == Qt::ToolTipRole) {
            return item.data(index.column(), role);
        }
        return role == Qt::DisplayRole ? QVariant(QStringLiteral("42")) : QVariant();
    }
private:
    Gui::PropertyEditor::PropertyItem& item;
};

class testPropertyItem: public QObject
{
    Q_OBJECT

public:
    testPropertyItem()
    {
        tests::initApplication();
        item.reset(new MockPropertyItem());
    }

private Q_SLOTS:

    void init()
    {}

    void cleanup()
    {}

    void test_camelCaseSplitsTwoWords()  // NOLINT
    {
        item->setPropertyName(QLatin1String("CamelCase"));
        QCOMPARE(item->propertyName(), QLatin1String("Camel Case"));
    }

    void test_camelCaseSplitsThreeWords()  // NOLINT
    {
        item->setPropertyName(QLatin1String("ThreeWordProperty"));
        QCOMPARE(item->propertyName(), QLatin1String("Three Word Property"));
    }

    void test_digitBeforeUppercaseNotSplit()  // NOLINT
    {
        item->setPropertyName(QLatin1String("View3D"));
        QCOMPARE(item->propertyName(), QLatin1String("View3D"));
    }

    void test_camelCaseSplitButNotDigit()  // NOLINT
    {
        item->setPropertyName(QLatin1String("MyView3D"));
        QCOMPARE(item->propertyName(), QLatin1String("My View3D"));
    }

    void test_consecutiveUppercaseNotSplit()  // NOLINT
    {
        item->setPropertyName(QLatin1String("MyABC"));
        QCOMPARE(item->propertyName(), QLatin1String("My ABC"));
    }

    void test_notCleverEnoughToSplitConsecutiveCaps()  // NOLINT
    {
        item->setPropertyName(QLatin1String("MyABCOfDoom"));
        QCOMPARE(item->propertyName(), QLatin1String("My ABCOf Doom"));
    }

    void test_underscoresArentTheSameAsSpaces()  // NOLINT
    {
        item->setPropertyName(QLatin1String("Box_Length"));
        QCOMPARE(item->propertyName(), QLatin1String("Box_Length"));
    }

    void test_diagnosticRolesDoNotResolveDocumentPaths()  // NOLINT
    {
        App::DocumentInitFlags flags;
        flags.createView = false;
        auto& app = App::GetApplication();
        auto* doc = app.newDocument("DiagnosticPaintReads", "testUser", flags);
        auto* object = new DiagnosticReadProbe;
        doc->addObject(object, "Values");
        auto* prop = object->addDynamicProperty("App::PropertyFloat", "Value");
        {
            std::unique_ptr<Gui::PropertyEditor::PropertyItem> row(
                static_cast<Gui::PropertyEditor::PropertyItem*>(
                    Gui::PropertyEditor::PropertyFloatItem::create()));
            row->setPropertyName(QStringLiteral("Value"));
            row->setPropertyData({prop});
            DiagnosticPaintModel model(*row);
            Gui::PropertyEditor::PropertyItemDelegate delegate(nullptr);
            QImage image(180, 32, QImage::Format_ARGB32);
            QPainter painter(&image);
            QStyleOptionViewItem option;
            option.rect = QRect(0, 0, 180, 32);
            object->reads = 0;
            for (int i = 0; i < 100; ++i) {
                delegate.paint(&painter, option, model.index(0, 1));
                row->data(1, Qt::ForegroundRole);
                row->data(1, Qt::ToolTipRole);
                row->data(0, Qt::ToolTipRole);
            }
            QCOMPARE(object->reads, 0);
        }
        app.closeDocument(doc->getName());
    }

    void test_quantityEditorUsesItsWidgetLocale()  // NOLINT
    {
        tests::ScopedLocaleEnvironment localeState {
            {.qtLocale = "da_DK", .formattingLocale = "de_DE", .icuLocale = "fr_FR"}
        };

        TestPropertyUnitItem unitItem;
        QWidget parent;
        auto* editor = unitItem.createEditor(&parent, [] {});
        auto* spinBox = qobject_cast<Gui::QuantitySpinBox*>(editor);
        QVERIFY(spinBox != nullptr);
        if (!spinBox) {
            return;
        }

        spinBox->setLocale(QLocale(QStringLiteral("en_US")));
        unitItem.setEditorData(spinBox, QVariant::fromValue<Base::Quantity>(Base::Quantity(10.0, "mm")));
        spinBox->show();
        spinBox->setFocus();

        auto* lineEdit = spinBox->findChild<QLineEdit*>();
        QVERIFY(lineEdit != nullptr);
        if (!lineEdit) {
            return;
        }
        lineEdit->setText(QStringLiteral("12,345.67 mm"));
        QTest::keyClick(spinBox, Qt::Key_Return);

        QCOMPARE(spinBox->rawValue(), 12345.67);
        QVERIFY(spinBox->hasValidInput());
    }

private:
    std::unique_ptr<Gui::PropertyEditor::PropertyItem> item;
};

// NOLINTEND(readability-magic-numbers)

QTEST_MAIN(testPropertyItem)

#include "PropertyItem.moc"
