// SPDX-License-Identifier: LGPL-2.1-or-later

#include <gtest/gtest.h>

#include <algorithm>
#include <fstream>
#include <ranges>
#include <sstream>
#include <string>

#include <Base/Exception.h>
#include <Base/FileInfo.h>
#include <Base/Reader.h>

#include <App/Application.h>
#include <App/Document.h>
#include <App/DocumentObject.h>
#include <App/DocumentObjectFileIncluded.h>
#include <App/PropertyFile.h>

#include "InitApplication.h"

namespace
{
void restoreFileIncluded(App::PropertyFileIncluded& prop, const std::string& fileIncludedElement)
{
    std::string xml = "<?xml version='1.0' encoding='utf-8'?>\n";
    xml += "<Property name='File' type='App::PropertyFileIncluded'>\n";
    xml += fileIncludedElement;
    xml += "\n</Property>\n";

    std::stringstream data(xml);
    Base::XMLReader reader("Document.xml", data);
    prop.Restore(reader);
}
}  // namespace

// Regression tests for GHSA-5vqh-3v38-jw2r: a crafted .FCStd must not be able to escape the
// document transient directory through the "file" or "data" attributes of a FileIncluded element.
class PropertyFileIncludedTest: public ::testing::Test
{
protected:
    static void SetUpTestSuite()
    {
        tests::initApplication();
    }
    void SetUp() override
    {
        _doc = App::GetApplication().newDocument("PropertyFileIncludedTest");
        _object = _doc->addObject("App::VarSet", "VarSet");
        _property = static_cast<App::PropertyFileIncluded*>(
            _object->addDynamicProperty("App::PropertyFileIncluded", "File")
        );
    }
    void TearDown() override
    {
        App::GetApplication().closeDocument(_doc->getName());
    }
    App::Document* _doc {nullptr};
    App::DocumentObject* _object {nullptr};
    App::PropertyFileIncluded* _property {nullptr};
};

TEST_F(PropertyFileIncludedTest, rejectsParentTraversalInFileAttribute)
{
    EXPECT_THROW(
        restoreFileIncluded(*_property, "<FileIncluded file='../../../.bashrc'/>"),
        Base::FileException
    );
    EXPECT_TRUE(std::string(_property->getValue()).empty());
}

TEST_F(PropertyFileIncludedTest, rejectsBackslashTraversalInFileAttribute)
{
    EXPECT_THROW(
        restoreFileIncluded(*_property, "<FileIncluded file='..\\..\\.bashrc'/>"),
        Base::FileException
    );
    EXPECT_TRUE(std::string(_property->getValue()).empty());
}

TEST_F(PropertyFileIncludedTest, rejectsAbsolutePathInFileAttribute)
{
    EXPECT_THROW(
        restoreFileIncluded(*_property, "<FileIncluded file='/etc/passwd'/>"),
        Base::FileException
    );
    EXPECT_TRUE(std::string(_property->getValue()).empty());
}

TEST_F(PropertyFileIncludedTest, rejectsSubdirectoryInFileAttribute)
{
    EXPECT_THROW(
        restoreFileIncluded(*_property, "<FileIncluded file='sub/payload'/>"),
        Base::FileException
    );
    EXPECT_TRUE(std::string(_property->getValue()).empty());
}

TEST_F(PropertyFileIncludedTest, rejectsDotDotInFileAttribute)
{
    EXPECT_THROW(restoreFileIncluded(*_property, "<FileIncluded file='..'/>"), Base::FileException);
    EXPECT_TRUE(std::string(_property->getValue()).empty());
}

TEST_F(PropertyFileIncludedTest, rejectsParentTraversalInDataAttribute)
{
    EXPECT_THROW(
        restoreFileIncluded(*_property, "<FileIncluded data='../../../.bashrc'/>"),
        Base::FileException
    );
    EXPECT_TRUE(std::string(_property->getValue()).empty());
}

TEST_F(PropertyFileIncludedTest, acceptsPlainBasename)
{
    // A legitimate basename (as always produced by Save()) must still restore and resolve inside
    // the document transient directory.
    EXPECT_NO_THROW(restoreFileIncluded(*_property, "<FileIncluded file='PartShape.brp'/>"));

    // getDocTransientPath() normalizes backslashes to forward slashes, so normalize the expected
    // transient directory the same way before comparing.
    std::string transientDir = _doc->TransientDir.getValue();
    std::ranges::replace(transientDir, '\\', '/');
    EXPECT_EQ(std::string(_property->getValue()), transientDir + "/PartShape.brp");
}

// Same-document copy/paste restores the included file onto the basename the
// source already owns. Destroying the copy used to delete that shared file and
// leave the original property pointing at a missing transient path.
TEST(PropertyFileIncludedOwnership, destroyingACopyKeepsTheOriginalFile)
{
    tests::initApplication();
    App::Document* doc = App::GetApplication().newDocument("IncludedFileOwnership");
    const std::string docName = doc->getName();
    const auto closeDocument = [&docName]() {
        if (App::GetApplication().getDocument(docName.c_str())) {
            App::GetApplication().closeDocument(docName.c_str());
        }
    };
    try {
        auto* original = dynamic_cast<App::DocumentObjectFileIncluded*>(
            doc->addObject("App::DocumentObjectFileIncluded", "Original"));
        ASSERT_NE(original, nullptr);
        const std::string source = std::string(doc->TransientDir.getValue()) + "/source.txt";
        {
            std::ofstream out(source);
            out << "template-bytes";
        }
        original->File.setValue(source.c_str(), "Embedded.txt");
        const std::string live = original->File.getValue();
        ASSERT_TRUE(Base::FileInfo(live).exists());

        const auto copies = doc->copyObject({original}, false);
        ASSERT_EQ(copies.size(), 1u);
        ASSERT_NE(copies.front(), nullptr);
        doc->removeObject(copies.front()->getNameInDocument());
        doc->clearUndos();

        EXPECT_EQ(std::string(original->File.getValue()), live);
        EXPECT_TRUE(Base::FileInfo(live).exists());
        std::ifstream in(live);
        std::string text;
        std::getline(in, text);
        EXPECT_EQ(text, "template-bytes");
    }
    catch (...) {
        closeDocument();
        throw;
    }
    closeDocument();
}
