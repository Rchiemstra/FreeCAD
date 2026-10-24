// SPDX-License-Identifier: LGPL-2.1-or-later

#include <gtest/gtest.h>
#include <sstream>
#include <stdexcept>
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
    first.version = 99;
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
    EXPECT_EQ(restored.version, 99);
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
    getObject()->addGeometry(getObject()->getGeometry(1));
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
