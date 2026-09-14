// SPDX-License-Identifier: LGPL-2.1-or-later

#include <gtest/gtest.h>
#include <gmock/gmock.h>

#include <cstdio>
#include <fstream>

#include "App/Application.h"
#include "App/Document.h"
#include "App/FeatureTest.h"
#include "App/MergeDocuments.h"
#include "App/StringHasher.h"
#include "App/Transactions.h"
#include "Base/Writer.h"
#include <src/App/InitApplication.h>

using ::testing::Eq;
using ::testing::Ne;

// NOLINTBEGIN(readability-magic-numbers)

class FakeWriter: public Base::Writer
{
    void writeFiles() override
    {}
    std::ostream& Stream() override
    {
        return std::cout;
    }
};

class DocumentTest: public ::testing::Test
{
protected:
    static void SetUpTestSuite()
    {
        tests::initApplication();
    }

    void SetUp() override
    {
        _docName = App::GetApplication().getUniqueDocumentName("test");
        _doc = App::GetApplication().newDocument(_docName.c_str(), "testUser");
    }

    void TearDown() override
    {
        App::GetApplication().closeDocument(_docName.c_str());
    }

    App::Document* doc()
    {
        return _doc;
    }

private:
    std::string _docName;
    App::Document* _doc {};
};


TEST_F(DocumentTest, addStringHasherIndicatesUnwrittenWhenNew)
{
    // Arrange
    App::StringHasherRef hasher(new App::StringHasher);

    // Act
    auto addResult = doc()->addStringHasher(hasher);

    // Assert
    EXPECT_TRUE(addResult.first);
    EXPECT_THAT(addResult.second, Ne(-1));
}

TEST_F(DocumentTest, addStringHasherIndicatesAlreadyWritten)
{
    // Arrange
    App::StringHasherRef hasher(new App::StringHasher);
    doc()->addStringHasher(hasher);

    // Act
    auto addResult = doc()->addStringHasher(hasher);

    // Assert
    EXPECT_FALSE(addResult.first);
}

TEST_F(DocumentTest, getStringHasherGivesExpectedHasher)
{
    // Arrange
    App::StringHasherRef hasher(new App::StringHasher);
    auto pair = doc()->addStringHasher(hasher);
    int index = pair.second;

    // Act
    auto foundHasher = doc()->getStringHasher(index);

    // Assert
    EXPECT_EQ(hasher, foundHasher);
}

TEST_F(DocumentTest, mergeIntoCombinesDifferentPropertiesOfTheSameObject)
{
    // Arrange
    auto* object = doc()->addObject<App::FeatureTest>("MergeDifferentProps");
    ASSERT_NE(object, nullptr);
    object->Integer.setValue(1);
    object->Float.setValue(1.0);

    App::Transaction parent;
    App::Transaction nested;
    // TransactionObject only snapshots a property the first time it sees it,
    // so each Transaction's snapshot is whatever the property held when that
    // Transaction first recorded it.
    parent.addObjectChange(object, &object->Integer);
    object->Integer.setValue(2);
    nested.addObjectChange(object, &object->Float);
    object->Float.setValue(2.0);

    // Act
    nested.mergeInto(parent);
    parent.apply(*doc(), false);

    // Assert: undoing the merged parent restores both properties, proving
    // the nested transaction's Float snapshot was not dropped when its
    // record for `object` was merged into the parent's.
    EXPECT_EQ(object->Integer.getValue(), 1);
    EXPECT_DOUBLE_EQ(object->Float.getValue(), 1.0);
}

TEST_F(DocumentTest, mergeIntoKeepsTheParentsSnapshotForADuplicateProperty)
{
    // Arrange
    auto* object = doc()->addObject<App::FeatureTest>("MergeDuplicateProp");
    ASSERT_NE(object, nullptr);
    object->Integer.setValue(1);

    App::Transaction parent;
    App::Transaction nested;
    parent.addObjectChange(object, &object->Integer);
    object->Integer.setValue(2);
    nested.addObjectChange(object, &object->Integer);
    object->Integer.setValue(3);

    // Act
    nested.mergeInto(parent);
    parent.apply(*doc(), false);

    // Assert: the parent's older snapshot (1) is the one undo restores, not
    // the nested transaction's newer duplicate (2).
    EXPECT_EQ(object->Integer.getValue(), 1);
}

TEST_F(DocumentTest, importObjectsRestoresSourceStringHasher)
{
    // Arrange
    auto& app = App::GetApplication();
    const std::string sourceName = app.getUniqueDocumentName("MergeSource");
    App::Document* source = app.newDocument(sourceName.c_str(), "testUser");
    App::StringHasherRef sourceHasher = source->getStringHasher();
    ASSERT_TRUE(sourceHasher);
    sourceHasher->setSaveAll(true);
    sourceHasher->getID("persisted element name");

    const std::string path = App::Application::getTempFileName();
    ASSERT_TRUE(source->saveAs(path.c_str()));
    const std::string savedPath = source->getFileName();
    app.closeDocument(sourceName.c_str());

    std::size_t restoredStringCount = 0;
    auto connection = doc()->signalFinishImportObjects.connect(
        [this, &restoredStringCount](const std::vector<App::DocumentObject*>&) {
            App::StringHasherRef importedHasher = doc()->getStringHasher(0);
            restoredStringCount = importedHasher ? importedHasher->size() : 0;
        }
    );

    // Act
    std::ifstream stream(savedPath, std::ios::in | std::ios::binary);
    ASSERT_TRUE(stream.is_open());
    App::MergeDocuments merge(doc());
    merge.importObjects(stream);

    // Assert
    EXPECT_GT(restoredStringCount, 0);
    connection.disconnect();
    std::remove(savedPath.c_str());
}

// NOLINTEND(readability-magic-numbers)
