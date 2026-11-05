// SPDX-License-Identifier: LGPL-2.1-or-later

#include <gtest/gtest.h>

#include <App/Application.h>
#include <App/Document.h>
#include <App/DocumentObject.h>
#include <App/DocumentRevisionIndex.h>
#include <App/FeatureTest.h>
#include <App/PropertyTouchPublication.h>
#include <src/App/InitApplication.h>

using namespace App;

namespace
{

DocumentRevision propertyRevision(const Document& document,
                                  const DocumentObject& object,
                                  const char* propertyName)
{
    return document.collaborationRevisions().current(
        DocumentRevisionKey::objectProperty(object.getNameInDocument(), propertyName));
}

}  // namespace

class PropertyTouchPublicationTest: public ::testing::Test
{
protected:
    static void SetUpTestSuite()
    {
        tests::initApplication();
    }

    void SetUp() override
    {
        _docName = GetApplication().getUniqueDocumentName("test");
        _doc = GetApplication().newDocument(_docName.c_str(), "testUser");
        _object = _doc->addObject<FeatureTest>("Touched");
        ASSERT_NE(_object, nullptr);
        ASSERT_NE(_object->getNameInDocument(), nullptr);
    }

    void TearDown() override
    {
        GetApplication().closeDocument(_docName.c_str());
    }

    std::string _docName;
    Document* _doc {};
    FeatureTest* _object {};
};

TEST_F(PropertyTouchPublicationTest, suppressorSkipsOnlyThatPropertyAndRestoresNesting)
{
    const auto labelBefore = propertyRevision(*_doc, *_object, "Label");
    const auto integerBefore = propertyRevision(*_doc, *_object, "Integer");
    const auto stringBefore = propertyRevision(*_doc, *_object, "String");

    {
        const PropertyTouchPublicationSuppressor suppressLabel(_object->Label);
        _object->Label.touch();
        EXPECT_EQ(propertyRevision(*_doc, *_object, "Label"), labelBefore);

        _object->Integer.setValue(_object->Integer.getValue() + 1);
        EXPECT_EQ(propertyRevision(*_doc, *_object, "Integer"), integerBefore + 1);
    }

    const auto labelAfterScope = propertyRevision(*_doc, *_object, "Label");
    _object->Label.touch();
    EXPECT_EQ(propertyRevision(*_doc, *_object, "Label"), labelAfterScope + 1);

    const auto labelAtNest = propertyRevision(*_doc, *_object, "Label");
    const auto stringAtNest = propertyRevision(*_doc, *_object, "String");
    EXPECT_EQ(stringAtNest, stringBefore);
    {
        const PropertyTouchPublicationSuppressor suppressLabel(_object->Label);
        {
            const PropertyTouchPublicationSuppressor suppressString(_object->String);
            _object->Label.touch();
            EXPECT_EQ(propertyRevision(*_doc, *_object, "Label"), labelAtNest + 1);
            _object->String.touch();
            EXPECT_EQ(propertyRevision(*_doc, *_object, "String"), stringAtNest);
        }

        const auto labelRestored = propertyRevision(*_doc, *_object, "Label");
        _object->Label.touch();
        EXPECT_EQ(propertyRevision(*_doc, *_object, "Label"), labelRestored);
        _object->String.touch();
        EXPECT_EQ(propertyRevision(*_doc, *_object, "String"), stringAtNest + 1);
    }

    const auto labelAfterNest = propertyRevision(*_doc, *_object, "Label");
    _object->Label.touch();
    EXPECT_EQ(propertyRevision(*_doc, *_object, "Label"), labelAfterNest + 1);
}

TEST_F(PropertyTouchPublicationTest, documentObjectTouchTruePublishesObjectModel)
{
    const auto modelKey = DocumentRevisionKey::objectModel(_object->getNameInDocument());
    const auto unknownKey = DocumentRevisionKey::unknownModelMutation();
    const auto modelBefore = _doc->collaborationRevisions().current(modelKey);
    const auto unknownBefore = _doc->collaborationRevisions().current(unknownKey);

    _object->touch(true);

    EXPECT_EQ(_doc->collaborationRevisions().current(modelKey), modelBefore + 1);
    EXPECT_EQ(_doc->collaborationRevisions().current(unknownKey), unknownBefore + 1);
}

TEST_F(PropertyTouchPublicationTest, observerWriteDuringSuppressedLabelTouchPublishes)
{
    _object->Integer.setValue(_object->Integer.getValue() + 1);
    const auto labelBefore = propertyRevision(*_doc, *_object, "Label");
    const auto integerBefore = propertyRevision(*_doc, *_object, "Integer");
    const long written = _object->Integer.getValue() + 1;

    const ::fastsignals::scoped_connection observer = _doc->signalChangedObject.connect(
        [&](const DocumentObject& object, const Property& property) {
            if (&object == _object && &property == &_object->Label) {
                _object->Integer.setValue(written);
            }
        });

    {
        const PropertyTouchPublicationSuppressor suppressLabel(_object->Label);
        _object->Label.touch();
    }

    EXPECT_EQ(_object->Integer.getValue(), written);
    EXPECT_EQ(propertyRevision(*_doc, *_object, "Label"), labelBefore);
    EXPECT_EQ(propertyRevision(*_doc, *_object, "Integer"), integerBefore + 1);
}
