// SPDX-License-Identifier: LGPL-2.1-or-later

#include <gtest/gtest.h>

#include <App/Application.h>
#include <App/CollaborativeOperationRegistry.h>
#include <App/Document.h>
#include <App/DocumentObject.h>
#include <App/FeatureTest.h>
#include <App/GenericIsolatedRecompute.h>
#include <App/GeometryWorkerOperationRegistry.h>
#include <App/Part.h>
#include <App/PropertyLinks.h>
#include <App/Range.h>
#include <Base/Exception.h>
#include <Base/Type.h>
#include <Base/Writer.h>
#include <Mod/Part/App/PropertyTopoShape.h>
#include <src/App/InitApplication.h>

#include <stop_token>
#include <stdexcept>
#include <string>
#include <string_view>

namespace
{

bool isBuiltInShapeCache(const App::Property* property)
{
    return property
        && property->getTypeId().getName()
            == std::string_view("Part::PropertyShapeCache")
        && property->testStatus(App::Property::PropDynamic)
        && property->testStatus(App::Property::PropNoPersist)
        && property->testStatus(App::Property::PropHidden)
        && property->testStatus(App::Property::PropOutput)
        && !property->testStatus(App::Property::PropTransient);
}

void expectArchiveOmitsShapeCache(const App::PropertyContainer& container)
{
    Base::StringWriter writer;
    container.Save(writer);
    if (writer.getString().find("_Part_ShapeCache") != std::string::npos) {
        throw std::runtime_error(
            "shape cache was written into the archive; the worker schema defect "
            "is not the one under test");
    }
}

App::Property* addDynamic(App::DocumentObject& object,
                          const char* type,
                          const char* name,
                          const short flags)
{
    auto* property = object.addDynamicProperty(type, name, "Part", "Shape cache", flags);
    if (!property) {
        throw std::runtime_error(std::string("could not add ") + name);
    }
    return property;
}

App::Property* addBuiltInPartShapeCache(App::DocumentObject& object)
{
    try {
        auto* property = addDynamic(
            object,
            "Part::PropertyShapeCache",
            "_Part_ShapeCache",
            App::Prop_NoPersist | App::Prop_Output | App::Prop_Hidden);
        if (property->getTypeId().getName()
            != std::string_view("Part::PropertyShapeCache")) {
            throw std::runtime_error(
                std::string("shape cache type mismatch: ")
                + std::string(property->getTypeId().getName()));
        }
        if (!property->testStatus(App::Property::PropDynamic)
            || !property->testStatus(App::Property::PropNoPersist)
            || !property->testStatus(App::Property::PropHidden)
            || !property->testStatus(App::Property::PropOutput)
            || property->testStatus(App::Property::PropTransient)) {
            throw std::runtime_error(
                "built-in shape cache status bits mismatch: dynamic="
                + std::to_string(property->testStatus(App::Property::PropDynamic))
                + " nopersist="
                + std::to_string(property->testStatus(App::Property::PropNoPersist))
                + " hidden="
                + std::to_string(property->testStatus(App::Property::PropHidden))
                + " output="
                + std::to_string(property->testStatus(App::Property::PropOutput))
                + " transient="
                + std::to_string(property->testStatus(App::Property::PropTransient))
                + " status=" + std::to_string(property->getStatus()));
        }
        expectArchiveOmitsShapeCache(object);
        return property;
    }
    catch (const Base::Exception& error) {
        throw std::runtime_error(std::string("shape-cache setup failed: ") + error.what());
    }
}

App::GeometryArchive executeIsolatedRecompute(App::Document& document,
                                              const char* featureName)
{
    try {
        App::CollaborativeOperationIntent intent {
            std::string(App::GenericIsolatedRecomputeOperationType),
            {{"feature", featureName}}};
        auto preparation =
            App::CollaborativeOperationRegistry::instance().prepare(document, intent);
        if (preparation.policy != App::PreparationPolicy::IsolatedProcess
            || !preparation.isolatedTask) {
            throw std::runtime_error(
                "isolated recompute test did not reach the worker schema path");
        }
        return App::Internal::GeometryWorkerOperationRegistry::instance().execute(
            std::string(App::GenericIsolatedRecomputeOperationType),
            preparation.isolatedTask->inputArchive,
            std::stop_token {});
    }
    catch (const Base::Exception& error) {
        throw std::runtime_error(error.what());
    }
}

void expectIsolatedRecomputeSucceeds(App::Document& document,
                                     App::FeatureTestColumn& feature)
{
    try {
        App::CollaborativeOperationIntent intent {
            std::string(App::GenericIsolatedRecomputeOperationType),
            {{"feature", feature.getNameInDocument()}}};
        auto preparation =
            App::CollaborativeOperationRegistry::instance().prepare(document, intent);
        if (preparation.policy != App::PreparationPolicy::IsolatedProcess
            || !preparation.isolatedTask) {
            throw std::runtime_error(
                "isolated recompute test did not reach the worker schema path");
        }
        const auto output =
            App::Internal::GeometryWorkerOperationRegistry::instance().execute(
                std::string(App::GenericIsolatedRecomputeOperationType),
                preparation.isolatedTask->inputArchive,
                std::stop_token {});
        auto operation = preparation.isolatedTask->decodeResult(output);
        if (!operation || !operation->recomputeOutcomeSucceeded()) {
            throw std::runtime_error(
                operation ? std::string(operation->recomputeOutcomeDiagnostic())
                          : std::string("isolated recompute returned no operation"));
        }
        operation->apply(document);
        if (feature.Value.getValue()
            != App::decodeColumn(feature.Column.getStrValue())) {
            throw std::runtime_error(
                "isolated recompute did not publish the column value");
        }
    }
    catch (const Base::Exception& error) {
        throw std::runtime_error(error.what());
    }
}

class ShapeCacheIsolatedRecomputeTest: public ::testing::Test
{
protected:
    static void SetUpTestSuite()
    {
        tests::initApplication();
        App::Internal::ensureGenericIsolatedRecomputeRegistered();
        // Part_tests_run links Part but does not run the Python module entry
        // that registers dynamic property type names.
        if (Part::PropertyShapeCache::getClassTypeId().isBad()) {
            Part::PropertyShapeCache::init();
        }
        ASSERT_FALSE(Part::PropertyShapeCache::getClassTypeId().isBad());
        ASSERT_FALSE(Base::Type::fromName("Part::PropertyShapeCache").isBad());
    }

    void SetUp() override
    {
        _documentName = App::GetApplication().getUniqueDocumentName("shapeCacheRecompute");
        _document = App::GetApplication().newDocument(
            _documentName.c_str(), "Shape cache isolated recompute");
        ASSERT_NE(_document, nullptr);
    }

    void TearDown() override
    {
        App::GetApplication().closeDocument(_documentName.c_str());
    }

    std::string _documentName;
    App::Document* _document {nullptr};
};

}  // namespace

TEST_F(ShapeCacheIsolatedRecomputeTest, coldColumnRecomputeSucceedsWithoutAShapeCache)
{
    auto* feature = _document->addObject<App::FeatureTestColumn>("ColdColumn");
    ASSERT_NE(feature, nullptr);
    feature->Column.setValue("B");
    ASSERT_EQ(feature->getDynamicPropertyByName("_Part_ShapeCache"), nullptr);

    try {
        expectIsolatedRecomputeSucceeds(*_document, *feature);
    }
    catch (const std::runtime_error& error) {
        FAIL() << error.what();
    }
    EXPECT_EQ(feature->Value.getValue(), App::decodeColumn("B"));
}

TEST_F(ShapeCacheIsolatedRecomputeTest, aggregateShapeCacheStillRecomputesDependent)
{
    auto* part = _document->addObject<App::Part>("WarmPart");
    ASSERT_NE(part, nullptr);
    // Exact built-in cache created by the aggregate shape path; avoid Part::Feature
    // children here so ShapeMaterial does not enter the isolated closure.
    ASSERT_NE(Part::PropertyShapeCache::get(part, true), nullptr);
    ASSERT_TRUE(isBuiltInShapeCache(part->getDynamicPropertyByName("_Part_ShapeCache")));
    expectArchiveOmitsShapeCache(*part);

    auto* feature = _document->addObject<App::FeatureTestColumn>("WarmDependent");
    ASSERT_NE(feature, nullptr);
    feature->Column.setValue("C");
    auto* link = dynamic_cast<App::PropertyLink*>(
        feature->addDynamicProperty("App::PropertyLink", "PartLink"));
    ASSERT_NE(link, nullptr);
    link->setValue(part);

    try {
        expectIsolatedRecomputeSucceeds(*_document, *feature);
    }
    catch (const std::runtime_error& error) {
        FAIL() << error.what();
    }
    EXPECT_EQ(feature->Value.getValue(), App::decodeColumn("C"));
    EXPECT_TRUE(isBuiltInShapeCache(part->getDynamicPropertyByName("_Part_ShapeCache")));
}

TEST_F(ShapeCacheIsolatedRecomputeTest, nestedPartAggregateCacheStillRecomputes)
{
    auto* outer = _document->addObject<App::Part>("OuterPart");
    auto* inner = _document->addObject<App::Part>("InnerPart");
    ASSERT_NE(outer, nullptr);
    ASSERT_NE(inner, nullptr);
    ASSERT_FALSE(outer->addObject(inner).empty());
    ASSERT_NE(Part::PropertyShapeCache::get(outer, true), nullptr);
    ASSERT_NE(Part::PropertyShapeCache::get(inner, true), nullptr);
    ASSERT_TRUE(isBuiltInShapeCache(outer->getDynamicPropertyByName("_Part_ShapeCache")));
    ASSERT_TRUE(isBuiltInShapeCache(inner->getDynamicPropertyByName("_Part_ShapeCache")));
    expectArchiveOmitsShapeCache(*outer);
    expectArchiveOmitsShapeCache(*inner);

    auto* feature = _document->addObject<App::FeatureTestColumn>("NestedDependent");
    ASSERT_NE(feature, nullptr);
    feature->Column.setValue("C");
    auto* link = dynamic_cast<App::PropertyLink*>(
        feature->addDynamicProperty("App::PropertyLink", "PartLink"));
    ASSERT_NE(link, nullptr);
    link->setValue(outer);

    try {
        expectIsolatedRecomputeSucceeds(*_document, *feature);
    }
    catch (const std::runtime_error& error) {
        FAIL() << error.what();
    }
    EXPECT_EQ(feature->Value.getValue(), App::decodeColumn("C"));
    EXPECT_TRUE(isBuiltInShapeCache(outer->getDynamicPropertyByName("_Part_ShapeCache")));
    EXPECT_TRUE(isBuiltInShapeCache(inner->getDynamicPropertyByName("_Part_ShapeCache")));
}

TEST_F(ShapeCacheIsolatedRecomputeTest, builtInShapeCacheOnTheTargetStillRecomputes)
{
    auto* feature = _document->addObject<App::FeatureTestColumn>("CachedTarget");
    ASSERT_NE(feature, nullptr);
    feature->Column.setValue("C");
    ASSERT_NE(addBuiltInPartShapeCache(*feature), nullptr);

    try {
        expectIsolatedRecomputeSucceeds(*_document, *feature);
    }
    catch (const std::runtime_error& error) {
        FAIL() << error.what();
    }
    EXPECT_NE(feature->getDynamicPropertyByName("_Part_ShapeCache"), nullptr);
    EXPECT_EQ(feature->Value.getValue(), App::decodeColumn("C"));
}

TEST_F(ShapeCacheIsolatedRecomputeTest, builtInShapeCacheOnAClosureDependencyStillRecomputes)
{
    auto* container = _document->addObject<App::DocumentObject>("CachedContainer");
    ASSERT_NE(container, nullptr);
    ASSERT_NE(addBuiltInPartShapeCache(*container), nullptr);

    auto* feature = _document->addObject<App::FeatureTestColumn>("CacheDependent");
    ASSERT_NE(feature, nullptr);
    feature->Column.setValue("D");
    auto* link = dynamic_cast<App::PropertyLink*>(
        feature->addDynamicProperty("App::PropertyLink", "ContainerLink"));
    ASSERT_NE(link, nullptr);
    link->setValue(container);

    try {
        expectIsolatedRecomputeSucceeds(*_document, *feature);
    }
    catch (const std::runtime_error& error) {
        FAIL() << error.what();
    }
    EXPECT_EQ(feature->Value.getValue(), App::decodeColumn("D"));
    EXPECT_NE(container->getDynamicPropertyByName("_Part_ShapeCache"), nullptr);
}

TEST_F(ShapeCacheIsolatedRecomputeTest, shapeCacheNameWithTheWrongTypeIsNotExempted)
{
    auto* feature = _document->addObject<App::FeatureTestColumn>("WrongCacheType");
    ASSERT_NE(feature, nullptr);
    feature->Column.setValue("E");
    ASSERT_NE(addDynamic(*feature,
                         "App::PropertyString",
                         "_Part_ShapeCache",
                         App::Prop_NoPersist | App::Prop_Output | App::Prop_Hidden),
              nullptr);

    try {
        static_cast<void>(executeIsolatedRecompute(*_document, "WrongCacheType"));
        FAIL() << "a non-cache property named _Part_ShapeCache was exempted";
    }
    catch (const std::runtime_error& error) {
        EXPECT_NE(std::string(error.what()).find("generic recompute changed a property set"),
                  std::string::npos)
            << error.what();
    }
}

TEST_F(ShapeCacheIsolatedRecomputeTest, persistentShapeCacheStillRoundTrips)
{
    auto* feature = _document->addObject<App::FeatureTestColumn>("PersistentCache");
    ASSERT_NE(feature, nullptr);
    feature->Column.setValue("F");
    auto* persistent = addDynamic(
        *feature, "Part::PropertyShapeCache", "_Part_ShapeCache", App::Prop_Output);
    ASSERT_TRUE(persistent->testStatus(App::Property::PropDynamic));
    ASSERT_FALSE(persistent->testStatus(App::Property::PropNoPersist));

    try {
        expectIsolatedRecomputeSucceeds(*_document, *feature);
    }
    catch (const std::runtime_error& error) {
        FAIL() << error.what();
    }
    EXPECT_NE(feature->getDynamicPropertyByName("_Part_ShapeCache"), nullptr);
}

TEST_F(ShapeCacheIsolatedRecomputeTest, shapeCacheWithTheWrongStatusIsNotExempted)
{
    auto* omitted = _document->addObject<App::FeatureTestColumn>("WrongCacheFlags");
    ASSERT_NE(omitted, nullptr);
    omitted->Column.setValue("G");
    ASSERT_NE(addDynamic(*omitted,
                         "Part::PropertyShapeCache",
                         "_Part_ShapeCache",
                         App::Prop_NoPersist),
              nullptr);

    try {
        static_cast<void>(executeIsolatedRecompute(*_document, "WrongCacheFlags"));
        FAIL() << "a nonpersistent shape cache with the wrong status was exempted";
    }
    catch (const std::runtime_error& error) {
        EXPECT_NE(std::string(error.what()).find("generic recompute changed a property set"),
                  std::string::npos)
            << error.what();
    }
}
