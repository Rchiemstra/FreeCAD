// SPDX-License-Identifier: LGPL-2.1-or-later

#include <gtest/gtest.h>
#include <sstream>
#include <iomanip>
#include <limits>
#include <stdexcept>
#include <nlohmann/json.hpp>
#include <Base/Interpreter.h>
#include <Base/Reader.h>
#include <Base/Writer.h>
#include <Mod/Sketcher/App/LinkedOffset.h>
#include "SketcherTestHelpers.h"

class LinkedOffsetTest: public SketchObjectTest
{
protected:
    void SetUp() override
    {
        SketchObjectTest::SetUp();
        Base::Interpreter().loadModule("Sketcher");
    }

    void populate()
    {
        Part::GeomLineSegment line;
        line.setPoints(Base::Vector3d(0, 0, 0), Base::Vector3d(10, 0, 0));
        getObject()->addGeometry(&line);
        getObject()->addGeometry(&line);
        getObject()->addGeometry(&line);
        auto geometry = getObject()->Geometry.getValues();
        // Work only on detached clones, as the GUI does before its transaction.
        std::vector<std::unique_ptr<Part::Geometry>> owned;
        for (auto*& g : geometry) {
            owned.emplace_back(g->clone());
            g = owned.back().get();
        }
        Sketcher::LinkedOffsetParameters parameters;
        parameters.distance = -2.5;
        parameters.tolerance = 0.005;
        Sketcher::attachLinkedOffset(geometry, {{1, true}}, {2}, parameters);
        getObject()->Geometry.setValues(geometry);
    }
};

TEST_F(LinkedOffsetTest, XmlRoundTrip)
{
    populate();
    auto before = Sketcher::listLinkedOffsets(*getObject()).at(0);
    Base::StringWriter writer;
    writer.Stream() << std::setprecision(std::numeric_limits<double>::digits10 + 1);
    getObject()->Geometry.Save(writer);
    std::stringstream data;
    data << "<?xml version='1.0'?><Root>" << writer.getString() << "</Root>";
    Base::XMLReader reader("LinkedOffset.xml", data);
    Part::PropertyGeometryList restored;
    restored.Restore(reader);
    getObject()->Geometry.Paste(restored);
    const auto after = Sketcher::listLinkedOffsets(*getObject()).at(0);
    EXPECT_EQ(after.status, Sketcher::LinkedOffsetStatus::Ok);
    EXPECT_EQ(after.relation.sources[0].uuid, before.relation.sources[0].uuid);
    EXPECT_TRUE(after.relation.sources[0].reversed);
    EXPECT_NE(after.relation.sources[0].uuid, after.relation.outputs[0].uuid);
    EXPECT_EQ(after.relation.outputs[0].signature, before.relation.outputs[0].signature);
    EXPECT_FALSE(after.relation.outputs[0].signature.empty());
    EXPECT_DOUBLE_EQ(after.relation.parameters.distance, -2.5);
    EXPECT_DOUBLE_EQ(after.relation.parameters.tolerance, 0.005);
}

TEST_F(LinkedOffsetTest, CopyAndUniqueIdentities)
{
    Sketcher::LinkedOffsetExtension first;
    Sketcher::LinkedOffsetExtension second;
    EXPECT_NE(first.uuid, second.uuid);
    first.membership = "member";
    first.record = "opaque & future";
    first.version = "99";
    auto copy = first.copy();
    auto* extension = static_cast<Sketcher::LinkedOffsetExtension*>(copy.get());
    EXPECT_EQ(extension->uuid, first.uuid);
    EXPECT_EQ(extension->membership, first.membership);
    EXPECT_EQ(extension->record, first.record);
    Base::StringWriter writer;
    first.Save(writer);
    std::stringstream data;
    data << "<?xml version='1.0'?><Root>" << writer.getString() << "</Root>";
    Base::XMLReader reader("Future.xml", data);
    reader.readElement("GeoExtension");
    Sketcher::LinkedOffsetExtension restored;
    EXPECT_NO_THROW(restored.Restore(reader));
    EXPECT_EQ(restored.version, "99");
    EXPECT_EQ(restored.record, first.record);
    Part::GeomLineSegment line;
    line.setPoints(Base::Vector3d(0, 0, 0), Base::Vector3d(10, 0, 0));
    line.setExtension(restored.copy());
    getObject()->addGeometry(&line);
    EXPECT_TRUE(Sketcher::listLinkedOffsets(*getObject()).empty());
    auto geometry = getObject()->Geometry.getValues();
    geometry.push_back(&line);
    EXPECT_THROW(
        Sketcher::attachLinkedOffset(geometry, {{0, false}}, {1}, {}),
        std::invalid_argument
    );
}

TEST_F(LinkedOffsetTest, Renumbering)
{
    populate();
    getObject()->delGeometry(0);
    const auto relation = Sketcher::listLinkedOffsets(*getObject()).at(0);
    EXPECT_EQ(relation.status, Sketcher::LinkedOffsetStatus::Ok);
    EXPECT_EQ(relation.sources, std::vector<int> {0});
    EXPECT_EQ(relation.outputs, std::vector<int> {1});
}

TEST_F(LinkedOffsetTest, DuplicateIsAmbiguous)
{
    populate();
    auto geometry = getObject()->Geometry.getValues();
    geometry.push_back(geometry[1]);
    getObject()->Geometry.setValues(geometry);
    EXPECT_EQ(
        Sketcher::listLinkedOffsets(*getObject()).at(0).status,
        Sketcher::LinkedOffsetStatus::Ambiguous
    );
}

TEST_F(LinkedOffsetTest, DeletedSource)
{
    populate();
    getObject()->delGeometry(1);
    EXPECT_EQ(
        Sketcher::listLinkedOffsets(*getObject()).at(0).status,
        Sketcher::LinkedOffsetStatus::MissingSource
    );
}

TEST_F(LinkedOffsetTest, TypeChanged)
{
    populate();
    Part::GeomCircle circle;
    const auto* old = getObject()->getGeometry(1);
    circle.setExtension(
        old->getExtension(Sketcher::LinkedOffsetExtension::getClassTypeId()).lock()->copy()
    );
    getObject()->Geometry.set1Value(1, std::unique_ptr<Part::Geometry>(circle.clone()));
    EXPECT_EQ(
        Sketcher::listLinkedOffsets(*getObject()).at(0).status,
        Sketcher::LinkedOffsetStatus::TypeChanged
    );
}

TEST_F(LinkedOffsetTest, MissingDesignatedOutput)
{
    populate();
    Part::GeomLineSegment line;
    line.setPoints(Base::Vector3d(0, 3, 0), Base::Vector3d(10, 3, 0));
    getObject()->addGeometry(&line);
    auto geometry = getObject()->Geometry.getValues();
    std::vector<std::unique_ptr<Part::Geometry>> owned;
    for (auto*& g : geometry) {
        owned.emplace_back(g->clone());
        g = owned.back().get();
    }
    Sketcher::attachLinkedOffset(geometry, {{1, false}}, {2, 3}, {});
    getObject()->Geometry.setValues(geometry);
    getObject()->delGeometry(2);
    const auto relations = Sketcher::listLinkedOffsets(*getObject());
    ASSERT_EQ(relations.size(), 1);
    EXPECT_EQ(relations[0].status, Sketcher::LinkedOffsetStatus::MissingOutput);
}

TEST_F(LinkedOffsetTest, MalformedVersionIsOpaque)
{
    for (const auto* version : {"x", "2147483648", "2"}) {
        std::stringstream data;
        data << "<Root><GeoExtension type='Sketcher::LinkedOffsetExtension' uuid='uuid' "
             << "version='" << version << "' membership='group' record='opaque'/></Root>";
        Base::XMLReader reader("Version.xml", data);
        reader.readElement("GeoExtension");
        Sketcher::LinkedOffsetExtension restored;
        ASSERT_NO_THROW(restored.Restore(reader));
        Base::StringWriter writer;
        restored.Save(writer);
        EXPECT_NE(writer.getString().find(std::string("version=\"") + version + "\""),
                  std::string::npos);
        EXPECT_EQ(restored.record, "opaque");
    }
}

TEST_F(LinkedOffsetTest, ExtraDuplicateMembersAreAmbiguous)
{
    populate();
    auto geometry = getObject()->Geometry.getValues();
    Part::GeomLineSegment extra;
    extra.setPoints(Base::Vector3d(0, 5, 0), Base::Vector3d(10, 5, 0));
    auto ext = std::make_unique<Sketcher::LinkedOffsetExtension>();
    ext->membership = Sketcher::listLinkedOffsets(*getObject())[0].relation.id;
    extra.setExtension(std::move(ext));
    geometry.push_back(&extra);
    geometry.push_back(&extra);
    getObject()->Geometry.setValues(geometry);
    EXPECT_EQ(Sketcher::listLinkedOffsets(*getObject())[0].status,
              Sketcher::LinkedOffsetStatus::Ambiguous);
}

TEST_F(LinkedOffsetTest, StrictVersionOneSchema)
{
    populate();
    const auto geometry = getObject()->Geometry.getValues();
    auto owner = std::dynamic_pointer_cast<Sketcher::LinkedOffsetExtension>(
        geometry[2]->getExtension(Sketcher::LinkedOffsetExtension::getClassTypeId()).lock());
    const auto valid = nlohmann::json::parse(owner->record);
    std::vector<nlohmann::json> invalid;
    for (const auto& item : valid["parameters"].items()) {
        auto record = valid;
        record["parameters"].erase(item.key());
        invalid.push_back(record);
        record = valid;
        record["parameters"][item.key()] = nullptr;
        invalid.push_back(record);
    }
    for (const auto* list : {"sources", "outputs"}) {
        for (const auto& item : valid[list][0].items()) {
            auto record = valid;
            record[list][0].erase(item.key());
            invalid.push_back(record);
        }
    }
    for (const auto& [key, value] : std::vector<std::pair<std::string, nlohmann::json>> {
             {"distance", 0}, {"tolerance", -1}, {"maxWork", 0}, {"maxDepth", 25},
             {"normal", {0, 0, 0}}, {"normal", {0, 0, 1, 2}}, {"normal", {1e308, 0, 0}},
             {"join", "unknown"},
             {"generator", "unknown"}, {"orientation", 2}, {"maxSegments", 1.5},
             {"closed", 0}, {"maxDegree", 15}, {"maxRefinementDepth", 100}}) {
        auto record = valid;
        record["parameters"][key] = value;
        invalid.push_back(record);
    }
    for (const auto& record : invalid) {
        owner->record = record.dump();
        const auto result = Sketcher::listLinkedOffsets(*getObject())[0];
        EXPECT_EQ(result.status, Sketcher::LinkedOffsetStatus::InvalidRecord) << record;
        EXPECT_FALSE(result.reason.empty()) << record;
    }
    owner->record = valid.dump();
    EXPECT_EQ(Sketcher::listLinkedOffsets(*getObject())[0].status,
              Sketcher::LinkedOffsetStatus::Ok);
}

TEST_F(LinkedOffsetTest, SignatureMatchesRestoredGeometry)
{
    populate();
    Part::GeomLineSegment line;
    line.setPoints(Base::Vector3d(0.12345678901234566, 0, 0), Base::Vector3d(10, 0, 0));
    auto geometry = getObject()->Geometry.getValues();
    geometry[2] = &line;
    Sketcher::attachLinkedOffset(geometry, {{1, false}}, {2}, {});
    getObject()->Geometry.setValues(geometry);
    Base::StringWriter writer;
    writer.Stream() << std::setprecision(std::numeric_limits<double>::digits10 + 1);
    getObject()->Geometry.Save(writer);
    std::stringstream data;
    data << "<Root>" << writer.getString() << "</Root>";
    Base::XMLReader reader("Signature.xml", data);
    Part::PropertyGeometryList restored;
    restored.Restore(reader);
    getObject()->Geometry.Paste(restored);
    const auto stored = Sketcher::listLinkedOffsets(*getObject())[0].relation.outputs[0].signature;
    geometry = getObject()->Geometry.getValues();
    std::unique_ptr<Part::Geometry> fresh(geometry[2]->clone());
    geometry[2] = fresh.get();
    Sketcher::attachLinkedOffset(geometry, {{1, false}}, {2}, {});
    getObject()->Geometry.setValues(geometry);
    EXPECT_EQ(stored,
              Sketcher::listLinkedOffsets(*getObject())[0].relation.outputs[0].signature);
}

TEST_F(LinkedOffsetTest, ReplacingCompleteGroupCannotCopyAncestry)
{
    populate();
    std::unique_ptr<Part::Geometry> source(getObject()->getGeometry(1)->clone());
    std::unique_ptr<Part::Geometry> output(getObject()->getGeometry(2)->clone());
    std::vector<Part::Geometry*> replacements {source.get(), output.get()};
    getObject()->replaceGeometries({1, 2}, replacements);
    source.release();
    output.release();
    const auto relations = Sketcher::listLinkedOffsets(*getObject());
    ASSERT_EQ(relations.size(), 1);
    EXPECT_NE(relations[0].status, Sketcher::LinkedOffsetStatus::Ok);
}

TEST_F(LinkedOffsetTest, OpaqueSourceVersionIsUnavailable)
{
    populate();
    auto ext = std::dynamic_pointer_cast<Sketcher::LinkedOffsetExtension>(
        getObject()->Geometry.getValues()[1]->getExtension(
            Sketcher::LinkedOffsetExtension::getClassTypeId()).lock());
    ext->version = "x";
    const auto relation = Sketcher::listLinkedOffsets(*getObject())[0];
    EXPECT_NE(relation.status, Sketcher::LinkedOffsetStatus::Ok);
    EXPECT_FALSE(relation.reason.empty());
}
