// SPDX-License-Identifier: LGPL-2.1-or-later

#include <gtest/gtest.h>

#include <BRepBndLib.hxx>
#include <BRepExtrema_DistShapeShape.hxx>
#include <BRepPrimAPI_MakeBox.hxx>
#include <BRep_Builder.hxx>
#include <Bnd_Box.hxx>
#include <Standard_Failure.hxx>
#include <TopExp.hxx>
#include <TopTools_IndexedMapOfShape.hxx>
#include <gp_Pnt.hxx>
#include <atomic>
#include <cmath>
#include <limits>
#include <cstdio>
#include <sstream>
#include <algorithm>
#include <memory>
#include <tuple>
#include <utility>

#include <App/Document.h>
#include <App/DocumentObjectGroup.h>
#include <App/Link.h>
#include <App/Part.h>
#include <App/PropertyLinks.h>
#include <App/Range.h>
#include <Base/Writer.h>
#include <Base/Interpreter.h>
#include <Base/Placement.h>
#include <Mod/Assembly/App/AssemblyLink.h>
#include <Mod/Assembly/App/AssemblyObject.h>
#include <Mod/Assembly/App/Groups.h>
#include <Mod/Assembly/App/InterferenceScan.h>
#include <Mod/Assembly/App/InterferenceScanSession.h>
#include <Mod/Assembly/App/ReviewNote.h>
#include <Mod/Part/App/FeaturePartBox.h>
#include <Mod/PartDesign/App/Body.h>
#include <Mod/Spreadsheet/App/Sheet.h>
#include <src/App/InitApplication.h>

namespace
{

Assembly::InterferenceLeaf makeLeaf(
    const char* path,
    const char* sourceId,
    const gp_Pnt& corner,
    double dx = 10,
    double dy = 10,
    double dz = 10
)
{
    Assembly::InterferenceLeaf leaf;
    leaf.displayPath = path;
    leaf.occurrenceSubName = path;
    leaf.sourceId = sourceId;
    leaf.worldShape = BRepPrimAPI_MakeBox(corner, dx, dy, dz).Shape();
    leaf.worldBoundBox = Base::BoundBox3d(
        corner.X(),
        corner.Y(),
        corner.Z(),
        corner.X() + dx,
        corner.Y() + dy,
        corner.Z() + dz
    );
    leaf.shapeValid = true;
    leaf.visible = true;
    return leaf;
}

Part::Box* makeBox(
    App::Document* doc,
    const char* name,
    const Base::Vector3d& pos,
    double dx = 10,
    double dy = 10,
    double dz = 10
)
{
    auto* box = doc->addObject<Part::Box>(name);
    box->Length.setValue(dx);
    box->Width.setValue(dy);
    box->Height.setValue(dz);
    box->Placement.setValue(Base::Placement(pos, Base::Rotation()));
    box->recomputeFeature();
    return box;
}

std::pair<std::string, std::string> canonicalIds(const std::string& a, const std::string& b)
{
    return (a <= b) ? std::make_pair(a, b) : std::make_pair(b, a);
}

std::string objectSourceId(const App::DocumentObject* obj)
{
    if (!obj || !obj->isAttachedToDocument()) {
        return {};
    }
    return std::string(obj->getDocument()->getName()) + "#" + obj->getNameInDocument();
}

std::string exclusionPropertyXml(Assembly::AssemblyObject* assembly)
{
    auto* prop = assembly->getPropertyByName("InterferenceExcludedSources");
    if (!prop) {
        return {};
    }
    Base::StringWriter writer;
    prop->Save(writer);
    return writer.getString();
}

class ScopedExternalDocument
{
public:
    ScopedExternalDocument(const char* userName, const char* label)
    {
        _name = App::GetApplication().getUniqueDocumentName(userName);
        _doc = App::GetApplication().newDocument(_name.c_str(), label);
    }

    ~ScopedExternalDocument()
    {
        if (_doc && App::GetApplication().getDocument(_name.c_str())) {
            App::GetApplication().closeDocument(_name.c_str());
        }
    }

    App::Document* doc() const
    {
        return _doc;
    }

    const std::string& name() const
    {
        return _name;
    }

private:
    std::string _name;
    App::Document* _doc {};
};

class ScopedSavedOwnerPath
{
public:
    void assign(App::Document* doc, std::string path)
    {
        _path = std::move(path);
        doc->FileName.setValue(_path.c_str());
    }

    ~ScopedSavedOwnerPath()
    {
        if (!_path.empty()) {
            std::remove(_path.c_str());
        }
    }

private:
    std::string _path;
};

}  // namespace

class InterferenceScanTest: public ::testing::Test
{
protected:
    static void SetUpTestSuite()
    {
        tests::initApplication();
        Base::Interpreter().runString("import Part");
        Base::Interpreter().runString("import Material");
        Base::Interpreter().runString("import Spreadsheet");
        Base::Interpreter().runString("import _PartDesign");
        Base::Interpreter().runString("import AssemblyApp");
    }

    void SetUp() override
    {
        _docName = App::GetApplication().getUniqueDocumentName("asmInterference");
        _doc = App::GetApplication().newDocument(_docName.c_str(), "asmInterferenceUser");
        _assembly = _doc->addObject<Assembly::AssemblyObject>("Assembly");
    }

    void TearDown() override
    {
        App::GetApplication().closeDocument(_docName.c_str());
    }

    App::Document* _doc = nullptr;
    Assembly::AssemblyObject* _assembly = nullptr;
    std::string _docName;
};

TEST_F(InterferenceScanTest, runScanDetectsPenetrationOnSnapshotLeaves)
{
    std::vector<Assembly::InterferenceLeaf> leaves;
    leaves.push_back(makeLeaf("A", "srcA", gp_Pnt(0, 0, 0)));
    leaves.push_back(makeLeaf("B", "srcB", gp_Pnt(5, 0, 0)));

    Assembly::InterferenceScanOptions options;
    auto result = Assembly::runInterferenceScan(leaves, options);
    ASSERT_TRUE(result.complete);
    EXPECT_GE(result.counts.penetrations, 1);
}

TEST_F(InterferenceScanTest, runScanClearForSeparatedLeaves)
{
    std::vector<Assembly::InterferenceLeaf> leaves;
    leaves.push_back(makeLeaf("A", "srcA", gp_Pnt(0, 0, 0)));
    leaves.push_back(makeLeaf("B", "srcB", gp_Pnt(50, 0, 0)));

    Assembly::InterferenceScanOptions options;
    auto result = Assembly::runInterferenceScan(leaves, options);
    ASSERT_TRUE(result.complete);
    EXPECT_EQ(result.counts.penetrations, 0);
    EXPECT_EQ(result.counts.contacts, 0);
    EXPECT_EQ(result.counts.clearanceViolations, 0);
}

TEST_F(InterferenceScanTest, sourceExclusionSuppressesViolationBySourceId)
{
    auto* a = _doc->addObject<App::DocumentObject>("App::Feature", "SourceA");
    auto* b = _doc->addObject<App::DocumentObject>("App::Feature", "SourceB");
    _assembly->addObject(a);
    _assembly->addObject(b);
    _assembly->addInterferenceExclusion(a, b);

    const std::string idA = std::string(_doc->getName()) + "#SourceA";
    const std::string idB = std::string(_doc->getName()) + "#SourceB";

    std::vector<Assembly::InterferenceLeaf> leaves;
    leaves.push_back(makeLeaf("A", idA.c_str(), gp_Pnt(0, 0, 0)));
    leaves.push_back(makeLeaf("B", idB.c_str(), gp_Pnt(5, 0, 0)));

    Assembly::InterferenceScanOptions options;
    std::vector<std::pair<std::string, std::string>> excluded {canonicalIds(idA, idB)};
    auto result = Assembly::runInterferenceScan(leaves, options, excluded);
    ASSERT_TRUE(result.complete);
    EXPECT_EQ(result.counts.penetrations, 0);
    EXPECT_GE(result.counts.excludedViolations, 1);
    ASSERT_FALSE(result.pairs.empty());
    // Worker must classify via sourceId only; DocumentObject* are not required on leaves.
    EXPECT_TRUE(result.pairs.front().excluded);
}

TEST_F(InterferenceScanTest, clearanceAndExclusionPropertiesPersistBasics)
{
    auto* a = _doc->addObject<App::DocumentObject>("App::Feature", "SourceA");
    auto* b = _doc->addObject<App::DocumentObject>("App::Feature", "SourceB");

    _assembly->setInterferenceClearance(1.5);
    EXPECT_DOUBLE_EQ(_assembly->getInterferenceClearance(), 1.5);
    _assembly->addInterferenceExclusion(a, b);
    EXPECT_TRUE(_assembly->hasInterferenceExclusion(a, b));
    EXPECT_TRUE(_assembly->hasInterferenceExclusion(b, a));

    _assembly->removeInterferenceExclusion(a, b);
    EXPECT_FALSE(_assembly->hasInterferenceExclusion(a, b));
}

TEST_F(InterferenceScanTest, exclusionReasonsStayAlignedAndUseStableNoteIdentity)
{
    auto* a = _doc->addObject<App::DocumentObject>("App::Feature", "ReasonSourceA");
    auto* b = _doc->addObject<App::DocumentObject>("App::Feature", "ReasonSourceB");
    auto* c = _doc->addObject<App::DocumentObject>("App::Feature", "ReasonSourceC");
    auto* d = _doc->addObject<App::DocumentObject>("App::Feature", "ReasonSourceD");

    auto* group = _doc->addObject<Assembly::ReviewNoteGroup>("ReasonNotes");
    ASSERT_NE(group, nullptr);
    _assembly->addObject(group);
    auto* note = _doc->addObject<Assembly::ReviewNote>("InterferenceReason");
    ASSERT_NE(note, nullptr);
    group->addObject(note);
    auto* replacement = _doc->addObject<Assembly::ReviewNote>("ReplacementReason");
    ASSERT_NE(replacement, nullptr);
    group->addObject(replacement);

    Assembly::addInterferenceExclusionWithReason(_assembly, a, b, note);
    Assembly::addInterferenceExclusion(_assembly, c, d);

    auto* reasons = dynamic_cast<App::PropertyStringList*>(
        _assembly->getPropertyByName("InterferenceExclusionReasons")
    );
    ASSERT_NE(reasons, nullptr);
    ASSERT_EQ(reasons->getValues().size(), 2u);
    const std::string noteIdentity = objectSourceId(note);
    EXPECT_EQ(reasons->getValues()[0], noteIdentity);
    EXPECT_TRUE(reasons->getValues()[1].empty());

    auto rules = Assembly::getInterferenceExclusionRules(_assembly);
    ASSERT_EQ(rules.size(), 2u);
    EXPECT_EQ(rules[0].reason, note);
    EXPECT_EQ(rules[0].reasonIdentity, noteIdentity);
    EXPECT_EQ(rules[1].reason, nullptr);

    // Idempotently enriching the existing pair replaces only its reason.
    Assembly::addInterferenceExclusionWithReason(_assembly, b, a, replacement);
    rules = Assembly::getInterferenceExclusionRules(_assembly);
    ASSERT_EQ(rules.size(), 2u);
    EXPECT_EQ(rules[0].reason, replacement);
    EXPECT_EQ(rules[0].reasonIdentity, objectSourceId(replacement));

    // Deleting the note cannot create a stale link: only its identity string remains.
    const std::string replacementIdentity = objectSourceId(replacement);
    _doc->removeObject(replacement->getNameInDocument());
    rules = Assembly::getInterferenceExclusionRules(_assembly);
    ASSERT_EQ(rules.size(), 2u);
    EXPECT_EQ(rules[0].reason, nullptr);
    EXPECT_EQ(rules[0].reasonIdentity, replacementIdentity);
    EXPECT_EQ(
        dynamic_cast<App::PropertyLinkBase*>(
            _assembly->getPropertyByName("InterferenceExclusionReasons")
        ),
        nullptr
    );

    // Removing rule zero removes reason zero; the unreasoned second rule shifts with it.
    Assembly::removeInterferenceExclusionAt(_assembly, 0);
    rules = Assembly::getInterferenceExclusionRules(_assembly);
    ASSERT_EQ(rules.size(), 1u);
    EXPECT_TRUE(rules[0].reasonIdentity.empty());
    ASSERT_EQ(reasons->getValues().size(), 1u);
    EXPECT_TRUE(reasons->getValues()[0].empty());
}

TEST_F(InterferenceScanTest, exclusionPairInsertionAddsEndpointsPreservesOrderAndUndoRedo)
{
    auto* seedA = _doc->addObject<App::DocumentObject>("App::Feature", "SeedA");
    auto* seedB = _doc->addObject<App::DocumentObject>("App::Feature", "SeedZ");
    auto* addC = _doc->addObject<App::DocumentObject>("App::Feature", "AddC");
    auto* addD = _doc->addObject<App::DocumentObject>("App::Feature", "AddD");

    _assembly->addInterferenceExclusion(seedA, seedB);
    const std::string xmlAfterSeed = exclusionPropertyXml(_assembly);

    auto* prop = dynamic_cast<App::PropertyXLinkSubList*>(
        _assembly->getPropertyByName("InterferenceExcludedSources")
    );
    ASSERT_NE(prop, nullptr);
    ASSERT_EQ(prop->getSize(), 2);

    _doc->openTransaction("Add exclusion pair");
    Assembly::addInterferenceExclusion(_assembly, addC, addD);
    _doc->commitTransaction();

    ASSERT_EQ(prop->getSize(), 4);
    std::vector<App::DocumentObject*> values = prop->getValues();
    ASSERT_EQ(values.size(), 4u);
    EXPECT_EQ(values[0], seedA);
    EXPECT_EQ(values[1], seedB);
    EXPECT_EQ(values[2], addC);
    EXPECT_EQ(values[3], addD);

    _doc->undo();
    EXPECT_EQ(exclusionPropertyXml(_assembly), xmlAfterSeed);
    ASSERT_EQ(prop->getSize(), 2);

    _doc->redo();
    ASSERT_EQ(prop->getSize(), 4);
    EXPECT_TRUE(_assembly->hasInterferenceExclusion(addC, addD));

    _assembly->removeInterferenceExclusion(addC, addD);
    Assembly::addInterferenceExclusion(_assembly, addC, addD);
    EXPECT_TRUE(_assembly->hasInterferenceExclusion(addC, addD));
}

TEST_F(InterferenceScanTest, failedExclusionWithUnsavedExternalSecondRestoresProperty)
{
    auto* seedA = _doc->addObject<App::DocumentObject>("App::Feature", "KeepSeedA");
    auto* seedB = _doc->addObject<App::DocumentObject>("App::Feature", "KeepSeedZ");
    _assembly->addInterferenceExclusion(seedA, seedB);
    const std::string xmlBefore = exclusionPropertyXml(_assembly);

    ScopedExternalDocument other("zzz_asmExclUnsavedOther", "zzz_asmExclUnsavedOtherUser");
    ASSERT_NE(other.doc(), nullptr);
    auto* local = _doc->addObject<App::DocumentObject>("App::Feature", "LocalPairA");
    auto* remote = other.doc()->addObject<App::DocumentObject>("App::Feature", "RemotePairZ");
    ASSERT_LT(objectSourceId(local), objectSourceId(remote));

    ScopedSavedOwnerPath ownerPath;
    ownerPath.assign(_doc, std::string("/tmp/") + _docName + "_exclAtomic.FCStd");
    ASSERT_TRUE(_doc->save());

    const auto undoBefore = _doc->getAvailableUndoNames();
    bool sawInsertionFailure = false;
    try {
        Assembly::addInterferenceExclusion(_assembly, local, remote);
        FAIL() << "expected exception for unsaved external document";
    }
    catch (const Base::Exception& exc) {
        sawInsertionFailure = true;
        EXPECT_NE(std::string(exc.what()).find("Linked document not saved"), std::string::npos)
            << exc.what();
    }
    EXPECT_TRUE(sawInsertionFailure);

    EXPECT_EQ(exclusionPropertyXml(_assembly), xmlBefore);
    auto* prop = dynamic_cast<App::PropertyXLinkSubList*>(
        _assembly->getPropertyByName("InterferenceExcludedSources")
    );
    ASSERT_NE(prop, nullptr);
    EXPECT_EQ(prop->getSize(), 2);
    EXPECT_EQ(prop->getSize() % 2, 0);

    const auto rules = _assembly->getInterferenceExclusionRules();
    ASSERT_EQ(rules.size(), 1u);
    EXPECT_TRUE(rules[0].valid);
    EXPECT_EQ(rules[0].first, seedA);
    EXPECT_EQ(rules[0].second, seedB);

    EXPECT_EQ(_doc->getAvailableUndoNames().size(), undoBefore.size());
    EXPECT_FALSE(_doc->hasPendingTransaction());

    auto* okA = _doc->addObject<App::DocumentObject>("App::Feature", "LaterOkA");
    auto* okB = _doc->addObject<App::DocumentObject>("App::Feature", "LaterOkB");
    _assembly->addInterferenceExclusion(okA, okB);
    EXPECT_TRUE(_assembly->hasInterferenceExclusion(okA, okB));
    _assembly->removeInterferenceExclusion(okA, okB);
}

TEST_F(InterferenceScanTest, failedExclusionWithUnsavedExternalFirstRestoresProperty)
{
    auto* seedA = _doc->addObject<App::DocumentObject>("App::Feature", "KeepSeedOnlyA");
    auto* seedB = _doc->addObject<App::DocumentObject>("App::Feature", "KeepSeedOnlyZ");
    _assembly->addInterferenceExclusion(seedA, seedB);
    const std::string xmlBefore = exclusionPropertyXml(_assembly);

    ScopedExternalDocument other("aaa_asmExclUnsavedOther", "aaa_asmExclUnsavedOtherUser");
    auto* local = _doc->addObject<App::DocumentObject>("App::Feature", "Z_LocalLate");
    auto* remote = other.doc()->addObject<App::DocumentObject>("App::Feature", "A_RemoteEarly");
    ASSERT_GT(objectSourceId(local), objectSourceId(remote));

    ScopedSavedOwnerPath ownerPath;
    ownerPath.assign(_doc, std::string("/tmp/") + _docName + "_exclAtomicExtFirst.FCStd");
    ASSERT_TRUE(_doc->save());

    EXPECT_THROW(Assembly::addInterferenceExclusion(_assembly, local, remote), Base::Exception);
    EXPECT_EQ(exclusionPropertyXml(_assembly), xmlBefore);
}

TEST_F(InterferenceScanTest, broadPhaseFindsSeparatedCandidatesConservatively)
{
    std::vector<Assembly::InterferenceLeaf> leaves;
    for (int i = 0; i < 8; ++i) {
        leaves.push_back(makeLeaf(
            ("L" + std::to_string(i)).c_str(),
            ("S" + std::to_string(i)).c_str(),
            gp_Pnt(i * 30.0, 0, 0)
        ));
    }

    auto pairs = Assembly::broadPhaseCandidatePairs(leaves, 0.0, 1e-7);
    EXPECT_EQ(pairs.size(), 0u);

    pairs = Assembly::broadPhaseCandidatePairs(leaves, 25.0, 1e-7);
    EXPECT_GT(pairs.size(), 0u);
}

TEST_F(InterferenceScanTest, sameSourceExclusionSuppressesInstancePairsBySourceId)
{
    auto* source = _doc->addObject<App::DocumentObject>("App::Feature", "SharedSource");
    _assembly->addInterferenceExclusion(source, source);
    const std::string shared = std::string(_doc->getName()) + "#SharedSource";

    std::vector<Assembly::InterferenceLeaf> leaves;
    leaves.push_back(makeLeaf("L1", shared.c_str(), gp_Pnt(0, 0, 0), 5, 5, 5));
    leaves.push_back(makeLeaf("L2", shared.c_str(), gp_Pnt(2, 0, 0), 5, 5, 5));

    Assembly::InterferenceScanOptions options;
    std::vector<std::pair<std::string, std::string>> excluded {canonicalIds(shared, shared)};
    auto result = Assembly::runInterferenceScan(leaves, options, excluded);
    EXPECT_GE(result.counts.excludedViolations, 1);
}

TEST_F(InterferenceScanTest, nestedPartAppliesAncestorPlacementToWorldBounds)
{
    auto* nested = _doc->addObject<App::Part>("NestedPart");
    nested->Placement.setValue(Base::Placement(Base::Vector3d(100, 20, 30), Base::Rotation()));
    auto* box = makeBox(_doc, "InnerBox", Base::Vector3d(0, 0, 0), 10, 4, 5);
    nested->addObject(box);
    _assembly->addObject(nested);
    _doc->recompute();

    auto leaves = Assembly::collectInterferenceLeaves(_assembly, false);
    ASSERT_EQ(leaves.size(), 1u);
    EXPECT_NEAR(leaves[0].worldBoundBox.MinX, 100.0, 1e-6);
    EXPECT_NEAR(leaves[0].worldBoundBox.MaxX, 110.0, 1e-6);
    EXPECT_NEAR(leaves[0].worldBoundBox.MinY, 20.0, 1e-6);
    EXPECT_NEAR(leaves[0].worldBoundBox.MaxY, 24.0, 1e-6);
    EXPECT_NEAR(leaves[0].worldBoundBox.MinZ, 30.0, 1e-6);
    EXPECT_NEAR(leaves[0].worldBoundBox.MaxZ, 35.0, 1e-6);
    EXPECT_FALSE(leaves[0].occurrenceSubName.empty());
    EXPECT_NE(leaves[0].occurrenceSubName.find("InnerBox"), std::string::npos);
}

TEST_F(InterferenceScanTest, repeatedLinksShareSourceIdWithDistinctWorldBounds)
{
    auto* source = makeBox(_doc, "SharedBox", Base::Vector3d(0, 0, 0), 10, 10, 10);
    // Keep the definition outside the assembly group so only link occurrences are leaves.
    auto* link1 = freecad_cast<App::Link*>(_doc->addObject("App::Link", "LinkA"));
    auto* link2 = freecad_cast<App::Link*>(_doc->addObject("App::Link", "LinkB"));
    ASSERT_NE(link1, nullptr);
    ASSERT_NE(link2, nullptr);
    link1->setLink(-1, source);
    link2->setLink(-1, source);
    link1->LinkPlacement.setValue(Base::Placement(Base::Vector3d(0, 0, 0), Base::Rotation()));
    link2->LinkPlacement.setValue(Base::Placement(Base::Vector3d(50, 0, 0), Base::Rotation()));
    _assembly->addObject(link1);
    _assembly->addObject(link2);
    _doc->recompute();

    auto leaves = Assembly::collectInterferenceLeaves(_assembly, false);
    ASSERT_EQ(leaves.size(), 2u);
    EXPECT_EQ(leaves[0].sourceId, leaves[1].sourceId);
    EXPECT_NE(leaves[0].occurrenceSubName, leaves[1].occurrenceSubName);

    Base::BoundBox3d expectedA(0, 0, 0, 10, 10, 10);
    Base::BoundBox3d expectedB(50, 0, 0, 60, 10, 10);
    bool sawA = false;
    bool sawB = false;
    for (const auto& leaf : leaves) {
        if (std::abs(leaf.worldBoundBox.MinX - expectedA.MinX) < 1e-6
            && std::abs(leaf.worldBoundBox.MaxX - expectedA.MaxX) < 1e-6) {
            sawA = true;
        }
        if (std::abs(leaf.worldBoundBox.MinX - expectedB.MinX) < 1e-6
            && std::abs(leaf.worldBoundBox.MaxX - expectedB.MaxX) < 1e-6) {
            sawB = true;
        }
    }
    EXPECT_TRUE(sawA);
    EXPECT_TRUE(sawB);
}

TEST_F(InterferenceScanTest, exclusionDoesNotSuppressInvalidOrClearPairs)
{
    std::vector<Assembly::InterferenceLeaf> leaves;
    auto clearA = makeLeaf("A", "srcA", gp_Pnt(0, 0, 0));
    auto clearB = makeLeaf("B", "srcB", gp_Pnt(50, 0, 0));
    leaves.push_back(clearA);
    leaves.push_back(clearB);

    Assembly::InterferenceScanOptions options;
    std::vector<std::pair<std::string, std::string>> excluded {canonicalIds("srcA", "srcB")};
    auto result = Assembly::runInterferenceScan(leaves, options, excluded);
    EXPECT_EQ(result.counts.excludedViolations, 0);
    EXPECT_EQ(result.counts.penetrations, 0);
}

TEST_F(InterferenceScanTest, negativeClearanceRejectedWithoutFalseClear)
{
    std::vector<Assembly::InterferenceLeaf> leaves;
    leaves.push_back(makeLeaf("A", "srcA", gp_Pnt(0, 0, 0)));
    leaves.push_back(makeLeaf("B", "srcB", gp_Pnt(50, 0, 0)));
    Assembly::InterferenceScanOptions options;
    options.clearance = -1.0;
    auto result = Assembly::runInterferenceScan(leaves, options);
    EXPECT_FALSE(result.complete);
    EXPECT_GE(result.counts.invalidInputs, 1);
    EXPECT_EQ(result.counts.clearPairs, 0);
}

TEST_F(InterferenceScanTest, clearPairCountIncludesBroadPhasePrunedPairs)
{
    std::vector<Assembly::InterferenceLeaf> leaves;
    leaves.push_back(makeLeaf("A", "srcA", gp_Pnt(0, 0, 0)));
    leaves.push_back(makeLeaf("B", "srcB", gp_Pnt(50, 0, 0)));
    leaves.push_back(makeLeaf("C", "srcC", gp_Pnt(100, 0, 0)));
    Assembly::InterferenceScanOptions options;
    auto result = Assembly::runInterferenceScan(leaves, options);
    ASSERT_TRUE(result.complete);
    // Three valid leaves => 3 unique pairs, all clear/pruned.
    EXPECT_EQ(result.counts.clearPairs, 3);
    EXPECT_EQ(result.counts.penetrations, 0);
}

TEST_F(InterferenceScanTest, unresolvedExclusionRulesSurviveRewrite)
{
    auto* a = _doc->addObject<App::DocumentObject>("App::Feature", "SourceA");
    auto* b = _doc->addObject<App::DocumentObject>("App::Feature", "SourceB");
    _assembly->addInterferenceExclusion(a, b);

    std::vector<std::pair<App::DocumentObject*, App::DocumentObject*>> pairs;
    pairs.emplace_back(a, b);
    pairs.emplace_back(nullptr, nullptr);
    _assembly->setInterferenceExclusions(pairs);

    const auto rules = _assembly->getInterferenceExclusionRules();
    ASSERT_GE(rules.size(), 2u);
    bool sawUnresolved = false;
    for (const auto& rule : rules) {
        if (!rule.first && !rule.second) {
            sawUnresolved = true;
            EXPECT_FALSE(rule.valid);
        }
    }
    EXPECT_TRUE(sawUnresolved);
}

TEST_F(InterferenceScanTest, sameDocDeletedExclusionEndpointKeepsIdentity)
{
    try {
        auto* a = _doc->addObject<App::DocumentObject>("SourceKeepA");
        auto* b = _doc->addObject<App::DocumentObject>("SourceDeleteB");
        ASSERT_NE(a, nullptr);
        ASSERT_NE(b, nullptr);

        _assembly->addInterferenceExclusion(a, b);
        {
            const auto rules = _assembly->getInterferenceExclusionRules();
            ASSERT_EQ(rules.size(), 1u);
            EXPECT_TRUE(rules[0].valid);
        }

        const std::string deletedName = b->getNameInDocument();
        _doc->removeObject(deletedName.c_str());

        const auto rules = _assembly->getInterferenceExclusionRules();
        ASSERT_EQ(rules.size(), 1u);
        EXPECT_FALSE(rules[0].valid);
        EXPECT_TRUE(
            rules[0].firstIdentity.find(deletedName) != std::string::npos
            || rules[0].secondIdentity.find(deletedName) != std::string::npos
        ) << "first=" << rules[0].firstIdentity << " second=" << rules[0].secondIdentity;
        EXPECT_TRUE(
            rules[0].firstIdentity.find("SourceKeepA") != std::string::npos
            || rules[0].secondIdentity.find("SourceKeepA") != std::string::npos
        );
    }
    catch (const Base::Exception& exc) {
        FAIL() << "Base::Exception: " << exc.what();
    }
}

TEST_F(InterferenceScanTest, detachedIdentityRetentionIsOptInForExclusionProperty)
{
    auto* ordinaryOwner =
        _doc->addObject<App::DocumentObject>("App::FeaturePython", "OrdinaryXLinkOwner");
    auto* ordinaryTarget =
        _doc->addObject<App::DocumentObject>("App::FeaturePython", "OrdinaryXLinkTarget");
    ASSERT_NE(ordinaryOwner, nullptr);
    ASSERT_NE(ordinaryTarget, nullptr);
    auto* ordinary = dynamic_cast<App::PropertyXLinkSubList*>(ordinaryOwner->addDynamicProperty(
        "App::PropertyXLinkSubList",
        "OrdinaryLinks"
    ));
    ASSERT_NE(ordinary, nullptr);
    ordinary->append(ordinaryTarget);
    ASSERT_EQ(ordinary->getSubListValues().size(), 1u);

    auto* exclusionTarget =
        _doc->addObject<App::DocumentObject>("App::FeaturePython", "ExclusionIdentityTarget");
    ASSERT_NE(exclusionTarget, nullptr);
    _assembly->addInterferenceExclusion(ordinaryOwner, exclusionTarget);

    const std::string ordinaryName = ordinaryTarget->getNameInDocument();
    const std::string exclusionName = exclusionTarget->getNameInDocument();
    _doc->removeObject(ordinaryName.c_str());
    _doc->removeObject(exclusionName.c_str());

    const auto& ordinaryLinks = ordinary->getSubListValues();
    ASSERT_EQ(ordinaryLinks.size(), 1u);
    EXPECT_EQ(ordinaryLinks.front().getValue(), nullptr);
    EXPECT_TRUE(
        !ordinaryLinks.front().getObjectName()
        || ordinaryLinks.front().getObjectName()[0] == '\0'
    );

    const auto rules = _assembly->getInterferenceExclusionRules();
    ASSERT_EQ(rules.size(), 1u);
    EXPECT_FALSE(rules.front().valid);
    EXPECT_TRUE(
        rules.front().firstIdentity.find(exclusionName) != std::string::npos
        || rules.front().secondIdentity.find(exclusionName) != std::string::npos
    );
}

TEST_F(InterferenceScanTest, detachedCrossDocExclusionIdentitySurvivesAddAndIndexedRemove)
{
    try {
        const std::string otherDocName =
            App::GetApplication().getUniqueDocumentName("asmInterferenceOther");
        auto* otherDoc =
            App::GetApplication().newDocument(otherDocName.c_str(), "asmInterferenceOtherUser");
        auto* local = _doc->addObject<App::DocumentObject>("SourceLocal");
        auto* remote = otherDoc->addObject<App::DocumentObject>("SourceRemote");
        ASSERT_NE(local, nullptr);
        ASSERT_NE(remote, nullptr);

        // External XLinks require both owner and target documents to be saved.
        const std::string ownerPath = std::string("/tmp/") + _docName + ".FCStd";
        const std::string otherPath = std::string("/tmp/") + otherDocName + ".FCStd";
        _doc->FileName.setValue(ownerPath.c_str());
        otherDoc->FileName.setValue(otherPath.c_str());
        ASSERT_TRUE(_doc->save()) << "Failed to save owner document at " << ownerPath;
        ASSERT_TRUE(otherDoc->save()) << "Failed to save external document at " << otherPath;

        _assembly->addInterferenceExclusion(local, remote);
        {
            const auto rules = _assembly->getInterferenceExclusionRules();
            ASSERT_EQ(rules.size(), 1u);
            EXPECT_TRUE(rules[0].valid);
            EXPECT_TRUE(
                rules[0].firstIdentity.find("SourceLocal") != std::string::npos
                || rules[0].secondIdentity.find("SourceLocal") != std::string::npos
            );
            EXPECT_TRUE(
                rules[0].firstIdentity.find("SourceRemote") != std::string::npos
                || rules[0].secondIdentity.find("SourceRemote") != std::string::npos
            );
        }

        App::GetApplication().closeDocument(otherDocName.c_str());
        otherDoc = nullptr;

        {
            const auto rules = _assembly->getInterferenceExclusionRules();
            ASSERT_EQ(rules.size(), 1u);
            EXPECT_FALSE(rules[0].valid);
            EXPECT_TRUE(
                rules[0].firstIdentity.find("SourceRemote") != std::string::npos
                || rules[0].secondIdentity.find("SourceRemote") != std::string::npos
            ) << "first=" << rules[0].firstIdentity << " second=" << rules[0].secondIdentity;
        }

        auto* c = _doc->addObject<App::DocumentObject>("SourceC");
        auto* d = _doc->addObject<App::DocumentObject>("SourceD");
        ASSERT_NE(c, nullptr);
        ASSERT_NE(d, nullptr);
        _assembly->addInterferenceExclusion(c, d);

        {
            const auto rules = _assembly->getInterferenceExclusionRules();
            ASSERT_EQ(rules.size(), 2u);
            bool sawDetachedRemote = false;
            for (const auto& rule : rules) {
                if (rule.firstIdentity.find("SourceRemote") != std::string::npos
                    || rule.secondIdentity.find("SourceRemote") != std::string::npos) {
                    sawDetachedRemote = true;
                    EXPECT_FALSE(rule.valid);
                }
            }
            EXPECT_TRUE(sawDetachedRemote);
        }

        const auto before = _assembly->getInterferenceExclusionRules();
        std::size_t attachedIndex = before.size();
        for (std::size_t i = 0; i < before.size(); ++i) {
            if (before[i].valid) {
                attachedIndex = i;
                break;
            }
        }
        ASSERT_LT(attachedIndex, before.size());
        _assembly->removeInterferenceExclusionAt(attachedIndex);

        const auto after = _assembly->getInterferenceExclusionRules();
        ASSERT_EQ(after.size(), 1u);
        EXPECT_FALSE(after[0].valid);
        EXPECT_TRUE(
            after[0].firstIdentity.find("SourceRemote") != std::string::npos
            || after[0].secondIdentity.find("SourceRemote") != std::string::npos
        );

        std::remove(ownerPath.c_str());
        std::remove(otherPath.c_str());
    }
    catch (const Base::Exception& exc) {
        FAIL() << "Base::Exception: " << exc.what();
    }
    catch (const std::exception& exc) {
        FAIL() << "std::exception: " << exc.what();
    }
}

TEST_F(InterferenceScanTest, detachedCrossDocExclusionIdentitySurvivesFcstdRoundTrip)
{
    try {
        const std::string otherDocName =
            App::GetApplication().getUniqueDocumentName("asmInterferenceOtherFcstd");
        auto* otherDoc =
            App::GetApplication().newDocument(otherDocName.c_str(), "asmInterferenceOtherFcstdUser");
        auto* local = _doc->addObject<App::DocumentObject>("SourceLocalFcstd");
        auto* remote = otherDoc->addObject<App::DocumentObject>("SourceRemoteFcstd");
        ASSERT_NE(local, nullptr);
        ASSERT_NE(remote, nullptr);

        const std::string ownerPath = std::string("/tmp/") + _docName + "_excl.FCStd";
        const std::string otherPath = std::string("/tmp/") + otherDocName + "_excl.FCStd";
        const std::string otherHiddenPath = otherPath + ".hidden";
        _doc->FileName.setValue(ownerPath.c_str());
        otherDoc->FileName.setValue(otherPath.c_str());
        ASSERT_TRUE(_doc->save());
        ASSERT_TRUE(otherDoc->save());

        _assembly->addInterferenceExclusion(local, remote);
        ASSERT_TRUE(_doc->save()) << "Failed to save owner with live exclusion";

        // Detach remote, then save owner again so FCStd encodes unresolved XLink identity.
        App::GetApplication().closeDocument(otherDocName.c_str());
        otherDoc = nullptr;
        {
            const auto rules = _assembly->getInterferenceExclusionRules();
            ASSERT_EQ(rules.size(), 1u);
            ASSERT_FALSE(rules[0].valid);
            ASSERT_TRUE(
                rules[0].firstIdentity.find("SourceRemoteFcstd") != std::string::npos
                || rules[0].secondIdentity.find("SourceRemoteFcstd") != std::string::npos
            ) << "first=" << rules[0].firstIdentity << " second=" << rules[0].secondIdentity;
        }
        ASSERT_TRUE(_doc->save()) << "Failed to save owner with detached exclusion";

        App::GetApplication().closeDocument(_docName.c_str());
        _doc = nullptr;
        _assembly = nullptr;

        // Hide the remote file so openDocument(owner) cannot auto-restore the XLink.
        ASSERT_EQ(std::rename(otherPath.c_str(), otherHiddenPath.c_str()), 0);

        App::DocumentInitFlags flags;
        flags.createView = false;
        auto* restored = App::GetApplication().openDocument(ownerPath.c_str(), flags);
        ASSERT_NE(restored, nullptr) << "Failed to reopen " << ownerPath;
        // Keep TearDown able to close whatever name restore assigned.
        _docName = restored->getName();
        _doc = restored;
        _assembly = dynamic_cast<Assembly::AssemblyObject*>(restored->getObject("Assembly"));
        ASSERT_NE(_assembly, nullptr);

        const auto rules = _assembly->getInterferenceExclusionRules();
        ASSERT_EQ(rules.size(), 1u);
        EXPECT_FALSE(rules[0].valid);
        EXPECT_TRUE(
            rules[0].firstIdentity.find("SourceLocalFcstd") != std::string::npos
            || rules[0].secondIdentity.find("SourceLocalFcstd") != std::string::npos
        ) << "first=" << rules[0].firstIdentity << " second=" << rules[0].secondIdentity;
        EXPECT_TRUE(
            rules[0].firstIdentity.find("SourceRemoteFcstd") != std::string::npos
            || rules[0].secondIdentity.find("SourceRemoteFcstd") != std::string::npos
        ) << "first=" << rules[0].firstIdentity << " second=" << rules[0].secondIdentity;

        // Restore remote file and open it: rule should resolve without losing identity strings.
        ASSERT_EQ(std::rename(otherHiddenPath.c_str(), otherPath.c_str()), 0);
        auto* reopenedRemote = App::GetApplication().openDocument(otherPath.c_str(), flags);
        ASSERT_NE(reopenedRemote, nullptr);
        const std::string reopenedRemoteName = reopenedRemote->getName();
        const auto resolved = _assembly->getInterferenceExclusionRules();
        ASSERT_EQ(resolved.size(), 1u);
        EXPECT_TRUE(resolved[0].valid) << "first=" << resolved[0].firstIdentity
                                       << " second=" << resolved[0].secondIdentity;
        EXPECT_NE(resolved[0].first, nullptr);
        EXPECT_NE(resolved[0].second, nullptr);
        EXPECT_TRUE(
            resolved[0].firstIdentity.find("SourceRemoteFcstd") != std::string::npos
            || resolved[0].secondIdentity.find("SourceRemoteFcstd") != std::string::npos
        );

        App::GetApplication().closeDocument(reopenedRemoteName.c_str());
        std::remove(ownerPath.c_str());
        std::remove(otherPath.c_str());
        std::remove(otherHiddenPath.c_str());
    }
    catch (const Base::Exception& exc) {
        FAIL() << "Base::Exception: " << exc.what();
    }
    catch (const std::exception& exc) {
        FAIL() << "std::exception: " << exc.what();
    }
}

TEST_F(InterferenceScanTest, staleThenRerunRejectsLateGeneration)
{
    Assembly::InterferenceScanSession session;
    const auto scanA = session.beginScan();
    EXPECT_TRUE(session.isBusy());

    session.markStale();
    EXPECT_TRUE(session.isStale());
    EXPECT_TRUE(session.isBusy());

    const auto scanB = session.beginScan();
    EXPECT_NE(scanA.generation, scanB.generation);
    EXPECT_TRUE(session.isBusy());
    EXPECT_FALSE(session.isStale());

    // Late completion of A must not clear B's ownership.
    EXPECT_FALSE(session.finishScan(scanA.generation));
    EXPECT_TRUE(session.isBusy());
    EXPECT_EQ(session.activeGeneration(), scanB.generation);

    EXPECT_TRUE(session.finishScan(scanB.generation));
    EXPECT_FALSE(session.isBusy());
}

TEST_F(InterferenceScanTest, markStaleThenLateFinishDoesNotAcceptResults)
{
    Assembly::InterferenceScanSession session;
    const auto scanA = session.beginScan();
    session.markStale();
    EXPECT_FALSE(session.finishScan(scanA.generation));
    EXPECT_FALSE(session.isBusy());
    EXPECT_TRUE(session.isStale());
}

TEST_F(InterferenceScanTest, bThenALateFinishKeepsActiveGeneration)
{
    // Reproduces the task-panel B-then-A bug: after B completes, finishing A must
    // remain a no-op against session ownership (UI must early-return on mismatch).
    Assembly::InterferenceScanSession session;
    const auto scanA = session.beginScan();
    const auto scanB = session.beginScan();
    ASSERT_TRUE(session.finishScan(scanB.generation));
    EXPECT_FALSE(session.isBusy());
    EXPECT_EQ(session.activeGeneration(), scanB.generation);

    EXPECT_NE(scanA.generation, session.activeGeneration());
    EXPECT_FALSE(session.finishScan(scanA.generation));
    EXPECT_EQ(session.activeGeneration(), scanB.generation);
    EXPECT_FALSE(session.isBusy());
}

TEST_F(InterferenceScanTest, invalidBoundsAreRebuiltOrReportedNotCountedClear)
{
    std::vector<Assembly::InterferenceLeaf> leaves;
    leaves.push_back(makeLeaf("A", "srcA", gp_Pnt(0, 0, 0)));
    leaves.push_back(makeLeaf("B", "srcB", gp_Pnt(5, 0, 0)));
    // Simulate the Docker probe: shapes present/valid but bounds left default/invalid.
    leaves[0].worldBoundBox = Base::BoundBox3d();
    leaves[1].worldBoundBox = Base::BoundBox3d();
    EXPECT_FALSE(leaves[0].worldBoundBox.IsValid());
    EXPECT_FALSE(leaves[1].worldBoundBox.IsValid());

    Assembly::InterferenceScanOptions options;
    auto result = Assembly::runInterferenceScan(leaves, options);
    EXPECT_GE(result.counts.penetrations, 1);
    EXPECT_EQ(result.counts.clearPairs, 0);
}

TEST_F(InterferenceScanTest, negativeLinearToleranceRejected)
{
    std::vector<Assembly::InterferenceLeaf> leaves;
    leaves.push_back(makeLeaf("A", "srcA", gp_Pnt(0, 0, 0)));
    leaves.push_back(makeLeaf("B", "srcB", gp_Pnt(50, 0, 0)));
    Assembly::InterferenceScanOptions options;
    options.detectionOptions.linearTolerance = -1.0;
    auto result = Assembly::runInterferenceScan(leaves, options);
    EXPECT_FALSE(result.complete);
    EXPECT_GE(result.counts.invalidInputs, 1);
    EXPECT_EQ(result.counts.clearPairs, 0);
}

TEST_F(InterferenceScanTest, collapsedLinkArrayCollectsVirtualOccurrences)
{
    auto* source = makeBox(_doc, "ArraySource", Base::Vector3d(0, 0, 0), 10, 10, 10);
    auto* link = freecad_cast<App::Link*>(_doc->addObject("App::Link", "CollapsedArray"));
    ASSERT_NE(link, nullptr);
    link->setLink(-1, source);
    link->ElementCount.setValue(2);
    link->ShowElement.setValue(false);
    std::vector<Base::Placement> placements {
        Base::Placement(Base::Vector3d(0, 0, 0), Base::Rotation()),
        Base::Placement(Base::Vector3d(50, 0, 0), Base::Rotation())
    };
    link->PlacementList.setValues(placements);
    _assembly->addObject(link);
    _doc->recompute();

    auto leaves = Assembly::collectInterferenceLeaves(_assembly, false);
    ASSERT_EQ(leaves.size(), 2u);
    EXPECT_NE(leaves[0].occurrenceSubName, leaves[1].occurrenceSubName);
    EXPECT_EQ(leaves[0].sourceId, leaves[1].sourceId);

    bool sawNear = false;
    bool sawFar = false;
    for (const auto& leaf : leaves) {
        if (std::abs(leaf.worldBoundBox.MinX - 0.0) < 1e-6
            && std::abs(leaf.worldBoundBox.MaxX - 10.0) < 1e-6) {
            sawNear = true;
        }
        if (std::abs(leaf.worldBoundBox.MinX - 50.0) < 1e-6
            && std::abs(leaf.worldBoundBox.MaxX - 60.0) < 1e-6) {
            sawFar = true;
        }
    }
    EXPECT_TRUE(sawNear);
    EXPECT_TRUE(sawFar);
}

TEST_F(InterferenceScanTest, includeHiddenRespectsVisibilityFilter)
{
    auto* visible = makeBox(_doc, "VisibleBox", Base::Vector3d(0, 0, 0), 10, 10, 10);
    auto* hidden = makeBox(_doc, "HiddenBox", Base::Vector3d(50, 0, 0), 10, 10, 10);
    hidden->Visibility.setValue(false);
    _assembly->addObject(visible);
    _assembly->addObject(hidden);
    _doc->recompute();

    auto withoutHidden = Assembly::collectInterferenceLeaves(_assembly, false);
    ASSERT_EQ(withoutHidden.size(), 1u);
    EXPECT_NE(withoutHidden[0].occurrenceSubName.find("VisibleBox"), std::string::npos);
    EXPECT_TRUE(withoutHidden[0].visible);

    auto withHidden = Assembly::collectInterferenceLeaves(_assembly, true);
    ASSERT_EQ(withHidden.size(), 2u);
    bool sawHidden = false;
    for (const auto& leaf : withHidden) {
        if (leaf.occurrenceSubName.find("HiddenBox") != std::string::npos) {
            sawHidden = true;
            EXPECT_FALSE(leaf.visible);
        }
    }
    EXPECT_TRUE(sawHidden);
}

TEST_F(InterferenceScanTest, nestedPartElementVisibilityIsHonored)
{
    auto* nested = _doc->addObject<App::Part>("NestedPart");
    auto* visible = makeBox(_doc, "VisibleInner", Base::Vector3d(0, 0, 0), 10, 10, 10);
    auto* hidden = makeBox(_doc, "HiddenInner", Base::Vector3d(50, 0, 0), 10, 10, 10);
    nested->addObject(visible);
    nested->addObject(hidden);
    // App::Part does not implement element VisibilityList; nested path visibility
    // follows each ancestor/child Visibility property.
    hidden->Visibility.setValue(false);
    _assembly->addObject(nested);
    _doc->recompute();

    auto withoutHidden = Assembly::collectInterferenceLeaves(_assembly, false);
    ASSERT_EQ(withoutHidden.size(), 1u);
    EXPECT_NE(withoutHidden[0].occurrenceSubName.find("VisibleInner"), std::string::npos);

    auto withHidden = Assembly::collectInterferenceLeaves(_assembly, true);
    ASSERT_EQ(withHidden.size(), 2u);
}

TEST_F(InterferenceScanTest, bodyTipShapeIsCollectedAsSingleLeaf)
{
    try {
        auto* tipBox = makeBox(_doc, "TipBox", Base::Vector3d(0, 0, 0), 10, 4, 5);
        tipBox->Placement.setValue(Base::Placement(Base::Vector3d(20, 30, 40), Base::Rotation()));
        tipBox->recomputeFeature();

        auto* body = _doc->addObject<PartDesign::Body>("Body");
        body->BaseFeature.setValue(tipBox);
        body->Placement.setValue(Base::Placement(Base::Vector3d(100, 0, 0), Base::Rotation()));
        _assembly->addObject(body);
        _doc->recompute();

        ASSERT_NE(body->Tip.getValue(), nullptr);
        EXPECT_TRUE(
            body->Tip.getValue()->isDerivedFrom(Base::Type::fromName("PartDesign::Feature"))
        );
        ASSERT_FALSE(body->Shape.getShape().isNull());

        auto leaves = Assembly::collectInterferenceLeaves(_assembly, false);
        ASSERT_EQ(leaves.size(), 1u) << "diagnostic="
                                     << (leaves.empty() ? "" : leaves[0].diagnostic);
        EXPECT_NE(leaves[0].occurrenceSubName.find("Body"), std::string::npos);
        // Since "PD: Preserve external base feature placement", the internal FeatureBase no
        // longer bakes the source placement into its geometry. Body::onChanged instead derives
        // FeatureBase.Placement as body.globalPlacement().inverse() * base.globalPlacement()
        // when it creates that FeatureBase -- here the body is still at the origin, so the
        // tip box keeps its own (20, 30, 40). The assembly path then applies Body.Placement
        // (100, 0, 0) on top, putting the 10x4x5 box at (120, 30, 40).
        EXPECT_NEAR(leaves[0].worldBoundBox.MinX, 120.0, 1e-4);
        EXPECT_NEAR(leaves[0].worldBoundBox.MaxX, 130.0, 1e-4);
        EXPECT_NEAR(leaves[0].worldBoundBox.MinY, 30.0, 1e-4);
        EXPECT_NEAR(leaves[0].worldBoundBox.MaxY, 34.0, 1e-4);
        EXPECT_NEAR(leaves[0].worldBoundBox.MinZ, 40.0, 1e-4);
        EXPECT_NEAR(leaves[0].worldBoundBox.MaxZ, 45.0, 1e-4);

        auto* other = makeBox(_doc, "OtherTip", Base::Vector3d(0, 0, 0), 2, 2, 2);
        body->BaseFeature.setValue(other);
        _doc->recompute();
        leaves = Assembly::collectInterferenceLeaves(_assembly, false);
        ASSERT_EQ(leaves.size(), 1u);
        // The FeatureBase already exists, so Body::onChanged only repoints its BaseFeature and
        // leaves the (20, 30, 40) it derived on creation in place. The new 2x2x2 tip therefore
        // lands at the same origin as the box it replaced.
        EXPECT_NEAR(leaves[0].worldBoundBox.MinX, 120.0, 1e-4);
        EXPECT_NEAR(leaves[0].worldBoundBox.MaxX, 122.0, 1e-4);
        EXPECT_NEAR(leaves[0].worldBoundBox.MaxY, 32.0, 1e-4);
        EXPECT_NEAR(leaves[0].worldBoundBox.MaxZ, 42.0, 1e-4);
    }
    catch (const Base::Exception& exc) {
        FAIL() << "Base::Exception: " << exc.what();
    }
    catch (const Standard_Failure& exc) {
        FAIL() << "OCCT: " << (exc.GetMessageString() ? exc.GetMessageString() : "?");
    }
    catch (const std::exception& exc) {
        FAIL() << "std::exception: " << exc.what();
    }
}

TEST_F(InterferenceScanTest, rigidAssemblyLinkAppliesLinkPlacementToLeaves)
{
    auto* sub = _doc->addObject<Assembly::AssemblyObject>("SubAssembly");
    auto* subBox = makeBox(_doc, "SubBox", Base::Vector3d(0, 0, 0), 10, 10, 10);
    sub->addObject(subBox);

    auto* link = _doc->addObject<Assembly::AssemblyLink>("RigidSub");
    link->LinkedObject.setValue(sub);
    link->Rigid.setValue(true);
    link->Placement.setValue(Base::Placement(Base::Vector3d(100, 20, 0), Base::Rotation()));
    _assembly->addObject(link);
    link->updateContents();
    _doc->recompute();

    ASSERT_FALSE(link->Group.getValues().empty());
    auto leaves = Assembly::collectInterferenceLeaves(_assembly, false);
    ASSERT_EQ(leaves.size(), 1u);
    EXPECT_NE(leaves[0].occurrenceSubName.find("RigidSub"), std::string::npos);
    EXPECT_NEAR(leaves[0].worldBoundBox.MinX, 100.0, 1e-4);
    EXPECT_NEAR(leaves[0].worldBoundBox.MaxX, 110.0, 1e-4);
    EXPECT_NEAR(leaves[0].worldBoundBox.MinY, 20.0, 1e-4);
    EXPECT_NEAR(leaves[0].worldBoundBox.MaxY, 30.0, 1e-4);
}

TEST_F(InterferenceScanTest, flexibleAssemblyLinkCollectsComponentLeaves)
{
    auto* sub = _doc->addObject<Assembly::AssemblyObject>("SubAssembly");
    auto* boxA = makeBox(_doc, "FlexA", Base::Vector3d(0, 0, 0), 10, 10, 10);
    auto* boxB = makeBox(_doc, "FlexB", Base::Vector3d(40, 0, 0), 10, 10, 10);
    sub->addObject(boxA);
    sub->addObject(boxB);

    auto* link = _doc->addObject<Assembly::AssemblyLink>("FlexibleSub");
    link->LinkedObject.setValue(sub);
    // Seed as rigid so synchronizeComponents copies source placements onto child links,
    // then switch to flexible (independent placements, still distinct world leaves).
    link->Rigid.setValue(true);
    _assembly->addObject(link);
    link->updateContents();
    link->Rigid.setValue(false);
    _doc->recompute();

    ASSERT_GE(link->Group.getValues().size(), 2u);
    auto leaves = Assembly::collectInterferenceLeaves(_assembly, false);
    ASSERT_EQ(leaves.size(), 2u);

    std::ostringstream dump;
    bool sawNear = false;
    bool sawFar = false;
    for (const auto& leaf : leaves) {
        dump << "[" << leaf.occurrenceSubName << " box=(" << leaf.worldBoundBox.MinX << ","
             << leaf.worldBoundBox.MaxX << "," << leaf.worldBoundBox.MinY << ","
             << leaf.worldBoundBox.MaxY << ")] ";
        EXPECT_NE(leaf.occurrenceSubName.find("FlexibleSub"), std::string::npos);
        if (std::abs(leaf.worldBoundBox.MinX - 0.0) < 1e-4
            && std::abs(leaf.worldBoundBox.MaxX - 10.0) < 1e-4) {
            sawNear = true;
        }
        if (std::abs(leaf.worldBoundBox.MinX - 40.0) < 1e-4
            && std::abs(leaf.worldBoundBox.MaxX - 50.0) < 1e-4) {
            sawFar = true;
        }
    }
    EXPECT_TRUE(sawNear) << dump.str();
    EXPECT_TRUE(sawFar) << dump.str();
}

TEST_F(InterferenceScanTest, clearancePropertyUndoRedoRoundTrip)
{
    _assembly->setInterferenceClearance(2.5);
    EXPECT_DOUBLE_EQ(_assembly->getInterferenceClearance(), 2.5);

    _doc->openTransaction("Set clearance");
    _assembly->setInterferenceClearance(7.0);
    _doc->commitTransaction();
    EXPECT_DOUBLE_EQ(_assembly->getInterferenceClearance(), 7.0);

    _doc->undo();
    EXPECT_DOUBLE_EQ(_assembly->getInterferenceClearance(), 2.5);
    _doc->redo();
    EXPECT_DOUBLE_EQ(_assembly->getInterferenceClearance(), 7.0);
}

TEST_F(InterferenceScanTest, resolveSelectedComponentUsesFirstOccurrenceUnderRoot)
{
    auto* casePart = _doc->addObject<App::Part>("AssemblyCase");
    auto* nested = _doc->addObject<App::Part>("CaseSnapWindowRightPocket");
    auto* faceOwner = makeBox(_doc, "PocketBox", Base::Vector3d(0, 0, 0), 10, 10, 10);
    nested->addObject(faceOwner);
    casePart->addObject(nested);

    auto* spool = _doc->addObject<App::Part>("AssemblySpool");
    auto* gear = makeBox(_doc, "Spool_GearTipRelief", Base::Vector3d(50, 0, 0), 10, 10, 10);
    spool->addObject(gear);

    _assembly->addObject(casePart);
    _assembly->addObject(spool);
    _doc->recompute();

    Assembly::InterferenceComponentOccurrence caseOcc;
    ASSERT_TRUE(Assembly::resolveInterferenceComponentOccurrence(
        _assembly,
        _assembly,
        "AssemblyCase.CaseSnapWindowRightPocket.PocketBox.",
        caseOcc
    ));
    EXPECT_EQ(caseOcc.component, casePart);
    EXPECT_EQ(caseOcc.occurrencePrefix, "AssemblyCase.");

    Assembly::InterferenceComponentOccurrence spoolOcc;
    ASSERT_TRUE(Assembly::resolveInterferenceComponentOccurrence(
        _assembly,
        _assembly,
        "AssemblySpool.Spool_GearTipRelief.",
        spoolOcc
    ));
    EXPECT_EQ(spoolOcc.component, spool);
    EXPECT_EQ(spoolOcc.occurrencePrefix, "AssemblySpool.");
}

TEST_F(InterferenceScanTest, ordinaryGroupPathPrefixAndPenetration)
{
    auto* folder = freecad_cast<App::DocumentObjectGroup*>(
        _doc->addObject("App::DocumentObjectGroup", "Folder")
    );
    ASSERT_NE(folder, nullptr);
    auto* componentA = _doc->addObject<App::Part>("ComponentA");
    auto* nested = _doc->addObject<App::Part>("NestedA");
    auto* solidA = makeBox(_doc, "SolidA", Base::Vector3d(0, 0, 0), 20, 20, 20);
    nested->addObject(solidA);
    componentA->addObject(nested);
    folder->addObject(componentA);

    auto* componentB = _doc->addObject<App::Part>("ComponentB");
    componentB->Placement.setValue(Base::Placement(Base::Vector3d(10, 0, 0), Base::Rotation()));
    auto* solidB = makeBox(_doc, "SolidB", Base::Vector3d(0, 0, 0), 20, 20, 20);
    componentB->addObject(solidB);

    _assembly->addObject(folder);
    _assembly->addObject(componentB);
    _doc->recompute();

    auto listed = Assembly::listInterferenceComponentOccurrences(_assembly, false);
    ASSERT_EQ(listed.size(), 2u);
    bool sawFolderPrefix = false;
    for (const auto& occ : listed) {
        if (occ.occurrencePrefix == "Folder.ComponentA.") {
            sawFolderPrefix = true;
        }
    }
    EXPECT_TRUE(sawFolderPrefix);

    Assembly::InterferenceScanOptions options;
    auto result = Assembly::runInterferenceScanAllVisibleComponents(_assembly, false, options);
    ASSERT_TRUE(result.complete);
    EXPECT_GE(result.counts.penetrations, 1);
    bool sawFolderLeaf = false;
    for (const auto& pair : result.pairs) {
        const auto& a = result.leaves[pair.leafIndexA].occurrenceSubName;
        const auto& b = result.leaves[pair.leafIndexB].occurrenceSubName;
        if (a.rfind("Folder.ComponentA.", 0) == 0 || b.rfind("Folder.ComponentA.", 0) == 0) {
            sawFolderLeaf = true;
            EXPECT_TRUE(
                a.rfind("Folder.ComponentA.", 0) == 0 || b.rfind("Folder.ComponentA.", 0) == 0
            );
            EXPECT_TRUE(a.rfind("ComponentB.", 0) == 0 || b.rfind("ComponentB.", 0) == 0);
        }
    }
    EXPECT_TRUE(sawFolderLeaf);
}

TEST_F(InterferenceScanTest, hiddenParentGroupOmitsComponentUnlessIncludeHidden)
{
    auto* folder = freecad_cast<App::DocumentObjectGroup*>(
        _doc->addObject("App::DocumentObjectGroup", "Folder")
    );
    ASSERT_NE(folder, nullptr);
    auto* hiddenComp = _doc->addObject<App::Part>("HiddenComp");
    auto* solidH = makeBox(_doc, "SolidH", Base::Vector3d(5, 0, 0), 20, 20, 20);
    hiddenComp->addObject(solidH);
    folder->addObject(hiddenComp);
    folder->Visibility.setValue(false);

    auto* visible = _doc->addObject<App::Part>("VisibleComp");
    auto* solidV = makeBox(_doc, "SolidV", Base::Vector3d(0, 0, 0), 20, 20, 20);
    visible->addObject(solidV);

    auto* other = _doc->addObject<App::Part>("OtherComp");
    other->Placement.setValue(Base::Placement(Base::Vector3d(100, 0, 0), Base::Rotation()));
    auto* solidO = makeBox(_doc, "SolidO", Base::Vector3d(0, 0, 0), 10, 10, 10);
    other->addObject(solidO);

    _assembly->addObject(folder);
    _assembly->addObject(visible);
    _assembly->addObject(other);
    _doc->recompute();

    auto without = Assembly::listInterferenceComponentOccurrences(_assembly, false);
    for (const auto& occ : without) {
        EXPECT_EQ(occ.occurrencePrefix.find("HiddenComp"), std::string::npos);
    }

    Assembly::InterferenceScanOptions options;
    auto scanWithout =
        Assembly::runInterferenceScanAllVisibleComponents(_assembly, false, options);
    for (const auto& pair : scanWithout.pairs) {
        EXPECT_EQ(
            scanWithout.leaves[pair.leafIndexA].occurrenceSubName.find("HiddenComp"),
            std::string::npos
        );
        EXPECT_EQ(
            scanWithout.leaves[pair.leafIndexB].occurrenceSubName.find("HiddenComp"),
            std::string::npos
        );
    }

    auto withHidden = Assembly::listInterferenceComponentOccurrences(_assembly, true);
    bool listedHidden = false;
    for (const auto& occ : withHidden) {
        if (occ.occurrencePrefix.find("HiddenComp") != std::string::npos) {
            listedHidden = true;
        }
    }
    EXPECT_TRUE(listedHidden);

    auto scanWith = Assembly::runInterferenceScanAllVisibleComponents(_assembly, true, options);
    bool pairMentionsHidden = false;
    for (const auto& pair : scanWith.pairs) {
        const auto& a = scanWith.leaves[pair.leafIndexA].occurrenceSubName;
        const auto& b = scanWith.leaves[pair.leafIndexB].occurrenceSubName;
        if (a.find("HiddenComp") != std::string::npos
            || b.find("HiddenComp") != std::string::npos) {
            pairMentionsHidden = true;
        }
    }
    EXPECT_TRUE(pairMentionsHidden);
}

TEST_F(InterferenceScanTest, nestedOrganizerGroupsPreserveFullPrefixes)
{
    auto* outer = freecad_cast<App::DocumentObjectGroup*>(
        _doc->addObject("App::DocumentObjectGroup", "OuterFolder")
    );
    auto* inner = freecad_cast<App::DocumentObjectGroup*>(
        _doc->addObject("App::DocumentObjectGroup", "InnerFolder")
    );
    ASSERT_NE(outer, nullptr);
    ASSERT_NE(inner, nullptr);
    auto* comp = _doc->addObject<App::Part>("DeepComp");
    auto* box = makeBox(_doc, "DeepBox", Base::Vector3d(0, 0, 0), 10, 10, 10);
    comp->addObject(box);
    inner->addObject(comp);
    outer->addObject(inner);
    _assembly->addObject(outer);
    _doc->recompute();

    auto listed = Assembly::listInterferenceComponentOccurrences(_assembly, true);
    ASSERT_EQ(listed.size(), 1u);
    EXPECT_EQ(listed[0].occurrencePrefix, "OuterFolder.InnerFolder.DeepComp.");
    EXPECT_NE(listed[0].displayPath.find("OuterFolder"), std::string::npos);

    auto snap = Assembly::prepareInterferenceComponentScanSnapshot(_assembly, true);
    ASSERT_EQ(snap.leaves.size(), 1u);
    EXPECT_EQ(snap.leaves[0].occurrenceSubName.rfind("OuterFolder.InnerFolder.DeepComp.", 0), 0);
}

TEST_F(InterferenceScanTest, collapsedLinkArrayElementsAreDistinctOccurrences)
{
    auto* source = makeBox(_doc, "ArraySource", Base::Vector3d(0, 0, 0), 20, 20, 20);
    auto* link = freecad_cast<App::Link*>(_doc->addObject("App::Link", "ArrayLink"));
    ASSERT_NE(link, nullptr);
    link->setLink(-1, source);
    link->ElementCount.setValue(2);
    link->ShowElement.setValue(false);
    std::vector<Base::Placement> placements {
        Base::Placement(Base::Vector3d(0, 0, 0), Base::Rotation()),
        Base::Placement(Base::Vector3d(10, 0, 0), Base::Rotation())
    };
    link->PlacementList.setValues(placements);
    _assembly->addObject(link);
    _doc->recompute();

    auto listed = Assembly::listInterferenceComponentOccurrences(_assembly, false);
    ASSERT_EQ(listed.size(), 2u);
    EXPECT_EQ(listed[0].occurrencePrefix, "ArrayLink.0.");
    EXPECT_EQ(listed[1].occurrencePrefix, "ArrayLink.1.");

    Assembly::InterferenceScanOptions options;
    auto result = Assembly::runInterferenceScanAllVisibleComponents(_assembly, false, options);
    ASSERT_TRUE(result.complete);
    EXPECT_GE(result.counts.penetrations, 1);
    ASSERT_FALSE(result.pairs.empty());
    const auto& a = result.leaves[result.pairs.front().leafIndexA].occurrenceSubName;
    const auto& b = result.leaves[result.pairs.front().leafIndexB].occurrenceSubName;
    EXPECT_TRUE(
        (a.rfind("ArrayLink.0.", 0) == 0 && b.rfind("ArrayLink.1.", 0) == 0)
        || (a.rfind("ArrayLink.1.", 0) == 0 && b.rfind("ArrayLink.0.", 0) == 0)
    );
}

TEST_F(InterferenceScanTest, linkArrayDoesNotPairLeavesWithinOneElement)
{
    // Two solids under a linked Part definition: one array element must not
    // report its own internal overlap as a cross-occurrence pair.
    auto* def = _doc->addObject<App::Part>("ArrayDef");
    auto* a = makeBox(_doc, "DefA", Base::Vector3d(0, 0, 0), 10, 10, 10);
    auto* b = makeBox(_doc, "DefB", Base::Vector3d(5, 0, 0), 10, 10, 10);
    def->addObject(a);
    def->addObject(b);
    auto* link = freecad_cast<App::Link*>(_doc->addObject("App::Link", "FarArray"));
    ASSERT_NE(link, nullptr);
    link->setLink(-1, def);
    link->ElementCount.setValue(2);
    link->ShowElement.setValue(false);
    std::vector<Base::Placement> placements {
        Base::Placement(Base::Vector3d(0, 0, 0), Base::Rotation()),
        Base::Placement(Base::Vector3d(200, 0, 0), Base::Rotation())
    };
    link->PlacementList.setValues(placements);
    _assembly->addObject(link);
    _doc->recompute();

    Assembly::InterferenceScanOptions options;
    auto result = Assembly::runInterferenceScanAllVisibleComponents(_assembly, false, options);
    ASSERT_TRUE(result.complete);
    for (const auto& pair : result.pairs) {
        const auto& pa = result.leaves[pair.leafIndexA].occurrenceSubName;
        const auto& pb = result.leaves[pair.leafIndexB].occurrenceSubName;
        const bool both0 = pa.rfind("FarArray.0.", 0) == 0 && pb.rfind("FarArray.0.", 0) == 0;
        const bool both1 = pa.rfind("FarArray.1.", 0) == 0 && pb.rfind("FarArray.1.", 0) == 0;
        EXPECT_FALSE(both0);
        EXPECT_FALSE(both1);
    }
}

TEST_F(InterferenceScanTest, hiddenLinkArrayElementRespectsIncludeHidden)
{
    auto* source = makeBox(_doc, "VisSource", Base::Vector3d(0, 0, 0), 20, 20, 20);
    auto* link = freecad_cast<App::Link*>(_doc->addObject("App::Link", "VisArray"));
    ASSERT_NE(link, nullptr);
    link->setLink(-1, source);
    link->ElementCount.setValue(2);
    link->ShowElement.setValue(false);
    std::vector<Base::Placement> placements {
        Base::Placement(Base::Vector3d(0, 0, 0), Base::Rotation()),
        Base::Placement(Base::Vector3d(10, 0, 0), Base::Rotation())
    };
    link->PlacementList.setValues(placements);
    link->setElementVisible("1", false);
    _assembly->addObject(link);
    _doc->recompute();

    auto without = Assembly::listInterferenceComponentOccurrences(_assembly, false);
    for (const auto& occ : without) {
        EXPECT_NE(occ.occurrencePrefix, "VisArray.1.");
    }
    auto withHidden = Assembly::listInterferenceComponentOccurrences(_assembly, true);
    bool saw1 = false;
    for (const auto& occ : withHidden) {
        if (occ.occurrencePrefix == "VisArray.1.") {
            saw1 = true;
        }
    }
    EXPECT_TRUE(saw1);
}

TEST_F(InterferenceScanTest, expandedLinkArrayElementsAreDistinctOccurrences)
{
    auto* source = makeBox(_doc, "ExpSource", Base::Vector3d(0, 0, 0), 20, 20, 20);
    auto* link = freecad_cast<App::Link*>(_doc->addObject("App::Link", "ExpArray"));
    ASSERT_NE(link, nullptr);
    link->setLink(-1, source);
    link->ShowElement.setValue(true);
    link->ElementCount.setValue(2);
    _assembly->addObject(link);
    _doc->recompute();

    ASSERT_EQ(link->ElementList.getSize(), 2);
    auto* elt0 = freecad_cast<App::LinkElement*>(link->ElementList.getValues()[0]);
    auto* elt1 = freecad_cast<App::LinkElement*>(link->ElementList.getValues()[1]);
    ASSERT_NE(elt0, nullptr);
    ASSERT_NE(elt1, nullptr);
    elt0->Placement.setValue(Base::Placement(Base::Vector3d(0, 0, 0), Base::Rotation()));
    elt1->Placement.setValue(Base::Placement(Base::Vector3d(10, 0, 0), Base::Rotation()));
    _doc->recompute();

    const std::string prefix0 =
        std::string("ExpArray.") + elt0->getNameInDocument() + ".";
    const std::string prefix1 =
        std::string("ExpArray.") + elt1->getNameInDocument() + ".";

    auto listed = Assembly::listInterferenceComponentOccurrences(_assembly, false);
    ASSERT_EQ(listed.size(), 2u);
    EXPECT_EQ(listed[0].occurrencePrefix, prefix0);
    EXPECT_EQ(listed[1].occurrencePrefix, prefix1);

    auto snap = Assembly::prepareInterferenceComponentScanSnapshot(_assembly, false);
    ASSERT_EQ(snap.leaves.size(), 2u);
    EXPECT_EQ(snap.leaves[0].occurrenceSubName.rfind(prefix0, 0), 0);
    EXPECT_EQ(snap.leaves[1].occurrenceSubName.rfind(prefix1, 0), 0);

    Assembly::InterferenceComponentOccurrence occ;
    ASSERT_TRUE(Assembly::resolveInterferenceComponentOccurrence(
        _assembly,
        _assembly,
        std::string("ExpArray.") + elt0->getNameInDocument() + ".Face1",
        occ
    ));
    EXPECT_EQ(occ.occurrencePrefix, prefix0);

    // Selection rooted at the LinkElement itself.
    ASSERT_TRUE(Assembly::resolveInterferenceComponentOccurrence(
        _assembly,
        elt1,
        "Face1",
        occ
    ));
    EXPECT_EQ(occ.occurrencePrefix, prefix1);

    // Malformed / oversized index → resolution fails (all-components fallback).
    EXPECT_FALSE(Assembly::resolveInterferenceComponentOccurrence(
        _assembly,
        _assembly,
        "ExpArray.999999999999999999999999.Face1",
        occ
    ));
    EXPECT_FALSE(Assembly::resolveInterferenceComponentOccurrence(
        _assembly,
        _assembly,
        "ExpArray.notAnElement.Face1",
        occ
    ));
    auto badScope = Assembly::resolveInterferenceSelectionScope(
        _assembly,
        {Assembly::InterferenceSelectionHandle {_assembly, "ExpArray.notAnElement.Face1"},
         Assembly::InterferenceSelectionHandle {_assembly, "ExpArray.alsoBad.Face1"}}
    );
    EXPECT_EQ(badScope.mode, Assembly::InterferenceScanScopeMode::AllComponents);

    // Collapsed-style numeric address is rejected while expanded.
    EXPECT_FALSE(Assembly::resolveInterferenceComponentOccurrence(
        _assembly,
        _assembly,
        "ExpArray.0.Face1",
        occ
    ));
}

TEST_F(InterferenceScanTest, expandedLinkArraySelectedPairDoesNotLeakSibling)
{
    auto* source = makeBox(_doc, "LeakSource", Base::Vector3d(0, 0, 0), 20, 20, 20);
    auto* link = freecad_cast<App::Link*>(_doc->addObject("App::Link", "LeakArray"));
    ASSERT_NE(link, nullptr);
    link->setLink(-1, source);
    link->ShowElement.setValue(true);
    link->ElementCount.setValue(2);
    auto* other = _doc->addObject<App::Part>("LeakOther");
    auto* otherBox = makeBox(_doc, "LeakOtherBox", Base::Vector3d(0, 0, 0), 20, 20, 20);
    other->addObject(otherBox);
    other->Placement.setValue(Base::Placement(Base::Vector3d(10, 0, 0), Base::Rotation()));
    _assembly->addObject(link);
    _assembly->addObject(other);
    _doc->recompute();

    ASSERT_EQ(link->ElementList.getSize(), 2);
    auto* elt0 = freecad_cast<App::LinkElement*>(link->ElementList.getValues()[0]);
    auto* elt1 = freecad_cast<App::LinkElement*>(link->ElementList.getValues()[1]);
    ASSERT_NE(elt0, nullptr);
    ASSERT_NE(elt1, nullptr);
    // elt0 clear of Other; elt1 penetrates Other.
    elt0->Placement.setValue(Base::Placement(Base::Vector3d(200, 0, 0), Base::Rotation()));
    elt1->Placement.setValue(Base::Placement(Base::Vector3d(0, 0, 0), Base::Rotation()));
    _doc->recompute();

    const std::string prefix0 =
        std::string("LeakArray.") + elt0->getNameInDocument() + ".";
    const std::string prefix1 =
        std::string("LeakArray.") + elt1->getNameInDocument() + ".";

    Assembly::InterferenceComponentOccurrence clearOcc;
    Assembly::InterferenceComponentOccurrence hitOcc;
    Assembly::InterferenceComponentOccurrence otherOcc;
    ASSERT_TRUE(Assembly::resolveInterferenceComponentOccurrence(
        _assembly, _assembly, prefix0 + "Face1", clearOcc
    ));
    ASSERT_TRUE(Assembly::resolveInterferenceComponentOccurrence(
        _assembly, _assembly, prefix1 + "Face1", hitOcc
    ));
    ASSERT_TRUE(Assembly::resolveInterferenceComponentOccurrence(
        _assembly, _assembly, "LeakOther.LeakOtherBox.Face1", otherOcc
    ));
    EXPECT_EQ(clearOcc.occurrencePrefix, prefix0);
    EXPECT_EQ(hitOcc.occurrencePrefix, prefix1);

    auto snap = Assembly::prepareInterferenceComponentScanSnapshot(_assembly, false);
    auto leavesFor = [&](const std::string& prefix) {
        std::vector<Assembly::InterferenceLeaf> out;
        for (const auto& leaf : snap.leaves) {
            if (leaf.occurrenceSubName.rfind(prefix, 0) == 0) {
                out.push_back(leaf);
            }
        }
        return out;
    };
    auto clearLeaves = leavesFor(prefix0);
    auto hitLeaves = leavesFor(prefix1);
    auto otherLeaves = leavesFor("LeakOther.");
    ASSERT_FALSE(clearLeaves.empty());
    ASSERT_FALSE(hitLeaves.empty());
    ASSERT_FALSE(otherLeaves.empty());

    Assembly::InterferenceScanOptions options;
    auto clearPair =
        Assembly::runInterferenceScanBetweenLeafSets(clearLeaves, otherLeaves, options);
    ASSERT_TRUE(clearPair.complete);
    EXPECT_EQ(clearPair.counts.penetrations, 0);
    for (const auto& pair : clearPair.pairs) {
        const auto& a = clearPair.leaves[pair.leafIndexA].occurrenceSubName;
        const auto& b = clearPair.leaves[pair.leafIndexB].occurrenceSubName;
        EXPECT_TRUE(a.rfind(prefix1, 0) != 0 && b.rfind(prefix1, 0) != 0);
    }

    auto hitPair = Assembly::runInterferenceScanBetweenLeafSets(hitLeaves, otherLeaves, options);
    ASSERT_TRUE(hitPair.complete);
    EXPECT_GE(hitPair.counts.penetrations, 1);
    bool sawHit = false;
    for (const auto& pair : hitPair.pairs) {
        const auto& a = hitPair.leaves[pair.leafIndexA].occurrenceSubName;
        const auto& b = hitPair.leaves[pair.leafIndexB].occurrenceSubName;
        if ((a.rfind(prefix1, 0) == 0 && b.rfind("LeakOther.", 0) == 0)
            || (b.rfind(prefix1, 0) == 0 && a.rfind("LeakOther.", 0) == 0)) {
            sawHit = true;
        }
        EXPECT_TRUE(a.rfind(prefix0, 0) != 0 && b.rfind(prefix0, 0) != 0);
    }
    EXPECT_TRUE(sawHit);

    // No intra-element pairs when scanning all expanded elements.
    auto all = Assembly::runInterferenceScanAcrossComponents(snap, options);
    ASSERT_TRUE(all.complete);
    for (const auto& pair : all.pairs) {
        const auto& a = all.leaves[pair.leafIndexA].occurrenceSubName;
        const auto& b = all.leaves[pair.leafIndexB].occurrenceSubName;
        EXPECT_FALSE(a.rfind(prefix0, 0) == 0 && b.rfind(prefix0, 0) == 0);
        EXPECT_FALSE(a.rfind(prefix1, 0) == 0 && b.rfind(prefix1, 0) == 0);
    }
}

TEST_F(InterferenceScanTest, hiddenExpandedLinkElementRespectsIncludeHidden)
{
    auto* source = makeBox(_doc, "HidExpSource", Base::Vector3d(0, 0, 0), 10, 10, 10);
    auto* link = freecad_cast<App::Link*>(_doc->addObject("App::Link", "HidExp"));
    ASSERT_NE(link, nullptr);
    link->setLink(-1, source);
    link->ShowElement.setValue(true);
    link->ElementCount.setValue(2);
    _assembly->addObject(link);
    _doc->recompute();
    ASSERT_EQ(link->ElementList.getSize(), 2);
    auto* elt1 = freecad_cast<App::LinkElement*>(link->ElementList.getValues()[1]);
    ASSERT_NE(elt1, nullptr);
    elt1->Visibility.setValue(false);
    _doc->recompute();

    const std::string prefix1 =
        std::string("HidExp.") + elt1->getNameInDocument() + ".";
    auto without = Assembly::listInterferenceComponentOccurrences(_assembly, false);
    for (const auto& occ : without) {
        EXPECT_NE(occ.occurrencePrefix, prefix1);
    }
    auto withHidden = Assembly::listInterferenceComponentOccurrences(_assembly, true);
    bool saw = false;
    for (const auto& occ : withHidden) {
        if (occ.occurrencePrefix == prefix1) {
            saw = true;
        }
    }
    EXPECT_TRUE(saw);
}

TEST_F(InterferenceScanTest, workerSnapshotPreservesInvalidLeafDiagnostics)
{
    Assembly::InterferenceComponentScanSnapshot snap;
    Assembly::InterferenceComponentOccurrence occ;
    occ.occurrencePrefix = "Only.";
    occ.displayPath = "Only";
    snap.components.push_back(occ);

    Assembly::InterferenceLeaf bad;
    bad.occurrenceSubName = "Only.Bad.";
    bad.displayPath = "Only.Bad";
    bad.sourceId = "doc#Bad";
    bad.shapeValid = false;
    bad.diagnostic = "synthetic null shape";
    snap.leaves.push_back(bad);
    snap.componentIndexOfLeaf.push_back(0);

    Assembly::InterferenceScanOptions options;
    auto result = Assembly::runInterferenceScanAcrossComponents(snap, options, {});
    EXPECT_FALSE(result.complete);
    EXPECT_GT(result.counts.invalidInputs, 0);
    ASSERT_FALSE(result.componentIssues.empty());
    EXPECT_NE(result.componentIssues.front().diagnostic.find("synthetic"), std::string::npos);
}

TEST_F(InterferenceScanTest, emptyOrSingleComponentInvalidOptionsAndCancel)
{
    Assembly::InterferenceScanOptions badClearance;
    badClearance.clearance = -1.0;
    auto r1 = Assembly::runInterferenceScanAllVisibleComponents(_assembly, false, badClearance);
    EXPECT_FALSE(r1.complete);
    EXPECT_GT(r1.counts.invalidInputs, 0);

    Assembly::InterferenceScanOptions badTol;
    badTol.detectionOptions.linearTolerance = -1.0;
    auto r2 = Assembly::runInterferenceScanAllVisibleComponents(_assembly, false, badTol);
    EXPECT_FALSE(r2.complete);
    EXPECT_GT(r2.counts.invalidInputs, 0);

    Assembly::InterferenceScanOptions nanTol;
    nanTol.detectionOptions.linearTolerance = std::numeric_limits<double>::quiet_NaN();
    auto r3 = Assembly::runInterferenceScanAllVisibleComponents(_assembly, false, nanTol);
    EXPECT_FALSE(r3.complete);
    EXPECT_GT(r3.counts.invalidInputs, 0);

    std::atomic<bool> cancel {true};
    Assembly::InterferenceScanOptions cancelled;
    cancelled.cancelFlag = &cancel;
    auto r4 = Assembly::runInterferenceScanAllVisibleComponents(_assembly, false, cancelled);
    EXPECT_TRUE(r4.cancelled);

    auto* only = _doc->addObject<App::Part>("OnlyComp");
    auto* box = makeBox(_doc, "OnlyBox", Base::Vector3d(0, 0, 0), 10, 10, 10);
    only->addObject(box);
    _assembly->addObject(only);
    _doc->recompute();

    auto r5 = Assembly::runInterferenceScanAllVisibleComponents(_assembly, false, badClearance);
    EXPECT_FALSE(r5.complete);
    EXPECT_GT(r5.counts.invalidInputs, 0);

    cancel.store(true);
    auto r6 = Assembly::runInterferenceScanAllVisibleComponents(_assembly, false, cancelled);
    EXPECT_TRUE(r6.cancelled);
}

TEST_F(InterferenceScanTest, selectionCardinalityExactTwoSubelements)
{
    auto* a = _doc->addObject<App::Part>("SelA");
    auto* boxA = makeBox(_doc, "SelBoxA", Base::Vector3d(0, 0, 0), 10, 10, 10);
    a->addObject(boxA);
    auto* b = _doc->addObject<App::Part>("SelB");
    auto* boxB = makeBox(_doc, "SelBoxB", Base::Vector3d(50, 0, 0), 10, 10, 10);
    b->addObject(boxB);
    auto* c = _doc->addObject<App::Part>("SelC");
    auto* boxC = makeBox(_doc, "SelBoxC", Base::Vector3d(100, 0, 0), 10, 10, 10);
    c->addObject(boxC);
    _assembly->addObject(a);
    _assembly->addObject(b);
    _assembly->addObject(c);
    _doc->recompute();

    using Assembly::InterferenceSelectionHandle;
    using Assembly::InterferenceScanScopeMode;

    auto none = Assembly::resolveInterferenceSelectionScope(_assembly, {});
    EXPECT_EQ(none.mode, InterferenceScanScopeMode::AllComponents);

    auto one = Assembly::resolveInterferenceSelectionScope(
        _assembly,
        {InterferenceSelectionHandle {_assembly, "SelA.SelBoxA."}}
    );
    EXPECT_EQ(one.mode, InterferenceScanScopeMode::AllComponents);
    EXPECT_EQ(one.subelementHandleCount, 1);

    auto two = Assembly::resolveInterferenceSelectionScope(
        _assembly,
        {InterferenceSelectionHandle {_assembly, "SelA.SelBoxA."},
         InterferenceSelectionHandle {_assembly, "SelB.SelBoxB."}}
    );
    EXPECT_EQ(two.mode, InterferenceScanScopeMode::SelectedPair);
    EXPECT_EQ(two.subelementHandleCount, 2);

    auto same = Assembly::resolveInterferenceSelectionScope(
        _assembly,
        {InterferenceSelectionHandle {_assembly, "SelA.SelBoxA.Face1"},
         InterferenceSelectionHandle {_assembly, "SelA.SelBoxA.Face2"}}
    );
    EXPECT_EQ(same.mode, InterferenceScanScopeMode::AllComponents);
    EXPECT_EQ(same.distinctOccurrenceCount, 1);

    auto three = Assembly::resolveInterferenceSelectionScope(
        _assembly,
        {InterferenceSelectionHandle {_assembly, "SelA.SelBoxA."},
         InterferenceSelectionHandle {_assembly, "SelB.SelBoxB."},
         InterferenceSelectionHandle {_assembly, "SelC.SelBoxC."}}
    );
    EXPECT_EQ(three.mode, InterferenceScanScopeMode::AllComponents);
    EXPECT_EQ(three.subelementHandleCount, 3);

    auto threeToTwo = Assembly::resolveInterferenceSelectionScope(
        _assembly,
        {InterferenceSelectionHandle {_assembly, "SelA.SelBoxA.Face1"},
         InterferenceSelectionHandle {_assembly, "SelA.SelBoxA.Face2"},
         InterferenceSelectionHandle {_assembly, "SelB.SelBoxB."}}
    );
    EXPECT_EQ(threeToTwo.mode, InterferenceScanScopeMode::AllComponents);
    EXPECT_EQ(threeToTwo.subelementHandleCount, 3);
    EXPECT_EQ(threeToTwo.distinctOccurrenceCount, 2);

    auto wholes = Assembly::resolveInterferenceSelectionScope(
        _assembly,
        {InterferenceSelectionHandle {a, {}}, InterferenceSelectionHandle {b, {}}}
    );
    EXPECT_EQ(wholes.mode, InterferenceScanScopeMode::SelectedPair);
    EXPECT_EQ(wholes.subelementHandleCount, 2);
    EXPECT_EQ(wholes.distinctOccurrenceCount, 2);
    EXPECT_EQ(wholes.first.occurrencePrefix, "SelA.");
    EXPECT_EQ(wholes.second.occurrencePrefix, "SelB.");

    // A third whole-object endpoint disables selected-pair mode.
    auto threeWholes = Assembly::resolveInterferenceSelectionScope(
        _assembly,
        {InterferenceSelectionHandle {a, {}},
         InterferenceSelectionHandle {b, {}},
         InterferenceSelectionHandle {c, {}}}
    );
    EXPECT_EQ(threeWholes.mode, InterferenceScanScopeMode::AllComponents);
    EXPECT_EQ(threeWholes.subelementHandleCount, 3);
}

TEST_F(InterferenceScanTest, selectionExTnpPathsPreferSharedRootOverUnrelatedEditAssembly)
{
    // Mimic FreeCAD consolidating two face picks into one SelectionEx object
    // (the real assembly) with TNP-encoded SubElementNames, while an unrelated
    // assembly remains in edit mode.
    auto* unrelatedEdit = _doc->addObject<Assembly::AssemblyObject>("UnrelatedEditAssembly");
    ASSERT_NE(unrelatedEdit, nullptr);

    auto* casePart = _doc->addObject<App::Part>("AssemblyCase");
    auto* caseBox = makeBox(_doc, "CaseSnapWindowRightPocket", Base::Vector3d(0, 0, 0), 20, 20, 20);
    casePart->addObject(caseBox);

    auto* spoolPart = _doc->addObject<App::Part>("AssemblySpool");
    spoolPart->Placement.setValue(Base::Placement(Base::Vector3d(10, 0, 0), Base::Rotation()));
    auto* spoolBox = makeBox(_doc, "Spool_GearTipRelief", Base::Vector3d(0, 0, 0), 20, 20, 20);
    spoolPart->addObject(spoolBox);

    _assembly->addObject(casePart);
    _assembly->addObject(spoolPart);
    _doc->recompute();

    // Encoded mapped-element token between feature and FaceN (ELEMENT_MAP_PREFIX ';').
    const std::string tnpCase =
        "AssemblyCase.CaseSnapWindowRightPocket.;#a:1;:G0;XTR;:Hc94:8,F.Face1";
    const std::string tnpSpool =
        "AssemblySpool.Spool_GearTipRelief.;#b:2;:G0;XTR;:Hd12:8,F.Face1";

    EXPECT_EQ(
        Assembly::normalizeInterferenceSubName(_assembly, tnpCase),
        "AssemblyCase.CaseSnapWindowRightPocket.Face1"
    );
    EXPECT_EQ(
        Assembly::normalizeInterferenceSubName(_assembly, tnpSpool),
        "AssemblySpool.Spool_GearTipRelief.Face1"
    );

    // One SelectionEx root (= _assembly) with two encoded subelement paths.
    std::vector<Assembly::InterferenceSelectionHandle> handles {
        {_assembly, tnpCase},
        {_assembly, tnpSpool}
    };

    auto* host =
        Assembly::resolveInterferenceHostFromHandles(handles, unrelatedEdit);
    ASSERT_EQ(host, _assembly);
    EXPECT_NE(host, unrelatedEdit);

    const auto scope = Assembly::resolveInterferenceSelectionScope(host, handles);
    EXPECT_EQ(scope.mode, Assembly::InterferenceScanScopeMode::SelectedPair);
    EXPECT_EQ(scope.subelementHandleCount, 2);
    EXPECT_EQ(scope.distinctOccurrenceCount, 2);
    EXPECT_EQ(scope.first.occurrencePrefix, "AssemblyCase.");
    EXPECT_EQ(scope.second.occurrencePrefix, "AssemblySpool.");

    // Command-facing seam: SelectedPair ⇒ Check Selected Components active.
    EXPECT_EQ(scope.mode, Assembly::InterferenceScanScopeMode::SelectedPair);
}

TEST_F(InterferenceScanTest, hostPrecedenceExactPairAndFallbacks)
{
    auto* editA = _doc->addObject<Assembly::AssemblyObject>("EditAssemblyA");
    auto* rootB = _doc->addObject<Assembly::AssemblyObject>("ScanAssemblyB");
    auto* rootC = _doc->addObject<Assembly::AssemblyObject>("ScanAssemblyC");
    ASSERT_NE(editA, nullptr);
    ASSERT_NE(rootB, nullptr);
    ASSERT_NE(rootC, nullptr);

    auto* b1 = _doc->addObject<App::Part>("BComp1");
    auto* b1box = makeBox(_doc, "BBox1", Base::Vector3d(0, 0, 0), 10, 10, 10);
    b1->addObject(b1box);
    auto* b2 = _doc->addObject<App::Part>("BComp2");
    auto* b2box = makeBox(_doc, "BBox2", Base::Vector3d(20, 0, 0), 10, 10, 10);
    b2->addObject(b2box);
    rootB->addObject(b1);
    rootB->addObject(b2);

    auto* c1 = _doc->addObject<App::Part>("CComp1");
    auto* c1box = makeBox(_doc, "CBox1", Base::Vector3d(0, 0, 0), 10, 10, 10);
    c1->addObject(c1box);
    auto* c2 = _doc->addObject<App::Part>("CComp2");
    auto* c2box = makeBox(_doc, "CBox2", Base::Vector3d(20, 0, 0), 10, 10, 10);
    c2->addObject(c2box);
    rootC->addObject(c1);
    rootC->addObject(c2);
    _doc->recompute();

    const std::string pathB1 = "BComp1.BBox1.Face1";
    const std::string pathB2 = "BComp2.BBox2.Face1";
    const std::string pathB1Face2 = "BComp1.BBox1.Face2";
    const std::string pathC1 = "CComp1.CBox1.Face1";
    const std::string pathC2 = "CComp2.CBox2.Face1";
    const std::string tnpB1 = "BComp1.BBox1.;#a:1;:G0;XTR;:Hc94:8,F.Face1";
    const std::string tnpB2 = "BComp2.BBox2.;#b:2;:G0;XTR;:Hd12:8,F.Face1";
    const std::string badTnpB1 = "GhostA.;#x:1;:G0;XTR;:Hbad:8,F.Face1";
    const std::string badTnpB2 = "GhostB.;#y:2;:G0;XTR;:Hbad:8,F.Face2";

    using Assembly::InterferenceSelectionHandle;
    using Assembly::InterferenceScanScopeMode;

    auto expectSelectedPairActive = [](App::DocumentObject* host,
                                       const std::vector<InterferenceSelectionHandle>& handles) {
        ASSERT_NE(host, nullptr);
        const auto scope = Assembly::resolveInterferenceSelectionScope(host, handles);
        EXPECT_EQ(scope.mode, InterferenceScanScopeMode::SelectedPair);
        EXPECT_EQ(scope.subelementHandleCount, 2);
        EXPECT_EQ(scope.distinctOccurrenceCount, 2);
        EXPECT_FALSE(scope.first.occurrencePrefix.empty());
        EXPECT_FALSE(scope.second.occurrencePrefix.empty());
        EXPECT_NE(scope.first.occurrencePrefix, scope.second.occurrencePrefix);
    };

    auto expectKeepEdit = [&](const std::vector<InterferenceSelectionHandle>& handles,
                              int expectedGlobalSubCount,
                              int expectedDistinctOnHost = 0) {
        App::DocumentObject* host = nullptr;
        EXPECT_NO_THROW({
            host = Assembly::resolveInterferenceHostFromHandles(handles, editA);
        });
        EXPECT_EQ(host, editA) << "edit-mode assembly must remain the scan host";
        ASSERT_NE(host, nullptr);
        const auto scope = Assembly::resolveInterferenceSelectionScope(host, handles);
        EXPECT_EQ(scope.mode, InterferenceScanScopeMode::AllComponents);
        EXPECT_EQ(scope.subelementHandleCount, expectedGlobalSubCount);
        EXPECT_EQ(scope.distinctOccurrenceCount, expectedDistinctOnHost);
        EXPECT_TRUE(scope.first.occurrencePrefix.empty());
        EXPECT_TRUE(scope.second.occurrencePrefix.empty());
    };

    // Happy: exact two old-style paths under shared root B override edit A.
    {
        std::vector<InterferenceSelectionHandle> handles {{rootB, pathB1}, {rootB, pathB2}};
        auto* host = Assembly::resolveInterferenceHostFromHandles(handles, editA);
        EXPECT_EQ(host, rootB);
        expectSelectedPairActive(host, handles);
    }

    // Happy: exact two TNP paths under B override edit A.
    {
        std::vector<InterferenceSelectionHandle> handles {{rootB, tnpB1}, {rootB, tnpB2}};
        auto* host = Assembly::resolveInterferenceHostFromHandles(handles, editA);
        EXPECT_EQ(host, rootB);
        expectSelectedPairActive(host, handles);
    }

    // Happy: selection root equals edit root.
    {
        std::vector<InterferenceSelectionHandle> handles {{rootB, pathB1}, {rootB, pathB2}};
        auto* host = Assembly::resolveInterferenceHostFromHandles(handles, rootB);
        EXPECT_EQ(host, rootB);
        expectSelectedPairActive(host, handles);
    }

    // Happy: no edit assembly — one selected interference root hosts all-visible.
    {
        std::vector<InterferenceSelectionHandle> handles {{rootB, pathB1}};
        auto* host = Assembly::resolveInterferenceHostFromHandles(handles, nullptr);
        EXPECT_EQ(host, rootB);
        const auto scope = Assembly::resolveInterferenceSelectionScope(host, handles);
        EXPECT_EQ(scope.mode, InterferenceScanScopeMode::AllComponents);
        EXPECT_EQ(scope.subelementHandleCount, 1);
        EXPECT_EQ(scope.distinctOccurrenceCount, 1);
        // first/second are only filled for SelectedPair.
        EXPECT_TRUE(scope.first.occurrencePrefix.empty());
        EXPECT_TRUE(scope.second.occurrencePrefix.empty());
    }

    // Happy: no edit — whole-object App::Part/Assembly selection.
    {
        std::vector<InterferenceSelectionHandle> handles {{rootB, {}}};
        auto* host = Assembly::resolveInterferenceHostFromHandles(handles, nullptr);
        EXPECT_EQ(host, rootB);
        const auto scope = Assembly::resolveInterferenceSelectionScope(host, handles);
        EXPECT_EQ(scope.mode, InterferenceScanScopeMode::AllComponents);
        EXPECT_EQ(scope.subelementHandleCount, 1);
        EXPECT_EQ(scope.distinctOccurrenceCount, 0);
        EXPECT_TRUE(scope.first.occurrencePrefix.empty());
        EXPECT_TRUE(scope.second.occurrencePrefix.empty());
    }
    {
        auto* plain = _doc->addObject<App::Part>("PlainHostPart");
        std::vector<InterferenceSelectionHandle> handles {{plain, {}}};
        auto* host = Assembly::resolveInterferenceHostFromHandles(handles, nullptr);
        EXPECT_EQ(host, plain);
        const auto scope = Assembly::resolveInterferenceSelectionScope(host, handles);
        EXPECT_EQ(scope.mode, InterferenceScanScopeMode::AllComponents);
        EXPECT_EQ(scope.subelementHandleCount, 1);
        EXPECT_EQ(scope.distinctOccurrenceCount, 0);
    }

    // 1. One valid pick under B while A is in edit → host A, AllComponents.
    expectKeepEdit({{rootB, pathB1}}, /*expectedGlobalSubCount=*/1);

    // 2. Two faces from the same occurrence under B → not SelectedPair; host A.
    {
        std::vector<InterferenceSelectionHandle> handles {{rootB, pathB1}, {rootB, pathB1Face2}};
        expectKeepEdit(handles, 2);
        const auto scopeB = Assembly::resolveInterferenceSelectionScope(rootB, handles);
        EXPECT_NE(scopeB.mode, InterferenceScanScopeMode::SelectedPair);
        EXPECT_EQ(scopeB.subelementHandleCount, 2);
        EXPECT_EQ(scopeB.distinctOccurrenceCount, 1);
        EXPECT_TRUE(scopeB.second.occurrencePrefix.empty());
    }

    // 3. Three valid subelement picks under B → global count 3; host A.
    {
        std::vector<InterferenceSelectionHandle> handles {
            {rootB, pathB1},
            {rootB, pathB2},
            {rootB, pathB1Face2}
        };
        expectKeepEdit(handles, 3);
        const auto scopeB = Assembly::resolveInterferenceSelectionScope(rootB, handles);
        EXPECT_NE(scopeB.mode, InterferenceScanScopeMode::SelectedPair);
        EXPECT_EQ(scopeB.subelementHandleCount, 3);
        EXPECT_EQ(scopeB.distinctOccurrenceCount, 2);
    }

    // 4. Two valid under B plus an extra under C — local B pair must not hide the third.
    {
        std::vector<InterferenceSelectionHandle> handles {
            {rootB, pathB1},
            {rootB, pathB2},
            {rootC, pathC1}
        };
        expectKeepEdit(handles, 3);
        // Filtered to B alone would look like a pair; global cardinality must win.
        const auto scopeBOnly = Assembly::resolveInterferenceSelectionScope(
            rootB,
            {{rootB, pathB1}, {rootB, pathB2}}
        );
        EXPECT_EQ(scopeBOnly.mode, InterferenceScanScopeMode::SelectedPair);
        const auto scopeBFull = Assembly::resolveInterferenceSelectionScope(rootB, handles);
        EXPECT_NE(scopeBFull.mode, InterferenceScanScopeMode::SelectedPair);
        EXPECT_EQ(scopeBFull.subelementHandleCount, 3);
    }

    // 5. One pick under B and one under C — no shared selection root; host A.
    expectKeepEdit({{rootB, pathB1}, {rootC, pathC1}}, 2);

    // 6. One valid path and one unresolved path under B → pair fails; host A.
    {
        std::vector<InterferenceSelectionHandle> handles {
            {rootB, pathB1},
            {rootB, "NoSuch.Face1"}
        };
        expectKeepEdit(handles, 2);
        const auto scopeB = Assembly::resolveInterferenceSelectionScope(rootB, handles);
        EXPECT_NE(scopeB.mode, InterferenceScanScopeMode::SelectedPair);
        EXPECT_EQ(scopeB.subelementHandleCount, 2);
        EXPECT_EQ(scopeB.distinctOccurrenceCount, 1);
        EXPECT_TRUE(scopeB.second.occurrencePrefix.empty());
    }

    // 7. Two malformed TNP paths under B — no throw; no manufactured pair; host A.
    {
        std::vector<InterferenceSelectionHandle> handles {{rootB, badTnpB1}, {rootB, badTnpB2}};
        expectKeepEdit(handles, 2);
        const auto scopeB = Assembly::resolveInterferenceSelectionScope(rootB, handles);
        EXPECT_EQ(scopeB.mode, InterferenceScanScopeMode::AllComponents);
        EXPECT_EQ(scopeB.subelementHandleCount, 2);
        EXPECT_EQ(scopeB.distinctOccurrenceCount, 0);
        EXPECT_TRUE(scopeB.first.occurrencePrefix.empty());
        EXPECT_TRUE(scopeB.second.occurrencePrefix.empty());
    }

    // 9. Null and empty handles — none may qualify as an exact pair alone.
    {
        expectKeepEdit({{nullptr, pathB1}}, 0);
        expectKeepEdit({{rootB, {}}}, 1);
        std::vector<InterferenceSelectionHandle> mixed {
            {nullptr, pathB1},
            {rootB, {}},
            {rootB, pathB2}
        };
        // null skipped; empty + subelement = two endpoints under B, but host stays edit
        // because scope is resolved against edit A (no SelectedPair there).
        expectKeepEdit(mixed, 2);
        const auto scopeB = Assembly::resolveInterferenceSelectionScope(rootB, mixed);
        // empty whole-object of rootB does not resolve to a component under rootB.
        EXPECT_NE(scopeB.mode, InterferenceScanScopeMode::SelectedPair);
        EXPECT_EQ(scopeB.subelementHandleCount, 2);
    }
    {
        // Two null objects with non-empty subnames: global count 0 for host scope
        // (null objects are skipped), and host stays edit A.
        std::vector<InterferenceSelectionHandle> handles {
            {nullptr, "DetachedComp.Face1"},
            {nullptr, "OtherComp.Face1"}
        };
        expectKeepEdit(handles, 0);
    }

    // 10. Invalid candidate objects cannot override A.
    {
        auto* box = makeBox(_doc, "LoneBox", Base::Vector3d(0, 0, 0), 5, 5, 5);
        expectKeepEdit({{box, "Face1"}, {box, "Face2"}}, 2);
    }
    {
        // Cross-document root with an otherwise valid local pair.
        const std::string otherName =
            App::GetApplication().getUniqueDocumentName("asmInterferenceOtherHost");
        App::Document* otherDoc =
            App::GetApplication().newDocument(otherName.c_str(), "asmInterferenceOtherHostUser");
        auto* foreign = otherDoc->addObject<Assembly::AssemblyObject>("ForeignAssembly");
        auto* f1 = otherDoc->addObject<App::Part>("FComp1");
        auto* f1box = makeBox(otherDoc, "FBox1", Base::Vector3d(0, 0, 0), 10, 10, 10);
        f1->addObject(f1box);
        auto* f2 = otherDoc->addObject<App::Part>("FComp2");
        auto* f2box = makeBox(otherDoc, "FBox2", Base::Vector3d(20, 0, 0), 10, 10, 10);
        f2->addObject(f2box);
        foreign->addObject(f1);
        foreign->addObject(f2);
        otherDoc->recompute();

        std::vector<InterferenceSelectionHandle> handles {
            {foreign, "FComp1.FBox1.Face1"},
            {foreign, "FComp2.FBox2.Face1"}
        };
        const auto foreignScope =
            Assembly::resolveInterferenceSelectionScope(foreign, handles);
        EXPECT_EQ(foreignScope.mode, InterferenceScanScopeMode::SelectedPair);

        expectKeepEdit(handles, 2);
        App::GetApplication().closeDocument(otherName.c_str());
    }

    // 11. Ambiguous roots — two local pairs; reordering must not pick first root.
    {
        std::vector<InterferenceSelectionHandle> handles {
            {rootB, pathB1},
            {rootB, pathB2},
            {rootC, pathC1},
            {rootC, pathC2}
        };
        expectKeepEdit(handles, 4);
        std::vector<InterferenceSelectionHandle> reordered {
            {rootC, pathC2},
            {rootB, pathB2},
            {rootC, pathC1},
            {rootB, pathB1}
        };
        expectKeepEdit(reordered, 4);
        EXPECT_EQ(
            Assembly::resolveInterferenceHostFromHandles(handles, editA),
            Assembly::resolveInterferenceHostFromHandles(reordered, editA)
        );
    }

    // 12. No edit-mode assembly: malformed paths fail safely; legitimate root fallback.
    {
        std::vector<InterferenceSelectionHandle> handles {{rootB, badTnpB1}, {rootB, badTnpB2}};
        App::DocumentObject* host = nullptr;
        EXPECT_NO_THROW({
            host = Assembly::resolveInterferenceHostFromHandles(handles, nullptr);
        });
        EXPECT_EQ(host, rootB);
        const auto scope = Assembly::resolveInterferenceSelectionScope(host, handles);
        EXPECT_EQ(scope.mode, InterferenceScanScopeMode::AllComponents);
        EXPECT_EQ(scope.subelementHandleCount, 2);
        EXPECT_EQ(scope.distinctOccurrenceCount, 0);
        EXPECT_TRUE(scope.first.occurrencePrefix.empty());
        EXPECT_TRUE(scope.second.occurrencePrefix.empty());
    }
    {
        auto* box = makeBox(_doc, "NoEditLoneBox", Base::Vector3d(0, 0, 0), 5, 5, 5);
        std::vector<InterferenceSelectionHandle> handles {{box, "Face1"}, {box, "Face2"}};
        App::DocumentObject* host = nullptr;
        EXPECT_NO_THROW({
            host = Assembly::resolveInterferenceHostFromHandles(handles, nullptr);
        });
        EXPECT_EQ(host, nullptr);
    }
}

TEST_F(InterferenceScanTest, normalizeInterferenceSubNameBadPaths)
{
    auto* casePart = _doc->addObject<App::Part>("NormCase");
    auto* box = makeBox(_doc, "NormBox", Base::Vector3d(0, 0, 0), 10, 10, 10);
    casePart->addObject(box);
    _assembly->addObject(casePart);
    _doc->recompute();

    auto* editA = _doc->addObject<Assembly::AssemblyObject>("NormEditA");
    ASSERT_NE(editA, nullptr);

    auto expectDeterministicNormalize = [&](const std::string& input) {
        std::string out;
        EXPECT_NO_THROW({
            out = Assembly::normalizeInterferenceSubName(_assembly, input);
        }) << input;
        // Deterministic: same input always yields the same output.
        EXPECT_EQ(Assembly::normalizeInterferenceSubName(_assembly, input), out) << input;
        EXPECT_EQ(out.find('?'), std::string::npos) << "must not leak missing marker: " << out;
        return out;
    };

    EXPECT_EQ(expectDeterministicNormalize(""), "");
    EXPECT_EQ(
        expectDeterministicNormalize("NormCase.NormBox.Face1"),
        "NormCase.NormBox.Face1"
    );

    const std::string mapped = "NormCase.NormBox.;#a:1;:G0;XTR;:Hc94:8,F.Face1";
    EXPECT_EQ(expectDeterministicNormalize(mapped), "NormCase.NormBox.Face1");

    // 8. TNP variants — each deterministic and never throws.
    const std::string truncated = "NormCase.NormBox.;#a:1;:G0";
    const std::string missingFace = "NormCase.NormBox.;#a:1;:G0;XTR;:Hc94:8,F";
    const std::string residualHash = "NormCase.NormBox.;#.Face1";
    const std::string repeated =
        "NormCase.NormBox.;#a:1;:G0;XTR;:Hc94:8,F.;#b:2;:G0;XTR;:Hd:8,F.Face1";
    const std::string stale = "NormCase.NormBox.;#dead:1;:G0;XTR;:Hbad:8,F.Face99";
    const std::string oversized =
        std::string("NormCase.NormBox.;#") + std::string(400, 'x') + ".Face1";
    const std::string malformed = "NormCase.;#@@.Face1";

    EXPECT_NO_THROW((void)expectDeterministicNormalize(truncated));
    EXPECT_NO_THROW((void)expectDeterministicNormalize(missingFace));
    EXPECT_NO_THROW((void)expectDeterministicNormalize(residualHash));
    EXPECT_NO_THROW((void)expectDeterministicNormalize(repeated));
    {
        const std::string staleOut = expectDeterministicNormalize(stale);
        EXPECT_NE(staleOut.find("Face99"), std::string::npos);
    }
    EXPECT_NO_THROW((void)expectDeterministicNormalize(oversized));
    EXPECT_NO_THROW((void)expectDeterministicNormalize(malformed));

    std::string nullObjOut;
    EXPECT_NO_THROW({
        nullObjOut = Assembly::normalizeInterferenceSubName(nullptr, mapped);
    });
    EXPECT_EQ(nullObjOut.find('?'), std::string::npos);
    EXPECT_EQ(Assembly::normalizeInterferenceSubName(nullptr, mapped), nullObjOut);

    // Two unresolved / malformed TNP paths must not manufacture a selected pair.
    {
        std::vector<Assembly::InterferenceSelectionHandle> badPair {
            {_assembly, "GhostA.;#x.Face1"},
            {_assembly, "GhostB.;#y.Face2"}
        };
        App::DocumentObject* host = nullptr;
        EXPECT_NO_THROW({
            host = Assembly::resolveInterferenceHostFromHandles(badPair, editA);
        });
        EXPECT_EQ(host, editA);
        const auto scopeHost = Assembly::resolveInterferenceSelectionScope(host, badPair);
        EXPECT_EQ(scopeHost.mode, Assembly::InterferenceScanScopeMode::AllComponents);
        EXPECT_EQ(scopeHost.subelementHandleCount, 2);
        EXPECT_EQ(scopeHost.distinctOccurrenceCount, 0);
        EXPECT_TRUE(scopeHost.first.occurrencePrefix.empty());
        EXPECT_TRUE(scopeHost.second.occurrencePrefix.empty());

        const auto scopeAsm = Assembly::resolveInterferenceSelectionScope(_assembly, badPair);
        EXPECT_EQ(scopeAsm.mode, Assembly::InterferenceScanScopeMode::AllComponents);
        EXPECT_EQ(scopeAsm.subelementHandleCount, 2);
        EXPECT_EQ(scopeAsm.distinctOccurrenceCount, 0);
        EXPECT_TRUE(scopeAsm.first.occurrencePrefix.empty());
        EXPECT_TRUE(scopeAsm.second.occurrencePrefix.empty());
    }
}

TEST_F(InterferenceScanTest, wholeObjectTreeSelectionResolvesCommonHostAndSelectedPair)
{
    auto* casePart = _doc->addObject<App::Part>("AssemblyCase");
    auto* caseBox = makeBox(_doc, "CaseBox", Base::Vector3d(0, 0, 0), 20, 20, 20);
    casePart->addObject(caseBox);
    auto* spool = _doc->addObject<App::Part>("AssemblySpool");
    spool->Placement.setValue(Base::Placement(Base::Vector3d(10, 0, 0), Base::Rotation()));
    auto* spoolBox = makeBox(_doc, "SpoolBox", Base::Vector3d(0, 0, 0), 20, 20, 20);
    spool->addObject(spoolBox);
    auto* gear = _doc->addObject<App::Part>("GearMeshAssembly");
    auto* gearBox = makeBox(_doc, "GearBox", Base::Vector3d(100, 0, 0), 5, 5, 5);
    gear->addObject(gearBox);
    _assembly->addObject(casePart);
    _assembly->addObject(spool);
    _assembly->addObject(gear);
    _doc->recompute();

    std::vector<Assembly::InterferenceSelectionHandle> handles {
        {casePart, {}},
        {spool, {}}
    };
    auto* host = Assembly::resolveInterferenceHostFromHandles(handles, nullptr);
    ASSERT_EQ(host, _assembly);

    const auto scope = Assembly::resolveInterferenceSelectionScope(host, handles);
    EXPECT_EQ(scope.mode, Assembly::InterferenceScanScopeMode::SelectedPair);
    EXPECT_EQ(scope.subelementHandleCount, 2);
    EXPECT_EQ(scope.distinctOccurrenceCount, 2);
    EXPECT_EQ(scope.first.occurrencePrefix, "AssemblyCase.");
    EXPECT_EQ(scope.second.occurrencePrefix, "AssemblySpool.");

    auto request = Assembly::resolveInterferenceSelectedPairRequest(handles, nullptr);
    ASSERT_TRUE(request.valid());
    EXPECT_EQ(request.host, _assembly);

    // Mixed whole-object + face still SelectedPair.
    std::vector<Assembly::InterferenceSelectionHandle> mixed {
        {casePart, {}},
        {_assembly, "AssemblySpool.SpoolBox.Face1"}
    };
    request = Assembly::resolveInterferenceSelectedPairRequest(mixed, nullptr);
    ASSERT_TRUE(request.valid());
    EXPECT_EQ(request.host, _assembly);

    // Third whole-object disables SelectedPair / Check Selected.
    handles.push_back({gear, {}});
    request = Assembly::resolveInterferenceSelectedPairRequest(handles, nullptr);
    EXPECT_FALSE(request.valid());
}

TEST_F(InterferenceScanTest, selectedPairIncludesHiddenOccurrenceAndExcludesSibling)
{
    auto* casePart = _doc->addObject<App::Part>("AssemblyCase");
    auto* caseBox = makeBox(_doc, "CaseBox", Base::Vector3d(0, 0, 0), 20, 20, 20);
    casePart->addObject(caseBox);
    casePart->Visibility.setValue(false);

    auto* spool = _doc->addObject<App::Part>("AssemblySpool");
    spool->Placement.setValue(Base::Placement(Base::Vector3d(10, 0, 0), Base::Rotation()));
    auto* spoolBox = makeBox(_doc, "SpoolBox", Base::Vector3d(0, 0, 0), 20, 20, 20);
    spool->addObject(spoolBox);

    auto* gear = _doc->addObject<App::Part>("GearMeshAssembly");
    auto* gearBox = makeBox(_doc, "GearBox", Base::Vector3d(80, 0, 0), 10, 10, 10);
    gear->addObject(gearBox);

    _assembly->addObject(casePart);
    _assembly->addObject(spool);
    _assembly->addObject(gear);
    _doc->recompute();

    // All-components with Include hidden off still excludes Case.
    auto listed = Assembly::listInterferenceComponentOccurrences(_assembly, false);
    bool sawCase = false;
    bool sawSpool = false;
    bool sawGear = false;
    for (const auto& occ : listed) {
        sawCase = sawCase || occ.occurrencePrefix == "AssemblyCase.";
        sawSpool = sawSpool || occ.occurrencePrefix == "AssemblySpool.";
        sawGear = sawGear || occ.occurrencePrefix == "GearMeshAssembly.";
    }
    EXPECT_FALSE(sawCase);
    EXPECT_TRUE(sawSpool);
    EXPECT_TRUE(sawGear);

    auto snapHiddenOff = Assembly::prepareInterferenceComponentScanSnapshot(_assembly, false);
    for (const auto& leaf : snapHiddenOff.leaves) {
        EXPECT_EQ(leaf.occurrenceSubName.rfind("AssemblyCase.", 0), std::string::npos);
    }

    auto leavesA = Assembly::collectInterferenceLeavesUnderPrefix(
        _assembly,
        "AssemblyCase.",
        /*includeHidden=*/true
    );
    auto leavesB = Assembly::collectInterferenceLeavesUnderPrefix(
        _assembly,
        "AssemblySpool.",
        /*includeHidden=*/true
    );
    ASSERT_FALSE(leavesA.empty());
    ASSERT_FALSE(leavesB.empty());
    for (const auto& leaf : leavesA) {
        EXPECT_EQ(leaf.occurrenceSubName.rfind("AssemblyCase.", 0), 0u);
        EXPECT_EQ(leaf.occurrenceSubName.rfind("GearMeshAssembly.", 0), std::string::npos);
    }
    for (const auto& leaf : leavesB) {
        EXPECT_EQ(leaf.occurrenceSubName.rfind("AssemblySpool.", 0), 0u);
        EXPECT_EQ(leaf.occurrenceSubName.rfind("GearMeshAssembly.", 0), std::string::npos);
    }

    Assembly::InterferenceScanOptions options;
    auto result = Assembly::runInterferenceScanBetweenLeafSets(leavesA, leavesB, options);
    ASSERT_TRUE(result.complete);
    EXPECT_GE(result.counts.penetrations, 1);
    EXPECT_TRUE(casePart->Visibility.getValue() == false);

    // Whole-object selection request stays valid while Case is hidden.
    auto request = Assembly::resolveInterferenceSelectedPairRequest(
        {{casePart, {}}, {spool, {}}},
        nullptr
    );
    ASSERT_TRUE(request.valid());
    EXPECT_EQ(request.host, _assembly);
}

TEST_F(InterferenceScanTest, selectedPairAndAllVisibleAgreeOnTwoComponentPenetration)
{
    auto* casePart = _doc->addObject<App::Part>("AssemblyCase");
    auto* nested = _doc->addObject<App::Part>("NestedCase");
    auto* caseBox = makeBox(_doc, "CaseBox", Base::Vector3d(0, 0, 0), 20, 20, 20);
    nested->addObject(caseBox);
    casePart->addObject(nested);

    auto* spool = _doc->addObject<App::Part>("AssemblySpool");
    spool->Placement.setValue(Base::Placement(Base::Vector3d(10, 0, 0), Base::Rotation()));
    auto* spoolBox = makeBox(_doc, "SpoolBox", Base::Vector3d(0, 0, 0), 20, 20, 20);
    spool->addObject(spoolBox);

    _assembly->addObject(casePart);
    _assembly->addObject(spool);
    _doc->recompute();

    auto snap = Assembly::prepareInterferenceComponentScanSnapshot(_assembly, false);
    std::vector<Assembly::InterferenceLeaf> leavesA;
    std::vector<Assembly::InterferenceLeaf> leavesB;
    for (const auto& leaf : snap.leaves) {
        if (leaf.occurrenceSubName.rfind("AssemblyCase.", 0) == 0) {
            leavesA.push_back(leaf);
        }
        else if (leaf.occurrenceSubName.rfind("AssemblySpool.", 0) == 0) {
            leavesB.push_back(leaf);
        }
    }

    Assembly::InterferenceScanOptions options;
    auto selected = Assembly::runInterferenceScanBetweenLeafSets(leavesA, leavesB, options);
    auto allVisible = Assembly::runInterferenceScanAcrossComponents(snap, options);
    ASSERT_TRUE(selected.complete);
    ASSERT_TRUE(allVisible.complete);
    EXPECT_EQ(selected.counts.penetrations, allVisible.counts.penetrations);
    EXPECT_EQ(selected.counts.contacts, allVisible.counts.contacts);
    ASSERT_EQ(selected.pairs.size(), allVisible.pairs.size());
    ASSERT_FALSE(selected.pairs.empty());

    auto endpointKey = [](const Assembly::InterferenceScanResult& r,
                          const Assembly::InterferencePairResult& p) {
        auto a = r.leaves[p.leafIndexA].occurrenceSubName;
        auto b = r.leaves[p.leafIndexB].occurrenceSubName;
        return (a <= b) ? a + "|" + b : b + "|" + a;
    };
    EXPECT_EQ(endpointKey(selected, selected.pairs.front()), endpointKey(allVisible, allVisible.pairs.front()));
    EXPECT_EQ(selected.pairs.front().detection.kind, allVisible.pairs.front().detection.kind);
    EXPECT_NEAR(
        selected.pairs.front().detection.overlapVolume,
        allVisible.pairs.front().detection.overlapVolume,
        1e-6
    );
}

TEST_F(InterferenceScanTest, plainPartRootCollectsNestedAndLinkedGeometry)
{
    auto* root = _doc->addObject<App::Part>("PartRoot");
    auto* nested = _doc->addObject<App::Part>("NestedPart");
    nested->Placement.setValue(Base::Placement(Base::Vector3d(100, 0, 0), Base::Rotation()));
    auto* inner = makeBox(_doc, "InnerBox", Base::Vector3d(0, 0, 0), 10, 4, 5);
    nested->addObject(inner);

    auto* source = makeBox(_doc, "LinkedSource", Base::Vector3d(0, 0, 0), 8, 8, 8);
    auto* link = freecad_cast<App::Link*>(_doc->addObject("App::Link", "PartLink"));
    ASSERT_NE(link, nullptr);
    link->setLink(-1, source);
    link->LinkPlacement.setValue(Base::Placement(Base::Vector3d(0, 50, 0), Base::Rotation()));

    root->addObject(nested);
    root->addObject(link);
    _doc->recompute();

    EXPECT_TRUE(Assembly::isInterferenceRoot(root));
    EXPECT_FALSE(Assembly::isInterferenceRoot(source));

    auto leaves = Assembly::collectInterferenceLeaves(root, false);
    ASSERT_EQ(leaves.size(), 2u);

    bool sawNested = false;
    bool sawLink = false;
    for (const auto& leaf : leaves) {
        if (leaf.occurrenceSubName.find("InnerBox") != std::string::npos) {
            sawNested = true;
            EXPECT_NEAR(leaf.worldBoundBox.MinX, 100.0, 1e-6);
            EXPECT_NEAR(leaf.worldBoundBox.MaxX, 110.0, 1e-6);
        }
        if (leaf.occurrenceSubName.find("PartLink") != std::string::npos) {
            sawLink = true;
            EXPECT_NEAR(leaf.worldBoundBox.MinY, 50.0, 1e-6);
            EXPECT_NEAR(leaf.worldBoundBox.MaxY, 58.0, 1e-6);
            EXPECT_EQ(leaf.sourceId, std::string(_doc->getName()) + "#LinkedSource");
        }
    }
    EXPECT_TRUE(sawNested);
    EXPECT_TRUE(sawLink);
}

TEST_F(InterferenceScanTest, plainPartClearanceAndExclusionsUseDynamicProperties)
{
    auto* root = _doc->addObject<App::Part>("PartRoot");
    auto* a = makeBox(_doc, "BoxA", Base::Vector3d(0, 0, 0), 10, 10, 10);
    auto* b = makeBox(_doc, "BoxB", Base::Vector3d(5, 0, 0), 10, 10, 10);
    root->addObject(a);
    root->addObject(b);
    _doc->recompute();

    Assembly::setInterferenceClearance(root, 1.25);
    EXPECT_DOUBLE_EQ(Assembly::getInterferenceClearance(root), 1.25);
    ASSERT_NE(root->getPropertyByName("InterferenceClearance"), nullptr);

    Assembly::addInterferenceExclusion(root, a, b);
    EXPECT_TRUE(Assembly::hasInterferenceExclusion(root, a, b));
    ASSERT_NE(root->getPropertyByName("InterferenceExcludedSources"), nullptr);

    auto leaves = Assembly::collectInterferenceLeaves(root, false);
    ASSERT_EQ(leaves.size(), 2u);
    Assembly::InterferenceScanOptions options;
    options.clearance = Assembly::getInterferenceClearance(root);
    std::vector<std::pair<std::string, std::string>> excluded {
        canonicalIds(leaves[0].sourceId, leaves[1].sourceId)
    };
    auto result = Assembly::runInterferenceScan(leaves, options, excluded);
    ASSERT_TRUE(result.complete);
    EXPECT_EQ(result.counts.penetrations, 0);
    EXPECT_GE(result.counts.excludedViolations, 1);

    Assembly::removeInterferenceExclusion(root, a, b);
    EXPECT_FALSE(Assembly::hasInterferenceExclusion(root, a, b));
}

namespace
{

struct FaceGapSample
{
    std::string pathA;
    std::string pathB;
    double distance = 0.0;
};

std::vector<FaceGapSample> sampleFaceGaps(
    const Assembly::InterferenceLeaf& leafA,
    const Assembly::InterferenceLeaf& leafB,
    double maxDistance
)
{
    std::vector<FaceGapSample> samples;
    TopTools_IndexedMapOfShape mapA;
    TopTools_IndexedMapOfShape mapB;
    TopExp::MapShapes(leafA.worldShape, TopAbs_FACE, mapA);
    TopExp::MapShapes(leafB.worldShape, TopAbs_FACE, mapB);
    for (int i = 1; i <= mapA.Extent(); ++i) {
        for (int j = 1; j <= mapB.Extent(); ++j) {
            BRepExtrema_DistShapeShape dist(mapA(i), mapB(j));
            if (!dist.IsDone() || dist.NbSolution() < 1) {
                continue;
            }
            const double d = dist.Value();
            if (!std::isfinite(d) || d < 0.0 || d > maxDistance) {
                continue;
            }
            FaceGapSample sample;
            sample.pathA = leafA.occurrenceSubName + "Face" + std::to_string(i);
            sample.pathB = leafB.occurrenceSubName + "Face" + std::to_string(j);
            sample.distance = d;
            samples.push_back(std::move(sample));
        }
    }
    std::sort(samples.begin(), samples.end(), [](const FaceGapSample& a, const FaceGapSample& b) {
        return a.distance < b.distance;
    });
    return samples;
}

Assembly::InterferenceLeaf makeCompoundLeaf(
    const char* path,
    const char* sourceId,
    const std::vector<TopoDS_Shape>& solids
)
{
    Assembly::InterferenceLeaf leaf;
    leaf.displayPath = path;
    leaf.occurrenceSubName = path;
    leaf.sourceId = sourceId;
    BRep_Builder builder;
    TopoDS_Compound compound;
    builder.MakeCompound(compound);
    Base::BoundBox3d box;
    for (const auto& solid : solids) {
        builder.Add(compound, solid);
        Bnd_Box bnd;
        BRepBndLib::Add(solid, bnd);
        if (!bnd.IsVoid()) {
            double xmin = 0, ymin = 0, zmin = 0, xmax = 0, ymax = 0, zmax = 0;
            bnd.Get(xmin, ymin, zmin, xmax, ymax, zmax);
            box.Add(Base::Vector3d(xmin, ymin, zmin));
            box.Add(Base::Vector3d(xmax, ymax, zmax));
        }
    }
    leaf.worldShape = compound;
    leaf.worldBoundBox = box;
    leaf.shapeValid = true;
    leaf.visible = true;
    return leaf;
}

}  // namespace

TEST_F(InterferenceScanTest, lookupClearancePrefersExactPairThenMaxThenStarThenAssembly)
{
    Assembly::InterferenceClearanceRuleTable table;
    Assembly::InterferenceClearanceRule star;
    star.faceA = "*";
    star.clearanceMm = 0.05;
    star.spreadsheetRow = 2;
    star.comment = "default";
    table.rules.push_back(star);
    table.hasDefaultStar = true;
    table.defaultStarClearance = 0.05;

    Assembly::InterferenceClearanceRule faceA;
    faceA.faceA = "CompA.Face1";
    faceA.clearanceMm = 0.10;
    faceA.spreadsheetRow = 3;
    table.rules.push_back(faceA);

    Assembly::InterferenceClearanceRule faceB;
    faceB.faceA = "CompB.Face2";
    faceB.clearanceMm = 0.40;
    faceB.spreadsheetRow = 4;
    table.rules.push_back(faceB);

    Assembly::InterferenceClearanceRule exact;
    exact.faceA = "CompA.Face1";
    exact.faceB = "CompB.Face2";
    exact.clearanceMm = 0.25;
    exact.spreadsheetRow = 5;
    exact.comment = "pair override";
    table.rules.push_back(exact);

    auto exactLookup = Assembly::lookupInterferenceClearance(
        table,
        "CompA.Face1",
        "CompB.Face2",
        0.01
    );
    EXPECT_EQ(exactLookup.kind, Assembly::InterferenceClearanceRuleKind::ExactPair);
    EXPECT_NEAR(exactLookup.clearanceMm, 0.25, 1e-12);
    ASSERT_EQ(exactLookup.sourceRows.size(), 1u);
    EXPECT_EQ(exactLookup.sourceRows.front(), 5);

    table.rules.pop_back();
    auto maxLookup = Assembly::lookupInterferenceClearance(
        table,
        "CompA.Face1",
        "CompB.Face2",
        0.01
    );
    EXPECT_EQ(maxLookup.kind, Assembly::InterferenceClearanceRuleKind::MaxIndividual);
    EXPECT_NEAR(maxLookup.clearanceMm, 0.40, 1e-12);
    ASSERT_EQ(maxLookup.sourceRows.size(), 2u);
    EXPECT_EQ(maxLookup.sourceRows[0], 3);
    EXPECT_EQ(maxLookup.sourceRows[1], 4);

    auto starLookup = Assembly::lookupInterferenceClearance(
        table,
        "CompA.Face9",
        "CompB.Face9",
        0.01
    );
    EXPECT_EQ(starLookup.kind, Assembly::InterferenceClearanceRuleKind::DefaultStar);
    EXPECT_NEAR(starLookup.clearanceMm, 0.05, 1e-12);

    Assembly::InterferenceClearanceRuleTable empty;
    auto globalLookup = Assembly::lookupInterferenceClearance(
        empty,
        "CompA.Face1",
        "CompB.Face2",
        0.07
    );
    EXPECT_EQ(globalLookup.kind, Assembly::InterferenceClearanceRuleKind::AssemblyGlobal);
    EXPECT_NEAR(globalLookup.clearanceMm, 0.07, 1e-12);
}

TEST_F(InterferenceScanTest, conservativeMaxIncludesHostFallbackWhenNoStar)
{
    Assembly::InterferenceClearanceRuleTable table;
    Assembly::InterferenceClearanceRule small;
    small.faceA = "CompA.Face1";
    small.clearanceMm = 0.05;
    small.spreadsheetRow = 2;
    table.rules.push_back(small);
    table.maxEnabledClearance = 0.05;
    table.hasDefaultStar = false;

    EXPECT_NEAR(Assembly::conservativeMaxDesignClearance(table, 0.80), 0.80, 1e-12);

    table.hasDefaultStar = true;
    table.defaultStarClearance = 0.10;
    table.maxEnabledClearance = 0.10;
    EXPECT_NEAR(Assembly::conservativeMaxDesignClearance(table, 0.80), 0.10, 1e-12);
}

TEST_F(InterferenceScanTest, parseClearanceSheetAndInvalidToleranceDiagnostics)
{
    auto* sheet = _doc->addObject<Spreadsheet::Sheet>("ClearanceSheet");
    ASSERT_NE(sheet, nullptr);
    sheet->setCell(App::CellAddress(0, 0), "Enabled");
    sheet->setCell(App::CellAddress(0, 1), "Face");
    sheet->setCell(App::CellAddress(0, 2), "FaceB");
    sheet->setCell(App::CellAddress(0, 3), "Tolerance");
    sheet->setCell(App::CellAddress(0, 4), "Comment");

    sheet->setCell(App::CellAddress(1, 0), "true");
    sheet->setCell(App::CellAddress(1, 1), "*");
    sheet->setCell(App::CellAddress(1, 3), "0.05");
    sheet->setCell(App::CellAddress(1, 4), "default");

    sheet->setCell(App::CellAddress(2, 0), "true");
    sheet->setCell(App::CellAddress(2, 1), "CompA.Face1");
    sheet->setCell(App::CellAddress(2, 3), "not-a-number");

    sheet->setCell(App::CellAddress(3, 0), "false");
    sheet->setCell(App::CellAddress(3, 1), "CompA.Face2");
    sheet->setCell(App::CellAddress(3, 3), "9.0");

    _doc->recompute();
    _assembly->setInterferenceClearanceSheet(sheet);

    auto table = Assembly::snapshotInterferenceClearanceRules(_assembly);
    // Disabled rows are ignored entirely (no rule entry).
    ASSERT_EQ(table.rules.size(), 2u);
    EXPECT_TRUE(table.hasDefaultStar);
    EXPECT_NEAR(table.defaultStarClearance, 0.05, 1e-12);
    EXPECT_NEAR(table.maxEnabledClearance, 0.05, 1e-12);
    EXPECT_EQ(table.invalidRuleCount, 1);

    bool sawInvalid = false;
    bool sawDisabled = false;
    for (const auto& rule : table.rules) {
        if (rule.faceA.find("Face1") != std::string::npos) {
            EXPECT_FALSE(rule.valid);
            sawInvalid = true;
        }
        if (rule.faceA.find("Face2") != std::string::npos) {
            sawDisabled = true;
        }
    }
    EXPECT_TRUE(sawInvalid);
    EXPECT_FALSE(sawDisabled);
}

TEST_F(InterferenceScanTest, faceSpecificRulesClassifyTwoGapsIndependently)
{
    const TopoDS_Shape boxA = BRepPrimAPI_MakeBox(gp_Pnt(0, 0, 0), 10, 10, 10).Shape();
    const TopoDS_Shape shelfClose = BRepPrimAPI_MakeBox(gp_Pnt(10.15, 0, 0), 5, 5, 10).Shape();
    const TopoDS_Shape shelfFar = BRepPrimAPI_MakeBox(gp_Pnt(10.35, 5, 0), 5, 5, 10).Shape();

    Assembly::InterferenceLeaf leafA;
    leafA.displayPath = "CompA.";
    leafA.occurrenceSubName = "CompA.";
    leafA.sourceId = "srcA";
    leafA.worldShape = boxA;
    leafA.worldBoundBox = Base::BoundBox3d(0, 0, 0, 10, 10, 10);
    leafA.shapeValid = true;
    leafA.visible = true;

    auto leafB = makeCompoundLeaf("CompB.", "srcB", {shelfClose, shelfFar});

    auto gaps = sampleFaceGaps(leafA, leafB, 0.6);
    ASSERT_GE(gaps.size(), 2u);

    const FaceGapSample* passGap = nullptr;
    const FaceGapSample* failGap = nullptr;
    for (const auto& gap : gaps) {
        if (!passGap && gap.distance > 0.12 && gap.distance < 0.20) {
            passGap = &gap;
        }
        if (!failGap && gap.distance > 0.28 && gap.distance < 0.45) {
            failGap = &gap;
        }
    }
    ASSERT_NE(passGap, nullptr) << "expected ~0.15 mm face gap";
    ASSERT_NE(failGap, nullptr) << "expected ~0.35 mm face gap";
    ASSERT_NE(passGap->pathA + passGap->pathB, failGap->pathA + failGap->pathB);

    Assembly::InterferenceClearanceRuleTable rules;
    Assembly::InterferenceClearanceRule passRule;
    passRule.faceA = passGap->pathA;
    passRule.faceB = passGap->pathB;
    passRule.clearanceMm = 0.10;
    passRule.spreadsheetRow = 2;
    passRule.comment = "tight pass";
    rules.rules.push_back(passRule);

    Assembly::InterferenceClearanceRule failRule;
    failRule.faceA = failGap->pathA;
    failRule.faceB = failGap->pathB;
    failRule.clearanceMm = 0.50;
    failRule.spreadsheetRow = 3;
    failRule.comment = "loose fail";
    rules.rules.push_back(failRule);
    rules.maxEnabledClearance = 0.50;

    Assembly::InterferenceScanOptions options;
    options.clearance = 0.0;
    options.clearanceRules = rules;

    std::vector<Assembly::InterferenceLeaf> leaves {leafA, leafB};
    auto result = Assembly::runInterferenceScan(leaves, options);
    ASSERT_TRUE(result.complete);
    EXPECT_EQ(result.counts.penetrations, 0);
    ASSERT_EQ(result.pairs.size(), 1u);
    const auto& pairResult = result.pairs.front();
    ASSERT_LT(pairResult.governingFaceHitIndex, pairResult.faceHits.size());

    bool sawPassClear = false;
    bool sawFailViolation = false;
    for (const auto& hit : pairResult.faceHits) {
        const bool matchPass =
            (hit.facePathA == passGap->pathA && hit.facePathB == passGap->pathB)
            || (hit.facePathA == passGap->pathB && hit.facePathB == passGap->pathA);
        const bool matchFail =
            (hit.facePathA == failGap->pathA && hit.facePathB == failGap->pathB)
            || (hit.facePathA == failGap->pathB && hit.facePathB == failGap->pathA);
        if (matchPass) {
            EXPECT_EQ(hit.classification, Part::InterferenceKind::Clear);
            EXPECT_NEAR(hit.appliedClearance, 0.10, 1e-12);
            EXPECT_EQ(hit.ruleKind, Assembly::InterferenceClearanceRuleKind::ExactPair);
            ASSERT_FALSE(hit.sourceRows.empty());
            EXPECT_EQ(hit.sourceRows.front(), 2);
            sawPassClear = true;
        }
        if (matchFail) {
            EXPECT_EQ(hit.classification, Part::InterferenceKind::ClearanceViolation);
            EXPECT_NEAR(hit.appliedClearance, 0.50, 1e-12);
            EXPECT_EQ(hit.ruleKind, Assembly::InterferenceClearanceRuleKind::ExactPair);
            ASSERT_FALSE(hit.sourceRows.empty());
            EXPECT_EQ(hit.sourceRows.front(), 3);
            sawFailViolation = true;
        }
    }
    EXPECT_TRUE(sawPassClear);
    EXPECT_TRUE(sawFailViolation);
    EXPECT_GE(result.counts.clearFaceHits, 1);
    EXPECT_GE(result.counts.clearanceViolations, 1);

    const auto& governingHit = pairResult.faceHits[pairResult.governingFaceHitIndex];
    EXPECT_EQ(governingHit.facePathA, failGap->pathA);
    EXPECT_EQ(governingHit.facePathB, failGap->pathB);
    EXPECT_EQ(governingHit.classification, Part::InterferenceKind::ClearanceViolation);
    EXPECT_EQ(governingHit.ruleKind, Assembly::InterferenceClearanceRuleKind::ExactPair);
    ASSERT_EQ(governingHit.sourceRows.size(), 1u);
    EXPECT_EQ(governingHit.sourceRows[0], 3);
    ASSERT_EQ(governingHit.sourceComments.size(), 1u);
    EXPECT_EQ(governingHit.sourceComments[0], "loose fail");
    EXPECT_TRUE(governingHit.closestPointsValid);
    EXPECT_NEAR(pairResult.detection.minimumDistance, governingHit.distance, 1e-9);
    EXPECT_LT(gaps.front().distance, governingHit.distance)
        << "the governed exact-rule pair must not be the global closest face pair";
}

TEST_F(InterferenceScanTest, governingContactRetainsItsCommonShape)
{
    auto leafA = makeLeaf("ContactA.", "srcA", gp_Pnt(0, 0, 0));
    auto leafB = makeLeaf("ContactB.", "srcB", gp_Pnt(10, 0, 0));

    auto result = Assembly::runInterferenceScan(
        {leafA, leafB},
        Assembly::InterferenceScanOptions {}
    );
    ASSERT_TRUE(result.complete);
    ASSERT_EQ(result.pairs.size(), 1u);
    const auto& pair = result.pairs.front();
    EXPECT_NE(pair.detection.kind, Part::InterferenceKind::Penetration);
    ASSERT_LT(pair.governingFaceHitIndex, pair.faceHits.size());
    const auto& hit = pair.faceHits[pair.governingFaceHitIndex];
    EXPECT_EQ(hit.classification, Part::InterferenceKind::Contact);
    EXPECT_FALSE(hit.commonShape.IsNull());
    EXPECT_TRUE(hit.closestPointsValid);
}

TEST_F(InterferenceScanTest, hostFallbackBroadPhaseKeepsUnmatchedFacePair)
{
    std::vector<Assembly::InterferenceLeaf> leaves;
    leaves.push_back(makeLeaf("HostA.", "srcA", gp_Pnt(0, 0, 0)));
    leaves.push_back(makeLeaf("HostB.", "srcB", gp_Pnt(10.4, 0, 0)));

    Assembly::InterferenceClearanceRuleTable rules;
    Assembly::InterferenceClearanceRule small;
    small.faceA = "Unrelated.Face1";
    small.clearanceMm = 0.05;
    small.spreadsheetRow = 2;
    rules.rules.push_back(small);
    rules.maxEnabledClearance = 0.05;
    rules.hasDefaultStar = false;

    Assembly::InterferenceScanOptions options;
    options.clearance = 0.5;
    options.clearanceRules = rules;
    auto result = Assembly::runInterferenceScan(leaves, options);
    ASSERT_TRUE(result.complete);
    EXPECT_GE(result.counts.clearanceViolations, 1);
}

TEST_F(InterferenceScanTest, distantFacesArePrunedFromFaceEnumeration)
{
    const TopoDS_Shape boxA = BRepPrimAPI_MakeBox(gp_Pnt(0, 0, 0), 10, 10, 10).Shape();
    const TopoDS_Shape boxB = BRepPrimAPI_MakeBox(gp_Pnt(10.2, 0, 0), 10, 10, 10).Shape();
    Assembly::InterferenceLeaf leafA;
    leafA.displayPath = "PruneA.";
    leafA.occurrenceSubName = "PruneA.";
    leafA.sourceId = "srcA";
    leafA.worldShape = boxA;
    leafA.worldBoundBox = Base::BoundBox3d(0, 0, 0, 10, 10, 10);
    leafA.shapeValid = true;
    leafA.visible = true;
    Assembly::InterferenceLeaf leafB;
    leafB.displayPath = "PruneB.";
    leafB.occurrenceSubName = "PruneB.";
    leafB.sourceId = "srcB";
    leafB.worldShape = boxB;
    leafB.worldBoundBox = Base::BoundBox3d(10.2, 0, 0, 20.2, 10, 10);
    leafB.shapeValid = true;
    leafB.visible = true;

    Assembly::InterferenceClearanceRuleTable rules;
    Assembly::InterferenceClearanceRule star;
    star.faceA = "*";
    star.clearanceMm = 0.5;
    star.spreadsheetRow = 2;
    rules.rules.push_back(star);
    rules.hasDefaultStar = true;
    rules.defaultStarClearance = 0.5;
    rules.maxEnabledClearance = 0.5;

    Assembly::InterferenceScanOptions options;
    options.clearance = 0.0;
    options.clearanceRules = rules;
    auto result = Assembly::runInterferenceScan({leafA, leafB}, options);
    ASSERT_TRUE(result.complete);
    ASSERT_EQ(result.pairs.size(), 1u);
    EXPECT_LT(result.pairs.front().faceHits.size(), 36u);
    EXPECT_GE(result.pairs.front().faceHits.size(), 1u);
}

TEST_F(InterferenceScanTest, faceRulesKeepPenetrationAndBroadPhaseForLargeClearance)
{
    std::vector<Assembly::InterferenceLeaf> leaves;
    leaves.push_back(makeLeaf("PenA.", "srcA", gp_Pnt(0, 0, 0)));
    leaves.push_back(makeLeaf("PenB.", "srcB", gp_Pnt(5, 0, 0)));

    Assembly::InterferenceClearanceRuleTable rules;
    Assembly::InterferenceClearanceRule star;
    star.faceA = "*";
    star.clearanceMm = 100.0;
    star.spreadsheetRow = 2;
    rules.rules.push_back(star);
    rules.hasDefaultStar = true;
    rules.defaultStarClearance = 100.0;
    rules.maxEnabledClearance = 100.0;

    Assembly::InterferenceScanOptions options;
    options.clearance = 0.0;
    options.clearanceRules = rules;
    auto result = Assembly::runInterferenceScan(leaves, options);
    ASSERT_TRUE(result.complete);
    EXPECT_EQ(result.counts.penetrations, 1);
    ASSERT_EQ(result.pairs.size(), 1u);
    EXPECT_EQ(result.pairs.front().detection.kind, Part::InterferenceKind::Penetration);
    ASSERT_EQ(result.pairs.front().faceHits.size(), 1u);
    EXPECT_EQ(
        result.pairs.front().faceHits.front().classification,
        Part::InterferenceKind::Penetration
    );

    std::vector<Assembly::InterferenceLeaf> separated;
    separated.push_back(makeLeaf("SepA.", "srcA", gp_Pnt(0, 0, 0)));
    separated.push_back(makeLeaf("SepB.", "srcB", gp_Pnt(10.4, 0, 0)));
    Assembly::InterferenceClearanceRuleTable keepRules;
    Assembly::InterferenceClearanceRule keep;
    keep.faceA = "*";
    keep.clearanceMm = 0.5;
    keep.spreadsheetRow = 2;
    keepRules.rules.push_back(keep);
    keepRules.hasDefaultStar = true;
    keepRules.defaultStarClearance = 0.5;
    keepRules.maxEnabledClearance = 0.5;

    Assembly::InterferenceScanOptions keepOpts;
    keepOpts.clearance = 0.0;
    keepOpts.clearanceRules = keepRules;
    auto kept = Assembly::runInterferenceScan(separated, keepOpts);
    ASSERT_TRUE(kept.complete);
    EXPECT_GE(kept.counts.clearanceViolations, 1);
}

namespace
{

Assembly::InterferenceComponentScanSnapshot makeAcrossSnap(
    const Assembly::InterferenceLeaf& a,
    const Assembly::InterferenceLeaf& b
)
{
    Assembly::InterferenceComponentScanSnapshot snap;
    Assembly::InterferenceComponentOccurrence ca;
    ca.occurrencePrefix = a.occurrenceSubName;
    ca.displayPath = a.displayPath;
    Assembly::InterferenceComponentOccurrence cb;
    cb.occurrencePrefix = b.occurrenceSubName;
    cb.displayPath = b.displayPath;
    snap.components = {ca, cb};
    snap.leaves = {a, b};
    snap.componentIndexOfLeaf = {0, 1};
    return snap;
}

Spreadsheet::Sheet* makeClearanceSheet(App::Document* doc, const char* name)
{
    auto* sheet = doc->addObject<Spreadsheet::Sheet>(name);
    sheet->setCell(App::CellAddress(0, 0), "Enabled");
    sheet->setCell(App::CellAddress(0, 1), "Face");
    sheet->setCell(App::CellAddress(0, 2), "FaceB");
    sheet->setCell(App::CellAddress(0, 3), "Tolerance");
    sheet->setCell(App::CellAddress(0, 4), "Comment");
    return sheet;
}

}  // namespace

TEST_F(InterferenceScanTest, duplicateExactRulesKeepStrictestRowsAndComments)
{
    Assembly::InterferenceClearanceRuleTable table;
    Assembly::InterferenceClearanceRule a;
    a.faceA = "A.Face1";
    a.faceB = "B.Face1";
    a.clearanceMm = 0.5;
    a.spreadsheetRow = 2;
    a.comment = "first strict";
    table.rules.push_back(a);
    Assembly::InterferenceClearanceRule b = a;
    b.clearanceMm = 0.5;
    b.spreadsheetRow = 3;
    b.comment = "second strict";
    table.rules.push_back(b);
    Assembly::InterferenceClearanceRule weak = a;
    weak.clearanceMm = 0.1;
    weak.spreadsheetRow = 4;
    weak.comment = "weaker";
    table.rules.push_back(weak);

    auto lookup = Assembly::lookupInterferenceClearance(table, "A.Face1", "B.Face1", 0.0);
    EXPECT_EQ(lookup.kind, Assembly::InterferenceClearanceRuleKind::ExactPair);
    EXPECT_NEAR(lookup.clearanceMm, 0.5, 1e-12);
    ASSERT_EQ(lookup.sourceRows.size(), 2u);
    EXPECT_EQ(lookup.sourceRows[0], 2);
    EXPECT_EQ(lookup.sourceRows[1], 3);
    ASSERT_EQ(lookup.sourceComments.size(), 2u);
    EXPECT_EQ(lookup.sourceComments[0], "first strict");
    EXPECT_EQ(lookup.sourceComments[1], "second strict");
}

TEST_F(InterferenceScanTest, maxIndividualKeepsEmptyCommentsAlignedWithRows)
{
    Assembly::InterferenceClearanceRuleTable table;
    Assembly::InterferenceClearanceRule faceA;
    faceA.faceA = "A.Face1";
    faceA.clearanceMm = 0.2;
    faceA.spreadsheetRow = 2;
    faceA.comment = "";
    table.rules.push_back(faceA);
    Assembly::InterferenceClearanceRule faceB;
    faceB.faceA = "B.Face2";
    faceB.clearanceMm = 0.4;
    faceB.spreadsheetRow = 3;
    faceB.comment = "from B";
    table.rules.push_back(faceB);

    auto lookup = Assembly::lookupInterferenceClearance(table, "A.Face1", "B.Face2", 0.0);
    EXPECT_EQ(lookup.kind, Assembly::InterferenceClearanceRuleKind::MaxIndividual);
    EXPECT_NEAR(lookup.clearanceMm, 0.4, 1e-12);
    ASSERT_EQ(lookup.sourceRows.size(), 2u);
    ASSERT_EQ(lookup.sourceComments.size(), 2u);
    EXPECT_EQ(lookup.sourceComments[0], "");
    EXPECT_EQ(lookup.sourceComments[1], "from B");
}

TEST_F(InterferenceScanTest, lengthQuantityAndFormulaToleranceAreMillimetres)
{
    auto* sheet = makeClearanceSheet(_doc, "QtyClearance");
    sheet->setCell(App::CellAddress(1, 0), "true");
    sheet->setCell(App::CellAddress(1, 1), "*");
    sheet->setCell(App::CellAddress(1, 3), "0.5 mm");
    sheet->setCell(App::CellAddress(2, 0), "true");
    sheet->setCell(App::CellAddress(2, 1), "Alone.Face1");
    sheet->setCell(App::CellAddress(2, 3), "=1/2");
    _doc->recompute();

    // Host-less parse: structural FaceN only (Alone is unresolved under host).
    auto table = Assembly::parseInterferenceClearanceSheet(sheet, nullptr);
    ASSERT_GE(table.rules.size(), 1u);
    EXPECT_TRUE(table.hasDefaultStar);
    EXPECT_NEAR(table.defaultStarClearance, 0.5, 1e-12);

    bool sawFormula = false;
    for (const auto& rule : table.rules) {
        if (rule.faceA.find("Alone.Face1") != std::string::npos) {
            EXPECT_TRUE(rule.valid);
            EXPECT_NEAR(rule.clearanceMm, 0.5, 1e-9);
            sawFormula = true;
        }
    }
    EXPECT_TRUE(sawFormula);
}

TEST_F(InterferenceScanTest, genericSelectedAndAcrossComponentsAgreeForSamePair)
{
    auto leafA = makeLeaf("AgreeA.", "srcA", gp_Pnt(0, 0, 0));
    auto leafB = makeLeaf("AgreeB.", "srcB", gp_Pnt(10.4, 0, 0));

    Assembly::InterferenceClearanceRuleTable rules;
    Assembly::InterferenceClearanceRule star;
    star.faceA = "*";
    star.clearanceMm = 0.5;
    star.spreadsheetRow = 2;
    rules.rules.push_back(star);
    rules.hasDefaultStar = true;
    rules.defaultStarClearance = 0.5;
    rules.maxEnabledClearance = 0.5;

    Assembly::InterferenceScanOptions options;
    options.clearance = 0.0;
    options.clearanceRules = rules;

    auto generic = Assembly::runInterferenceScan({leafA, leafB}, options);
    auto selected = Assembly::runInterferenceScanBetweenLeafSets({leafA}, {leafB}, options);
    auto across = Assembly::runInterferenceScanAcrossComponents(makeAcrossSnap(leafA, leafB), options);

    ASSERT_TRUE(generic.complete);
    ASSERT_TRUE(selected.complete);
    ASSERT_TRUE(across.complete);
    EXPECT_EQ(generic.counts.clearanceViolations, selected.counts.clearanceViolations);
    EXPECT_EQ(generic.counts.clearanceViolations, across.counts.clearanceViolations);
    EXPECT_EQ(generic.counts.clearFaceHits, selected.counts.clearFaceHits);
    EXPECT_EQ(generic.counts.clearFaceHits, across.counts.clearFaceHits);
    EXPECT_EQ(generic.counts.penetrations, across.counts.penetrations);
    EXPECT_GE(generic.counts.clearanceViolations, 1);
}

TEST_F(InterferenceScanTest, acrossComponentsEmptyTableUsesAssemblyGlobalFaceCounts)
{
    auto leafA = makeLeaf("GlobA.", "srcA", gp_Pnt(0, 0, 0));
    auto leafB = makeLeaf("GlobB.", "srcB", gp_Pnt(10.4, 0, 0));

    Assembly::InterferenceScanOptions options;
    options.clearance = 0.5;
    // Empty rule table → AssemblyGlobal face evaluation.
    auto result =
        Assembly::runInterferenceScanAcrossComponents(makeAcrossSnap(leafA, leafB), options);
    ASSERT_TRUE(result.complete);
    EXPECT_GE(result.counts.clearanceViolations, 1);
    ASSERT_EQ(result.pairs.size(), 1u);
    EXPECT_FALSE(result.pairs.front().faceHits.empty());
    EXPECT_EQ(result.pairs.front().faceHits.front().ruleKind,
              Assembly::InterferenceClearanceRuleKind::AssemblyGlobal);
}

TEST_F(InterferenceScanTest, acrossComponentsStarReportsFaceClearanceViolations)
{
    auto leafA = makeLeaf("StarA.", "srcA", gp_Pnt(0, 0, 0));
    auto leafB = makeLeaf("StarB.", "srcB", gp_Pnt(10.4, 0, 0));

    Assembly::InterferenceClearanceRuleTable rules;
    Assembly::InterferenceClearanceRule star;
    star.faceA = "*";
    star.clearanceMm = 0.5;
    star.spreadsheetRow = 2;
    rules.rules.push_back(star);
    rules.hasDefaultStar = true;
    rules.defaultStarClearance = 0.5;
    rules.maxEnabledClearance = 0.5;

    Assembly::InterferenceScanOptions options;
    options.clearance = 0.0;
    options.clearanceRules = rules;
    auto result =
        Assembly::runInterferenceScanAcrossComponents(makeAcrossSnap(leafA, leafB), options);
    ASSERT_TRUE(result.complete);
    EXPECT_GE(result.counts.clearanceViolations, 1);
    EXPECT_EQ(result.counts.penetrations, 0);
}

TEST_F(InterferenceScanTest, acrossComponentsIndividualBeyondGlobalStillRetained)
{
    auto leafA = makeLeaf("KeepA.", "srcA", gp_Pnt(0, 0, 0));
    auto leafB = makeLeaf("KeepB.", "srcB", gp_Pnt(10.4, 0, 0));

    // Find a proximate face path on leafA for an individual rule.
    TopTools_IndexedMapOfShape mapA;
    TopExp::MapShapes(leafA.worldShape, TopAbs_FACE, mapA);
    ASSERT_GE(mapA.Extent(), 1);
    const std::string facePath = leafA.occurrenceSubName + "Face1";

    Assembly::InterferenceClearanceRuleTable rules;
    Assembly::InterferenceClearanceRule individual;
    individual.faceA = facePath;
    individual.clearanceMm = 0.5;
    individual.spreadsheetRow = 2;
    rules.rules.push_back(individual);
    rules.maxEnabledClearance = 0.5;
    rules.hasDefaultStar = false;

    Assembly::InterferenceScanOptions options;
    options.clearance = 0.0;  // host alone would prune 0.4 mm gap
    options.clearanceRules = rules;
    auto result =
        Assembly::runInterferenceScanAcrossComponents(makeAcrossSnap(leafA, leafB), options);
    ASSERT_TRUE(result.complete);
    EXPECT_GE(result.counts.clearanceViolations + result.counts.clearFaceHits, 1);
    EXPECT_FALSE(result.pairs.empty());
}

TEST_F(InterferenceScanTest, acrossComponentsRejectsIntraComponentPairs)
{
    auto leafA1 = makeLeaf("SameComp.A1.", "src1", gp_Pnt(0, 0, 0));
    auto leafA2 = makeLeaf("SameComp.A2.", "src2", gp_Pnt(5, 0, 0));
    auto leafB = makeLeaf("OtherComp.B.", "src3", gp_Pnt(50, 0, 0));

    Assembly::InterferenceComponentScanSnapshot snap;
    Assembly::InterferenceComponentOccurrence ca;
    ca.occurrencePrefix = "SameComp.";
    Assembly::InterferenceComponentOccurrence cb;
    cb.occurrencePrefix = "OtherComp.";
    snap.components = {ca, cb};
    snap.leaves = {leafA1, leafA2, leafB};
    snap.componentIndexOfLeaf = {0, 0, 1};

    Assembly::InterferenceScanOptions options;
    options.clearance = 1.0;
    auto result = Assembly::runInterferenceScanAcrossComponents(snap, options);
    ASSERT_TRUE(result.complete);
    for (const auto& pair : result.pairs) {
        const auto ia = snap.componentIndexOfLeaf[pair.leafIndexA];
        const auto ib = snap.componentIndexOfLeaf[pair.leafIndexB];
        EXPECT_NE(ia, ib);
    }
    EXPECT_EQ(result.counts.penetrations, 0);
}

TEST_F(InterferenceScanTest, penetrationHasOneRepresentativeFacePairWithoutMultiplyingCount)
{
    auto leafA = makeLeaf("PenOnlyA.", "srcA", gp_Pnt(0, 0, 0));
    auto leafB = makeLeaf("PenOnlyB.", "srcB", gp_Pnt(5, 0, 0));
    Assembly::InterferenceScanOptions options;
    options.clearance = 0.0;
    auto result = Assembly::runInterferenceScan({leafA, leafB}, options);
    ASSERT_TRUE(result.complete);
    EXPECT_EQ(result.counts.penetrations, 1);
    ASSERT_EQ(result.pairs.size(), 1u);
    const auto& pair = result.pairs.front();
    ASSERT_EQ(pair.faceHits.size(), 1u);
    EXPECT_EQ(pair.governingFaceHitIndex, 0u);
    const auto& hit = pair.faceHits.front();
    EXPECT_EQ(hit.classification, Part::InterferenceKind::Penetration);
    EXPECT_FALSE(hit.facePathA.empty());
    EXPECT_FALSE(hit.facePathB.empty());
    EXPECT_NE(hit.facePathA.find("PenOnlyA.Face"), std::string::npos);
    EXPECT_NE(hit.facePathB.find("PenOnlyB.Face"), std::string::npos);
    EXPECT_TRUE(hit.closestPointsValid);
    EXPECT_FALSE(hit.commonShape.IsNull());
    EXPECT_NE(hit.diagnostic.find("occurrence-level"), std::string::npos);
}

TEST_F(InterferenceScanTest, excludedFaceViolationsCountOncePerComponentPair)
{
    auto leafA = makeLeaf("ExA.", "srcA", gp_Pnt(0, 0, 0));
    auto leafB = makeLeaf("ExB.", "srcB", gp_Pnt(10.4, 0, 0));

    Assembly::InterferenceClearanceRuleTable rules;
    Assembly::InterferenceClearanceRule star;
    star.faceA = "*";
    star.clearanceMm = 0.5;
    star.spreadsheetRow = 2;
    rules.rules.push_back(star);
    rules.hasDefaultStar = true;
    rules.defaultStarClearance = 0.5;
    rules.maxEnabledClearance = 0.5;

    Assembly::InterferenceScanOptions options;
    options.clearance = 0.0;
    options.clearanceRules = rules;
    auto result = Assembly::runInterferenceScan(
        {leafA, leafB},
        options,
        {canonicalIds("srcA", "srcB")}
    );
    ASSERT_TRUE(result.complete);
    EXPECT_EQ(result.counts.excludedViolations, 1);
    EXPECT_EQ(result.counts.clearanceViolations, 0);
    ASSERT_EQ(result.pairs.size(), 1u);
    EXPECT_TRUE(result.pairs.front().excluded);
    EXPECT_GT(result.pairs.front().faceHits.size(), 1u);
}

TEST_F(InterferenceScanTest, excludedInvalidAndInconclusiveStayVisible)
{
    Assembly::InterferenceLeaf bad = makeLeaf("Bad.", "srcBad", gp_Pnt(0, 0, 0));
    bad.shapeValid = false;
    bad.diagnostic = "bad geom";
    auto ok = makeLeaf("Ok.", "srcOk", gp_Pnt(50, 0, 0));

    Assembly::InterferenceScanOptions options;
    auto result = Assembly::runInterferenceScan(
        {bad, ok},
        options,
        {canonicalIds("srcBad", "srcOk")}
    );
    EXPECT_GT(result.counts.invalidInputs, 0);
    EXPECT_EQ(result.counts.excludedViolations, 0);
    for (const auto& pair : result.pairs) {
        if (pair.detection.kind == Part::InterferenceKind::InvalidInput) {
            EXPECT_FALSE(pair.excluded);
        }
    }
}

TEST_F(InterferenceScanTest, faceCandidateCapIsIncompleteNotInvalidInput)
{
    auto leafA = makeLeaf("CapA.", "srcA", gp_Pnt(0, 0, 0));
    auto leafB = makeLeaf("CapB.", "srcB", gp_Pnt(10.2, 0, 0));

    Assembly::InterferenceClearanceRuleTable rules;
    Assembly::InterferenceClearanceRule star;
    star.faceA = "*";
    star.clearanceMm = 5.0;
    star.spreadsheetRow = 2;
    rules.rules.push_back(star);
    rules.hasDefaultStar = true;
    rules.defaultStarClearance = 5.0;
    rules.maxEnabledClearance = 5.0;

    Assembly::InterferenceScanOptions options;
    options.clearance = 0.0;
    options.clearanceRules = rules;
    options.maxFacePairCandidates = 2;

    auto result = Assembly::runInterferenceScan({leafA, leafB}, options);
    EXPECT_EQ(result.counts.invalidInputs, 0);
    EXPECT_FALSE(result.complete);
    bool sawCap = false;
    for (const auto& issue : result.componentIssues) {
        if (issue.kind == Assembly::InterferenceComponentIssue::Kind::FaceEnumerationCapped) {
            sawCap = true;
            EXPECT_EQ(issue.leafIndex, 0u);
            EXPECT_EQ(issue.leafIndexB, 1u);
        }
    }
    EXPECT_TRUE(sawCap);
    ASSERT_FALSE(result.pairs.empty());
    EXPECT_FALSE(result.pairs.front().faceEnumerationDiagnostic.empty());

    auto again = Assembly::runInterferenceScan({leafA, leafB}, options);
    ASSERT_EQ(result.pairs.front().faceHits.size(), again.pairs.front().faceHits.size());
    for (std::size_t i = 0; i < result.pairs.front().faceHits.size(); ++i) {
        EXPECT_EQ(result.pairs.front().faceHits[i].facePathA, again.pairs.front().faceHits[i].facePathA);
        EXPECT_EQ(result.pairs.front().faceHits[i].facePathB, again.pairs.front().faceHits[i].facePathB);
    }
}

TEST_F(InterferenceScanTest, progressIsMonotonicWithStableGlobalTotal)
{
    auto leafA = makeLeaf("ProgA.", "srcA", gp_Pnt(0, 0, 0));
    auto leafB = makeLeaf("ProgB.", "srcB", gp_Pnt(10.4, 0, 0));
    auto leafC = makeLeaf("ProgC.", "srcC", gp_Pnt(30, 0, 0));

    Assembly::InterferenceClearanceRuleTable rules;
    Assembly::InterferenceClearanceRule star;
    star.faceA = "*";
    star.clearanceMm = 0.5;
    star.spreadsheetRow = 2;
    rules.rules.push_back(star);
    rules.hasDefaultStar = true;
    rules.defaultStarClearance = 0.5;
    rules.maxEnabledClearance = 0.5;

    Assembly::InterferenceScanOptions options;
    options.clearance = 0.0;
    options.clearanceRules = rules;
    int lastCurrent = -1;
    int stableTotal = -1;
    options.progress = [&](int current, int total) {
        if (stableTotal < 0) {
            stableTotal = total;
        }
        EXPECT_EQ(total, stableTotal);
        EXPECT_LE(current, total);
        EXPECT_GE(current, lastCurrent);
        lastCurrent = current;
    };

    auto snap = makeAcrossSnap(leafA, leafB);
    snap.components.push_back({});
    snap.components.back().occurrencePrefix = leafC.occurrenceSubName;
    snap.leaves.push_back(leafC);
    snap.componentIndexOfLeaf.push_back(2);
    auto result = Assembly::runInterferenceScanAcrossComponents(snap, options);
    ASSERT_TRUE(result.complete);
    EXPECT_GT(stableTotal, 0);
}

TEST_F(InterferenceScanTest, cancelDuringFaceClassificationReturnsCancelled)
{
    auto leafA = makeLeaf("CanA.", "srcA", gp_Pnt(0, 0, 0));
    auto leafB = makeLeaf("CanB.", "srcB", gp_Pnt(10.4, 0, 0));

    Assembly::InterferenceClearanceRuleTable rules;
    Assembly::InterferenceClearanceRule star;
    star.faceA = "*";
    star.clearanceMm = 0.5;
    star.spreadsheetRow = 2;
    rules.rules.push_back(star);
    rules.hasDefaultStar = true;
    rules.defaultStarClearance = 0.5;
    rules.maxEnabledClearance = 0.5;

    std::atomic<bool> cancel {false};
    Assembly::InterferenceScanOptions options;
    options.clearance = 0.0;
    options.clearanceRules = rules;
    options.cancelFlag = &cancel;
    options.progress = [&](int current, int total) {
        if (current > 0 && total > 0) {
            cancel.store(true);
        }
    };
    auto result = Assembly::runInterferenceScan({leafA, leafB}, options);
    EXPECT_TRUE(result.cancelled);
    EXPECT_FALSE(result.complete);
}

TEST_F(InterferenceScanTest, spreadsheetParserBadPathsAndTolerances)
{
    auto* sheet = makeClearanceSheet(_doc, "BadRules");
    int row = 1;
    auto add = [&](const char* enabled, const char* face, const char* faceB, const char* tol) {
        sheet->setCell(App::CellAddress(row, 0), enabled);
        sheet->setCell(App::CellAddress(row, 1), face);
        if (faceB) {
            sheet->setCell(App::CellAddress(row, 2), faceB);
        }
        sheet->setCell(App::CellAddress(row, 3), tol);
        ++row;
    };

    add("false", "Garbage.Face99", nullptr, "0.5 garbage");  // disabled → ignored
    add("true", "", nullptr, "0.5");                         // empty Face with data
    add("true", "MissingThing.Face1", nullptr, "0.5");       // unresolved under host
    add("true", "Edge1", nullptr, "0.5");
    add("true", "Face0", nullptr, "0.5");
    add("true", "Box.Face-1", nullptr, "0.5");
    add("true", "*", "Other.Face1", "0.5");                  // illegal * + FaceB
    add("true", "*", nullptr, "0.5 garbage");
    add("true", "*", nullptr, "nan");
    add("true", "*", nullptr, "-1");
    add("true", "*", nullptr, "1 kg");
    add("true", "Bad.Face1", "AlsoBad.Face2", "0.5");        // multi-field bad once

    _doc->recompute();
    auto table = Assembly::parseInterferenceClearanceSheet(sheet, _assembly);
    EXPECT_EQ(table.invalidRuleCount, row - 2);  // all enabled rows after disabled
    for (const auto& rule : table.rules) {
        if (!rule.enabled) {
            FAIL() << "disabled rule should not be retained";
        }
        if (rule.faceA == "*" && rule.faceB.empty() && rule.valid) {
            FAIL() << "no valid * rule expected from garbage tolerances";
        }
    }

    // Typo must not silently apply 0.5 and clear a 0.4 gap under global 0.
    // Keep the face pair in the conservative probe so the fail-closed
    // classification/provenance path is exercised directly.
    table.maxEnabledClearance = 0.5;
    Assembly::InterferenceScanOptions options;
    options.clearance = 0.0;
    options.clearanceRules = table;
    auto leafA = makeLeaf("TypoA.", "srcA", gp_Pnt(0, 0, 0));
    auto leafB = makeLeaf("TypoB.", "srcB", gp_Pnt(10.4, 0, 0));
    auto result = Assembly::runInterferenceScan({leafA, leafB}, options);
    EXPECT_GE(result.counts.invalidRules, 1);
    for (const auto& pair : result.pairs) {
        for (const auto& hit : pair.faceHits) {
            if (hit.ruleKind == Assembly::InterferenceClearanceRuleKind::AssemblyGlobal) {
                EXPECT_NE(hit.classification, Part::InterferenceKind::Clear);
            }
        }
    }
    ASSERT_FALSE(result.pairs.empty());
    const auto& governedPair = result.pairs.front();
    ASSERT_LT(governedPair.governingFaceHitIndex, governedPair.faceHits.size());
    const auto& governedHit =
        governedPair.faceHits[governedPair.governingFaceHitIndex];
    EXPECT_EQ(governedHit.classification, Part::InterferenceKind::Inconclusive);
    EXPECT_EQ(
        governedHit.ruleKind,
        Assembly::InterferenceClearanceRuleKind::AssemblyGlobal
    );
    EXPECT_TRUE(governedHit.sourceRows.empty());
    EXPECT_NE(governedHit.diagnostic.find("invalid enabled rules"), std::string::npos);
}

TEST_F(InterferenceScanTest, missingAndDuplicateHeadersAreDiagnostic)
{
    auto* sheet = _doc->addObject<Spreadsheet::Sheet>("HdrSheet");
    sheet->setCell(App::CellAddress(0, 0), "Enabled");
    sheet->setCell(App::CellAddress(0, 1), "Comment");
    _doc->recompute();
    auto missing = Assembly::parseInterferenceClearanceSheet(sheet, nullptr);
    EXPECT_EQ(missing.invalidRuleCount, 1);
    EXPECT_TRUE(missing.rules.empty());
    ASSERT_FALSE(missing.diagnostics.empty());
    EXPECT_EQ(
        missing.diagnostics.front(),
        "Clearance spreadsheet requires Face and Tolerance header columns"
    );

    auto* sheet2 = _doc->addObject<Spreadsheet::Sheet>("DupHdr");
    sheet2->setCell(App::CellAddress(0, 0), "Face");
    sheet2->setCell(App::CellAddress(0, 1), "Face");
    sheet2->setCell(App::CellAddress(0, 2), "Tolerance");
    sheet2->setCell(App::CellAddress(1, 0), "*");
    sheet2->setCell(App::CellAddress(1, 2), "0.1");
    _doc->recompute();
    auto dup = Assembly::parseInterferenceClearanceSheet(sheet2, nullptr);
    EXPECT_GE(dup.invalidRuleCount, 1);
    bool sawDup = false;
    for (const auto& d : dup.diagnostics) {
        if (d.find("Duplicate") != std::string::npos) {
            sawDup = true;
        }
    }
    EXPECT_TRUE(sawDup);
}

TEST_F(InterferenceScanTest, acrossComponentsSurfacesInvalidRuleDiagnostics)
{
    auto* sheet = makeClearanceSheet(_doc, "AcrossBad");
    sheet->setCell(App::CellAddress(1, 0), "true");
    sheet->setCell(App::CellAddress(1, 1), "*");
    sheet->setCell(App::CellAddress(1, 3), "0.5 garbage");
    _doc->recompute();
    auto table = Assembly::parseInterferenceClearanceSheet(sheet, nullptr);
    EXPECT_GE(table.invalidRuleCount, 1);

    auto leafA = makeLeaf("AR.A.", "srcA", gp_Pnt(0, 0, 0));
    auto leafB = makeLeaf("AR.B.", "srcB", gp_Pnt(10.4, 0, 0));
    Assembly::InterferenceScanOptions options;
    options.clearance = 0.0;
    options.clearanceRules = table;
    auto result =
        Assembly::runInterferenceScanAcrossComponents(makeAcrossSnap(leafA, leafB), options);
    EXPECT_GE(result.counts.invalidRules, 1);
    bool sawRuleIssue = false;
    for (const auto& issue : result.componentIssues) {
        if (issue.kind == Assembly::InterferenceComponentIssue::Kind::InvalidRule) {
            sawRuleIssue = true;
        }
    }
    EXPECT_TRUE(sawRuleIssue);
}

namespace
{

Assembly::InterferenceFaceHit makeHit(
    Part::InterferenceKind kind,
    const char* pathA = "A.Face1",
    const char* pathB = "B.Face1"
)
{
    Assembly::InterferenceFaceHit hit;
    hit.facePathA = pathA;
    hit.facePathB = pathB;
    hit.classification = kind;
    hit.appliedClearance = 0.5;
    hit.distance = 0.1;
    return hit;
}

Part::InterferenceResult solidInconclusive(const char* diag = "solid inconclusive")
{
    Part::InterferenceResult solid;
    solid.kind = Part::InterferenceKind::Inconclusive;
    solid.diagnostic = diag;
    return solid;
}

void expectInconclusiveIncomplete(const Assembly::InterferenceScanResult& result)
{
    ASSERT_FALSE(result.pairs.empty());
    EXPECT_EQ(result.pairs.front().detection.kind, Part::InterferenceKind::Inconclusive);
    EXPECT_EQ(result.counts.inconclusivePairs, 1);
    EXPECT_FALSE(result.complete);
    EXPECT_FALSE(result.cancelled);
}

}  // namespace

TEST_F(InterferenceScanTest, finalizeSolidInconclusiveNeverDowngrades)
{
    Part::InterferenceResult solid = solidInconclusive("occt failed");

    auto none = Assembly::finalizeInterferencePairDetection(solid, {}, false, {});
    EXPECT_EQ(none.kind, Part::InterferenceKind::Inconclusive);
    EXPECT_EQ(none.diagnostic, "occt failed");

    std::vector<Assembly::InterferenceFaceHit> clears {makeHit(Part::InterferenceKind::Clear)};
    auto withClear = Assembly::finalizeInterferencePairDetection(solid, clears, false, {});
    EXPECT_EQ(withClear.kind, Part::InterferenceKind::Inconclusive);
    EXPECT_EQ(withClear.diagnostic, "occt failed");

    std::vector<Assembly::InterferenceFaceHit> contact {makeHit(Part::InterferenceKind::Contact)};
    auto withContact = Assembly::finalizeInterferencePairDetection(solid, contact, false, {});
    EXPECT_EQ(withContact.kind, Part::InterferenceKind::Inconclusive);

    std::vector<Assembly::InterferenceFaceHit> viol {
        makeHit(Part::InterferenceKind::ClearanceViolation)
    };
    auto withViol = Assembly::finalizeInterferencePairDetection(solid, viol, false, {});
    EXPECT_EQ(withViol.kind, Part::InterferenceKind::Inconclusive);

    // Non-inconclusive solid still aggregates faces.
    Part::InterferenceResult clearSolid;
    clearSolid.kind = Part::InterferenceKind::Clear;
    auto upgraded = Assembly::finalizeInterferencePairDetection(clearSolid, viol, false, {});
    EXPECT_EQ(upgraded.kind, Part::InterferenceKind::ClearanceViolation);

    Part::InterferenceResult penetration;
    penetration.kind = Part::InterferenceKind::Penetration;
    penetration.overlapVolume = 12.0;
    auto penetrationWithFaceHits =
        Assembly::finalizeInterferencePairDetection(penetration, clears, false, {});
    EXPECT_EQ(penetrationWithFaceHits.kind, Part::InterferenceKind::Penetration);
    EXPECT_DOUBLE_EQ(penetrationWithFaceHits.overlapVolume, 12.0);
}

TEST_F(InterferenceScanTest, exclusionAffectedCountUsesUnorderedComponentPairs)
{
    auto* sourceA = makeBox(
        _doc,
        "ImpactSourceA",
        Base::Vector3d(100, 0, 0),
        1,
        1,
        1
    );
    auto* sourceB = makeBox(
        _doc,
        "ImpactSourceB",
        Base::Vector3d(110, 0, 0),
        1,
        1,
        1
    );
    const std::string idA =
        std::string(_doc->getName()) + "#" + sourceA->getNameInDocument();
    const std::string idB =
        std::string(_doc->getName()) + "#" + sourceB->getNameInDocument();

    Assembly::InterferenceScanResult result;
    auto addLeaf = [&](const std::string& id, const char* occurrence) {
        Assembly::InterferenceLeaf leaf;
        leaf.sourceId = id;
        leaf.displayPath = occurrence;
        result.leaves.push_back(std::move(leaf));
        return result.leaves.size() - 1;
    };
    const auto a0 = addLeaf(idA, "A0");
    const auto b0 = addLeaf(idB, "B0");
    const auto b1 = addLeaf(idB, "B1");
    const auto a1 = addLeaf(idA, "A1");
    const auto a2 = addLeaf(idA, "A2");
    const auto b2 = addLeaf(idB, "B2");

    Assembly::InterferencePairResult faceOnly;
    faceOnly.leafIndexA = a0;
    faceOnly.leafIndexB = b0;
    faceOnly.detection.kind = Part::InterferenceKind::Clear;
    faceOnly.faceHits = {
        makeHit(Part::InterferenceKind::ClearanceViolation),
        makeHit(Part::InterferenceKind::Contact)
    };
    result.pairs.push_back(faceOnly);

    Assembly::InterferencePairResult reversed;
    reversed.leafIndexA = b1;
    reversed.leafIndexB = a1;
    reversed.detection.kind = Part::InterferenceKind::Contact;
    result.pairs.push_back(reversed);

    Assembly::InterferencePairResult penetration;
    penetration.leafIndexA = a2;
    penetration.leafIndexB = b2;
    penetration.detection.kind = Part::InterferenceKind::Penetration;
    result.pairs.push_back(penetration);

    EXPECT_EQ(
        Assembly::countInterferenceExclusionAffectedPairs(result, idA, idB),
        3u
    );
    EXPECT_EQ(
        Assembly::countInterferenceExclusionAffectedPairs(result, idB, idA),
        3u
    );
}

TEST_F(InterferenceScanTest, exclusionAffectedCountRejectsUnknownAndSuppressedResults)
{
    auto* sourceA = makeBox(
        _doc,
        "RejectImpactA",
        Base::Vector3d(100, 0, 0),
        1,
        1,
        1
    );
    auto* sourceB = makeBox(
        _doc,
        "RejectImpactB",
        Base::Vector3d(110, 0, 0),
        1,
        1,
        1
    );
    const std::string idA =
        std::string(_doc->getName()) + "#" + sourceA->getNameInDocument();
    const std::string idB =
        std::string(_doc->getName()) + "#" + sourceB->getNameInDocument();

    Assembly::InterferenceScanResult result;
    Assembly::InterferenceLeaf leafA;
    leafA.sourceId = idA;
    Assembly::InterferenceLeaf leafB;
    leafB.sourceId = idB;
    result.leaves = {leafA, leafB};

    Assembly::InterferencePairResult clear;
    clear.leafIndexA = 0;
    clear.leafIndexB = 1;
    clear.detection.kind = Part::InterferenceKind::Clear;
    clear.faceHits = {makeHit(Part::InterferenceKind::Clear)};
    result.pairs.push_back(clear);

    Assembly::InterferencePairResult invalid = clear;
    invalid.detection.kind = Part::InterferenceKind::InvalidInput;
    invalid.faceHits = {makeHit(Part::InterferenceKind::Contact)};
    result.pairs.push_back(invalid);

    Assembly::InterferencePairResult inconclusive = clear;
    inconclusive.detection.kind = Part::InterferenceKind::Inconclusive;
    inconclusive.faceHits = {makeHit(Part::InterferenceKind::ClearanceViolation)};
    result.pairs.push_back(inconclusive);

    Assembly::InterferencePairResult suppressed = clear;
    suppressed.faceHits = {makeHit(Part::InterferenceKind::Contact)};
    suppressed.faceHits.front().suppressedByExclusion = true;
    result.pairs.push_back(suppressed);

    Assembly::InterferencePairResult excluded = clear;
    excluded.detection.kind = Part::InterferenceKind::Penetration;
    excluded.faceHits.clear();
    excluded.excluded = true;
    result.pairs.push_back(excluded);

    EXPECT_EQ(
        Assembly::countInterferenceExclusionAffectedPairs(result, idA, idB),
        0u
    );
    EXPECT_EQ(
        Assembly::countInterferenceExclusionAffectedPairs(
            result,
            std::string(_doc->getName()) + "#Missing",
            idB
        ),
        0u
    );
    EXPECT_EQ(
        Assembly::countInterferenceExclusionAffectedPairs(result, "malformed", idB),
        0u
    );
}

TEST_F(InterferenceScanTest, solidInconclusivePathsAgreeAndPreserveFaceHits)
{
    auto leafA = makeLeaf("IncA.", "srcA", gp_Pnt(0, 0, 0));
    auto leafB = makeLeaf("IncB.", "srcB", gp_Pnt(10.4, 0, 0));

    Assembly::InterferenceSolidOverride override;
    override.result = solidInconclusive("forced solid inconclusive");

    Assembly::InterferenceClearanceRuleTable rules;
    Assembly::InterferenceClearanceRule star;
    star.faceA = "*";
    star.clearanceMm = 0.5;
    star.spreadsheetRow = 2;
    star.comment = "star";
    rules.rules.push_back(star);
    rules.hasDefaultStar = true;
    rules.defaultStarClearance = 0.5;
    rules.maxEnabledClearance = 0.5;

    // Solid Inconclusive with no proximate face candidates (clearance/probe 0 keeps
    // the pair via host fallback when no * — use small individual rule beyond gap).
    {
        Assembly::InterferenceClearanceRuleTable keep;
        Assembly::InterferenceClearanceRule individual;
        individual.faceA = "IncA.Face1";
        individual.clearanceMm = 0.5;
        individual.spreadsheetRow = 2;
        keep.rules.push_back(individual);
        keep.maxEnabledClearance = 0.5;
        Assembly::InterferenceScanOptions options;
        options.solidOverride = &override;
        options.clearance = 0.0;
        options.clearanceRules = keep;
        // Cap at 0 candidates? maxFacePairCandidates 0 means uncapped; use 1 then
        // we still get hits. For empty faceHits, rely on finalize unit test; here
        // ensure the pair is retained and stays Inconclusive even if faces exist.
        auto generic = Assembly::runInterferenceScan({leafA, leafB}, options);
        ASSERT_EQ(generic.pairs.size(), 1u);
        expectInconclusiveIncomplete(generic);
        EXPECT_EQ(generic.pairs.front().detection.diagnostic, "forced solid inconclusive");
    }

    Assembly::InterferenceScanOptions options;
    options.solidOverride = &override;
    options.clearance = 0.0;
    options.clearanceRules = rules;
    auto generic = Assembly::runInterferenceScan({leafA, leafB}, options);
    auto selected = Assembly::runInterferenceScanBetweenLeafSets({leafA}, {leafB}, options);
    auto across =
        Assembly::runInterferenceScanAcrossComponents(makeAcrossSnap(leafA, leafB), options);

    expectInconclusiveIncomplete(generic);
    expectInconclusiveIncomplete(selected);
    expectInconclusiveIncomplete(across);
    EXPECT_EQ(generic.counts.clearanceViolations, selected.counts.clearanceViolations);
    EXPECT_EQ(generic.counts.clearanceViolations, across.counts.clearanceViolations);
    EXPECT_EQ(generic.counts.contacts, across.counts.contacts);
    EXPECT_EQ(generic.counts.clearFaceHits, across.counts.clearFaceHits);
    EXPECT_EQ(generic.counts.inconclusivePairs, 1);
    EXPECT_EQ(across.counts.inconclusivePairs, 1);
    ASSERT_FALSE(generic.pairs.front().faceHits.empty());
    EXPECT_GE(generic.counts.clearanceViolations + generic.counts.clearFaceHits, 1);
    EXPECT_EQ(generic.pairs.front().detection.kind, Part::InterferenceKind::Inconclusive);
    EXPECT_EQ(generic.counts.penetrations, 0);
}

TEST_F(InterferenceScanTest, solidInconclusiveRetainsContactAndClearanceFaceCounts)
{
    auto leafA = makeLeaf("IncCntA.", "srcA", gp_Pnt(0, 0, 0));
    auto leafB = makeLeaf("IncCntB.", "srcB", gp_Pnt(10.0, 0, 0));  // contact-ish / tiny gap

    Assembly::InterferenceSolidOverride override;
    override.result = solidInconclusive("forced");

    Assembly::InterferenceScanOptions options;
    options.solidOverride = &override;
    options.clearance = 0.5;
    auto result = Assembly::runInterferenceScan({leafA, leafB}, options);
    expectInconclusiveIncomplete(result);
    ASSERT_EQ(result.pairs.size(), 1u);
    EXPECT_EQ(result.pairs.front().detection.kind, Part::InterferenceKind::Inconclusive);
    // Face hits retained; pair inconclusive counted once.
    EXPECT_EQ(result.counts.inconclusivePairs, 1);
    EXPECT_EQ(result.counts.penetrations, 0);
}

TEST_F(InterferenceScanTest, excludedMixedPairsPreserveUnknownVisibilityState)
{
    Assembly::InterferencePairResult onlyViol;
    onlyViol.detection.kind = Part::InterferenceKind::Contact;
    Assembly::InterferenceFaceHit c = makeHit(Part::InterferenceKind::Contact);
    onlyViol.faceHits = {c};

    Assembly::InterferenceScanResult acc;
    // Simulate record via scan: build leaves and use exclusion on a real scan.
    auto leafA = makeLeaf("MixA.", "srcA", gp_Pnt(0, 0, 0));
    auto leafB = makeLeaf("MixB.", "srcB", gp_Pnt(10.4, 0, 0));

    Assembly::InterferenceClearanceRuleTable rules;
    Assembly::InterferenceClearanceRule star;
    star.faceA = "*";
    star.clearanceMm = 0.5;
    star.spreadsheetRow = 2;
    rules.rules.push_back(star);
    rules.hasDefaultStar = true;
    rules.defaultStarClearance = 0.5;
    rules.maxEnabledClearance = 0.5;

    Assembly::InterferenceScanOptions options;
    options.clearance = 0.0;
    options.clearanceRules = rules;
    auto excluded = Assembly::runInterferenceScan(
        {leafA, leafB},
        options,
        {canonicalIds("srcA", "srcB")}
    );
    ASSERT_EQ(excluded.pairs.size(), 1u);
    EXPECT_TRUE(excluded.pairs.front().excluded);
    EXPECT_EQ(excluded.counts.excludedViolations, 1);
    EXPECT_EQ(excluded.counts.clearanceViolations, 0);
    bool anySuppressed = false;
    for (const auto& hit : excluded.pairs.front().faceHits) {
        if (hit.classification == Part::InterferenceKind::ClearanceViolation) {
            EXPECT_TRUE(hit.suppressedByExclusion);
            anySuppressed = true;
        }
        if (hit.classification == Part::InterferenceKind::InvalidInput
            || hit.classification == Part::InterferenceKind::Inconclusive) {
            EXPECT_FALSE(hit.suppressedByExclusion);
        }
    }
    EXPECT_TRUE(anySuppressed);

    // Invalid-only excluded sources must not become excludedViolations.
    Assembly::InterferenceLeaf bad = makeLeaf("BadMix.", "srcBad", gp_Pnt(0, 0, 0));
    bad.shapeValid = false;
    bad.diagnostic = "bad";
    auto ok = makeLeaf("OkMix.", "srcOk", gp_Pnt(50, 0, 0));
    auto invalidOnly = Assembly::runInterferenceScan(
        {bad, ok},
        Assembly::InterferenceScanOptions {},
        {canonicalIds("srcBad", "srcOk")}
    );
    EXPECT_GT(invalidOnly.counts.invalidInputs, 0);
    EXPECT_EQ(invalidOnly.counts.excludedViolations, 0);
}

TEST_F(InterferenceScanTest, facePathExistenceValidatedAgainstHostShape)
{
    auto* box = makeBox(_doc, "ExistingBox", Base::Vector3d(0, 0, 0), 10, 10, 10);
    _assembly->addObject(box);
    _doc->recompute();

    TopTools_IndexedMapOfShape faces;
    TopExp::MapShapes(box->Shape.getShape().getShape(), TopAbs_FACE, faces);
    ASSERT_GE(faces.Extent(), 1);
    const int lastFace = faces.Extent();
    const std::string face1 = std::string(box->getNameInDocument()) + ".Face1";
    const std::string faceLast =
        std::string(box->getNameInDocument()) + ".Face" + std::to_string(lastFace);
    const std::string faceOor =
        std::string(box->getNameInDocument()) + ".Face" + std::to_string(lastFace + 1);
    const std::string faceHuge = std::string(box->getNameInDocument()) + ".Face9999999999";

    auto* sheet = makeClearanceSheet(_doc, "FaceExists");
    sheet->setCell(App::CellAddress(1, 0), "true");
    sheet->setCell(App::CellAddress(1, 1), face1.c_str());
    sheet->setCell(App::CellAddress(1, 3), "0.5");
    sheet->setCell(App::CellAddress(2, 0), "true");
    sheet->setCell(App::CellAddress(2, 1), faceLast.c_str());
    sheet->setCell(App::CellAddress(2, 3), "0.5");
    sheet->setCell(App::CellAddress(3, 0), "true");
    sheet->setCell(App::CellAddress(3, 1), faceOor.c_str());
    sheet->setCell(App::CellAddress(3, 3), "0.5");
    sheet->setCell(App::CellAddress(4, 0), "true");
    sheet->setCell(App::CellAddress(4, 1), faceHuge.c_str());
    sheet->setCell(App::CellAddress(4, 3), "0.5");
    sheet->setCell(App::CellAddress(5, 0), "false");
    sheet->setCell(App::CellAddress(5, 1), faceOor.c_str());
    sheet->setCell(App::CellAddress(5, 3), "0.5");
    sheet->setCell(App::CellAddress(6, 0), "true");
    sheet->setCell(App::CellAddress(6, 1), face1.c_str());
    sheet->setCell(App::CellAddress(6, 2), faceOor.c_str());
    sheet->setCell(App::CellAddress(6, 3), "0.5");
    sheet->setCell(App::CellAddress(7, 0), "true");
    sheet->setCell(App::CellAddress(7, 1), "MissingNest.Face1");
    sheet->setCell(App::CellAddress(7, 3), "0.5");
    _doc->recompute();

    auto table = Assembly::parseInterferenceClearanceSheet(sheet, _assembly);
    int valid = 0;
    int invalid = 0;
    for (const auto& rule : table.rules) {
        if (rule.valid) {
            ++valid;
        }
        else {
            ++invalid;
        }
    }
    EXPECT_EQ(valid, 2);  // Face1 and last face
    EXPECT_EQ(invalid, 4);  // oor, huge, FaceB bad, missing nest (disabled ignored)
    EXPECT_EQ(table.invalidRuleCount, 4);

    // Regression: Face9999 must not silently complete Clear for a 0.4 mm gap.
    auto* other = makeBox(_doc, "OtherBox", Base::Vector3d(10.4, 0, 0), 10, 10, 10);
    _assembly->addObject(other);
    _doc->recompute();

    auto* badSheet = makeClearanceSheet(_doc, "Face9999");
    badSheet->setCell(App::CellAddress(1, 0), "true");
    badSheet->setCell(
        App::CellAddress(1, 1),
        (std::string(box->getNameInDocument()) + ".Face9999").c_str()
    );
    badSheet->setCell(App::CellAddress(1, 3), "0.5");
    _doc->recompute();
    auto badTable = Assembly::parseInterferenceClearanceSheet(badSheet, _assembly);
    EXPECT_EQ(badTable.invalidRuleCount, 1);

    auto leaves = Assembly::collectInterferenceLeaves(_assembly, false);
    ASSERT_GE(leaves.size(), 2u);
    Assembly::InterferenceScanOptions options;
    options.clearance = 0.0;
    options.clearanceRules = badTable;
    auto result = Assembly::runInterferenceScan(leaves, options);
    EXPECT_EQ(result.counts.invalidRules, 1);
    EXPECT_FALSE(result.complete);
    for (const auto& pair : result.pairs) {
        for (const auto& hit : pair.faceHits) {
            if (hit.ruleKind == Assembly::InterferenceClearanceRuleKind::AssemblyGlobal) {
                EXPECT_NE(hit.classification, Part::InterferenceKind::Clear);
            }
        }
    }
}

TEST_F(InterferenceScanTest, facePathExistenceOnLinksAndNestedParts)
{
    auto* source = makeBox(_doc, "LinkFaceSrc", Base::Vector3d(0, 0, 0), 10, 10, 10);
    auto* link = freecad_cast<App::Link*>(_doc->addObject("App::Link", "LinkFaceOcc"));
    ASSERT_NE(link, nullptr);
    link->setLink(-1, source);
    _assembly->addObject(link);

    auto* nest = _doc->addObject<App::Part>("NestPart");
    auto* inner = makeBox(_doc, "InnerFaceBox", Base::Vector3d(20, 0, 0), 10, 10, 10);
    nest->addObject(inner);
    _assembly->addObject(nest);
    _doc->recompute();

    auto* sheet = makeClearanceSheet(_doc, "LinkFaceSheet");
    const std::string linkFace1 = std::string(link->getNameInDocument()) + ".Face1";
    const std::string linkBad = std::string(link->getNameInDocument()) + ".Face9999";
    const std::string nestFace1 =
        std::string(nest->getNameInDocument()) + "." + inner->getNameInDocument() + ".Face1";
    const std::string nestBad =
        std::string(nest->getNameInDocument()) + "." + inner->getNameInDocument() + ".Face9999";
    sheet->setCell(App::CellAddress(1, 0), "true");
    sheet->setCell(App::CellAddress(1, 1), linkFace1.c_str());
    sheet->setCell(App::CellAddress(1, 3), "0.2");
    sheet->setCell(App::CellAddress(2, 0), "true");
    sheet->setCell(App::CellAddress(2, 1), linkBad.c_str());
    sheet->setCell(App::CellAddress(2, 3), "0.2");
    sheet->setCell(App::CellAddress(3, 0), "true");
    sheet->setCell(App::CellAddress(3, 1), nestFace1.c_str());
    sheet->setCell(App::CellAddress(3, 3), "0.2");
    sheet->setCell(App::CellAddress(4, 0), "true");
    sheet->setCell(App::CellAddress(4, 1), nestBad.c_str());
    sheet->setCell(App::CellAddress(4, 3), "0.2");
    _doc->recompute();

    auto table = Assembly::parseInterferenceClearanceSheet(sheet, _assembly);
    EXPECT_EQ(table.invalidRuleCount, 2);
    int valid = 0;
    for (const auto& rule : table.rules) {
        if (rule.valid) {
            ++valid;
        }
    }
    EXPECT_EQ(valid, 2);
}

TEST_F(InterferenceScanTest, facePathExistenceOnExpandedLinkArrayElements)
{
    auto* source = makeBox(_doc, "ArrFaceSrc", Base::Vector3d(0, 0, 0), 10, 10, 10);
    auto* link = freecad_cast<App::Link*>(_doc->addObject("App::Link", "ArrFace"));
    ASSERT_NE(link, nullptr);
    link->setLink(-1, source);
    link->ShowElement.setValue(true);
    link->ElementCount.setValue(2);
    _assembly->addObject(link);
    _doc->recompute();
    ASSERT_EQ(link->ElementList.getSize(), 2);
    auto* elt0 = freecad_cast<App::LinkElement*>(link->ElementList.getValues()[0]);
    ASSERT_NE(elt0, nullptr);
    const std::string prefix =
        std::string(link->getNameInDocument()) + "." + elt0->getNameInDocument() + ".";
    const std::string good = prefix + "Face1";
    const std::string bad = prefix + "Face9999";

    auto* sheet = makeClearanceSheet(_doc, "ArrFaceSheet");
    sheet->setCell(App::CellAddress(1, 0), "true");
    sheet->setCell(App::CellAddress(1, 1), good.c_str());
    sheet->setCell(App::CellAddress(1, 3), "0.2");
    sheet->setCell(App::CellAddress(2, 0), "true");
    sheet->setCell(App::CellAddress(2, 1), bad.c_str());
    sheet->setCell(App::CellAddress(2, 3), "0.2");
    _doc->recompute();
    auto table = Assembly::parseInterferenceClearanceSheet(sheet, _assembly);
    EXPECT_EQ(table.invalidRuleCount, 1);
    ASSERT_EQ(table.rules.size(), 2u);
    EXPECT_TRUE(table.rules[0].valid);
    EXPECT_FALSE(table.rules[1].valid);
}

namespace
{

Assembly::InterferenceScanResult runHostClearanceScan(
    Assembly::AssemblyObject* assembly,
    const Assembly::InterferenceClearanceRuleTable& rules,
    double hostClearance = 0.0
)
{
    Assembly::InterferenceScanOptions options;
    options.clearance = hostClearance;
    options.clearanceRules = rules;
    auto leaves = Assembly::collectInterferenceLeaves(assembly, false);
    return Assembly::runInterferenceScan(leaves, options);
}

const Assembly::InterferenceFaceHit* findFaceHitBetween(
    const Assembly::InterferenceScanResult& result,
    const std::string& faceA,
    const std::string& faceB
)
{
    for (const auto& pair : result.pairs) {
        for (const auto& hit : pair.faceHits) {
            if ((hit.facePathA == faceA && hit.facePathB == faceB)
                || (hit.facePathA == faceB && hit.facePathB == faceA)) {
                return &hit;
            }
        }
    }
    return nullptr;
}

App::Link* makeTwoElementGapArray(
    App::Document* doc,
    Assembly::AssemblyObject* assembly,
    const char* linkName,
    bool expanded,
    double gapMm
)
{
    const std::string srcName = std::string(linkName) + "Src";
    auto* source = makeBox(doc, srcName.c_str(), Base::Vector3d(0, 0, 0), 10, 10, 10);
    auto* link = freecad_cast<App::Link*>(doc->addObject("App::Link", linkName));
    link->setLink(-1, source);
    link->ShowElement.setValue(expanded);
    link->ElementCount.setValue(2);
    if (expanded) {
        doc->recompute();
        if (link->ElementList.getSize() != 2) {
            return link;
        }
        auto* elt0 = freecad_cast<App::LinkElement*>(link->ElementList.getValues()[0]);
        auto* elt1 = freecad_cast<App::LinkElement*>(link->ElementList.getValues()[1]);
        if (elt0 && elt1) {
            elt0->Placement.setValue(Base::Placement(Base::Vector3d(0, 0, 0), Base::Rotation()));
            elt1->Placement.setValue(
                Base::Placement(Base::Vector3d(10.0 + gapMm, 0, 0), Base::Rotation())
            );
        }
    }
    else {
        std::vector<Base::Placement> placements {
            Base::Placement(Base::Vector3d(0, 0, 0), Base::Rotation()),
            Base::Placement(Base::Vector3d(10.0 + gapMm, 0, 0), Base::Rotation())
        };
        link->PlacementList.setValues(placements);
    }
    assembly->addObject(link);
    doc->recompute();
    return link;
}

void assertLeavesSeparatedByGap(
    const Assembly::InterferenceLeaf& leafA,
    const Assembly::InterferenceLeaf& leafB,
    double expectedGapMm,
    double toleranceMm = 0.05
)
{
    ASSERT_TRUE(leafA.worldBoundBox.IsValid());
    ASSERT_TRUE(leafB.worldBoundBox.IsValid());
    const double sepX = leafB.worldBoundBox.MinX - leafA.worldBoundBox.MaxX;
    EXPECT_NEAR(sepX, expectedGapMm, toleranceMm)
        << leafA.occurrenceSubName << " vs " << leafB.occurrenceSubName;

    Assembly::InterferenceScanOptions options;
    options.clearance = 0.0;
    auto solidScan = Assembly::runInterferenceScan({leafA, leafB}, options);
    EXPECT_EQ(solidScan.counts.penetrations, 0);
    EXPECT_EQ(solidScan.counts.invalidInputs, 0);
}

void assertExpandedGapFixture(
    App::Link* link,
    Assembly::AssemblyObject* assembly,
    double gapMm
)
{
    ASSERT_NE(link, nullptr);
    ASSERT_TRUE(link->ShowElement.getValue());
    ASSERT_EQ(link->ElementList.getSize(), 2);
    auto leaves = Assembly::collectInterferenceLeaves(assembly, false);
    ASSERT_EQ(leaves.size(), 2u);
    assertLeavesSeparatedByGap(leaves[0], leaves[1], gapMm);
}

std::pair<std::string, std::string> closestFacePairBetween(
    const Assembly::InterferenceLeaf& leafA,
    const Assembly::InterferenceLeaf& leafB
)
{
    auto gaps = sampleFaceGaps(leafA, leafB, 2.0);
    EXPECT_FALSE(gaps.empty()) << "expected at least one face gap sample";
    if (gaps.empty()) {
        return {};
    }
    return {gaps.front().pathA, gaps.front().pathB};
}

}  // namespace

TEST_F(InterferenceScanTest, clearanceFacePathCanonicalizesExpandedArrayAliases)
{
    auto* link = makeTwoElementGapArray(_doc, _assembly, "CanonExp", true, 0.4);
    assertExpandedGapFixture(link, _assembly, 0.4);
    auto leaves = Assembly::collectInterferenceLeaves(_assembly, false);
    ASSERT_EQ(leaves.size(), 2u);
    const auto [closeA, closeB] = closestFacePairBetween(leaves[0], leaves[1]);
    ASSERT_FALSE(closeA.empty());
    ASSERT_FALSE(closeB.empty());

    auto* sheetCanon = makeClearanceSheet(_doc, "CanonExpCanon");
    sheetCanon->setCell(App::CellAddress(1, 0), "true");
    sheetCanon->setCell(App::CellAddress(1, 1), closeA.c_str());
    sheetCanon->setCell(App::CellAddress(1, 3), "0.5");
    sheetCanon->setCell(App::CellAddress(1, 4), "canon-row");
    _doc->recompute();
    auto tableCanon = Assembly::parseInterferenceClearanceSheet(sheetCanon, _assembly);
    ASSERT_EQ(tableCanon.rules.size(), 1u);
    EXPECT_TRUE(tableCanon.rules[0].valid);
    EXPECT_EQ(tableCanon.invalidRuleCount, 0);
    EXPECT_EQ(tableCanon.rules[0].faceA, closeA);

    auto lookupCanon =
        Assembly::lookupInterferenceClearance(tableCanon, closeA, closeB, 0.0);
    EXPECT_NEAR(lookupCanon.clearanceMm, 0.5, 1e-12);
    EXPECT_EQ(lookupCanon.kind, Assembly::InterferenceClearanceRuleKind::MaxIndividual);

    const std::string numericAlias =
        std::string(link->getNameInDocument()) + ".0."
        + closeA.substr(closeA.find_last_of('.') + 1);

    auto* sheetAlias = makeClearanceSheet(_doc, "CanonExpAlias");
    sheetAlias->setCell(App::CellAddress(1, 0), "true");
    sheetAlias->setCell(App::CellAddress(1, 1), numericAlias.c_str());
    sheetAlias->setCell(App::CellAddress(1, 3), "0.5");
    _doc->recompute();
    EXPECT_NO_THROW({
        auto tableAlias = Assembly::parseInterferenceClearanceSheet(sheetAlias, _assembly);
        ASSERT_EQ(tableAlias.rules.size(), 1u);
        EXPECT_TRUE(tableAlias.rules[0].valid);
        EXPECT_EQ(tableAlias.rules[0].faceA, closeA);
        EXPECT_EQ(tableAlias.invalidRuleCount, 0);
        auto lookupAlias =
            Assembly::lookupInterferenceClearance(tableAlias, closeA, closeB, 0.0);
        EXPECT_NEAR(lookupAlias.clearanceMm, 0.5, 1e-12);
        auto lookupCanonScan =
            Assembly::lookupInterferenceClearance(tableCanon, closeA, closeB, 0.0);
        EXPECT_NEAR(lookupCanonScan.clearanceMm, 0.5, 1e-12);
    });
}

TEST_F(InterferenceScanTest, clearanceFacePathAliasScanUsesCanonicalKeysOnBoxes)
{
    auto* boxA = makeBox(_doc, "CanonBoxA", Base::Vector3d(0, 0, 0), 10, 10, 10);
    auto* boxB = makeBox(_doc, "CanonBoxB", Base::Vector3d(10.4, 0, 0), 10, 10, 10);
    _assembly->addObject(boxA);
    _assembly->addObject(boxB);
    _doc->recompute();

    auto leaves = Assembly::collectInterferenceLeaves(_assembly, false);
    ASSERT_EQ(leaves.size(), 2u);
    const auto [closeA, closeB] = closestFacePairBetween(leaves[0], leaves[1]);
    ASSERT_FALSE(closeA.empty());

    auto* sheet = makeClearanceSheet(_doc, "CanonBoxAlias");
    sheet->setCell(App::CellAddress(1, 0), "true");
    sheet->setCell(App::CellAddress(1, 1), closeA.c_str());
    sheet->setCell(App::CellAddress(1, 3), "0.5");
    _doc->recompute();
    auto table = Assembly::parseInterferenceClearanceSheet(sheet, _assembly);
    ASSERT_TRUE(table.rules[0].valid);
    EXPECT_EQ(table.rules[0].faceA, closeA);

    auto scan = runHostClearanceScan(_assembly, table);
    EXPECT_TRUE(scan.complete);
    EXPECT_EQ(scan.counts.invalidRules, 0);
    EXPECT_GE(scan.counts.clearanceViolations, 1);
    const auto* hit = findFaceHitBetween(scan, closeA, closeB);
    ASSERT_NE(hit, nullptr);
    EXPECT_NEAR(hit->appliedClearance, 0.5, 1e-12);
}

TEST_F(InterferenceScanTest, clearanceFacePathCanonicalizesCollapsedArrayAliases)
{
    auto* link = makeTwoElementGapArray(_doc, _assembly, "CanonCol", false, 0.4);
    auto leaves = Assembly::collectInterferenceLeaves(_assembly, false);
    ASSERT_EQ(leaves.size(), 2u);
    const auto [closeA, closeB] = closestFacePairBetween(leaves[0], leaves[1]);
    ASSERT_FALSE(closeA.empty());

    auto* sheetCanon = makeClearanceSheet(_doc, "CanonColCanon");
    sheetCanon->setCell(App::CellAddress(1, 0), "true");
    sheetCanon->setCell(App::CellAddress(1, 1), closeA.c_str());
    sheetCanon->setCell(App::CellAddress(1, 3), "0.5");
    _doc->recompute();
    auto tableCanon = Assembly::parseInterferenceClearanceSheet(sheetCanon, _assembly);
    ASSERT_TRUE(tableCanon.rules[0].valid);
    EXPECT_EQ(tableCanon.rules[0].faceA, closeA);
    auto lookupCanon =
        Assembly::lookupInterferenceClearance(tableCanon, closeA, closeB, 0.0);
    EXPECT_NEAR(lookupCanon.clearanceMm, 0.5, 1e-12);

    const std::string elementAlias = std::string(link->getNameInDocument()) + ".0."
        + closeA.substr(closeA.find_last_of('.') + 1);

    auto* sheetAlias = makeClearanceSheet(_doc, "CanonColAlias");
    sheetAlias->setCell(App::CellAddress(1, 0), "true");
    sheetAlias->setCell(App::CellAddress(1, 1), elementAlias.c_str());
    sheetAlias->setCell(App::CellAddress(1, 3), "0.5");
    _doc->recompute();
    EXPECT_NO_THROW({
        auto tableAlias = Assembly::parseInterferenceClearanceSheet(sheetAlias, _assembly);
        ASSERT_EQ(tableAlias.rules.size(), 1u);
        EXPECT_TRUE(tableAlias.rules[0].valid);
        EXPECT_EQ(tableAlias.rules[0].faceA, closeA);
        EXPECT_EQ(tableAlias.invalidRuleCount, 0);
        auto scanAlias = runHostClearanceScan(_assembly, tableAlias);
        EXPECT_TRUE(scanAlias.complete);
        EXPECT_EQ(scanAlias.counts.invalidRules, 0);
        EXPECT_GE(scanAlias.counts.clearanceViolations, 1);
        const auto* hit = findFaceHitBetween(scanAlias, closeA, closeB);
        ASSERT_NE(hit, nullptr);
        EXPECT_NEAR(hit->appliedClearance, 0.5, 1e-12);
    });
}

TEST_F(InterferenceScanTest, clearanceFacePathRejectsBadArrayTokensAndPartCompoundAliases)
{
    auto* link = makeTwoElementGapArray(_doc, _assembly, "BadArr", true, 0.4);
    const std::string oor = std::string(link->getNameInDocument()) + ".99.Face1";
    const std::string huge =
        std::string(link->getNameInDocument()) + ".999999999999999999999.Face1";
    const std::string sibling =
        std::string(link->getNameInDocument()) + ".notAnElement.Face1";

    auto* sheet = makeClearanceSheet(_doc, "BadArrSheet");
    sheet->setCell(App::CellAddress(1, 0), "true");
    sheet->setCell(App::CellAddress(1, 1), oor.c_str());
    sheet->setCell(App::CellAddress(1, 3), "0.5");
    sheet->setCell(App::CellAddress(2, 0), "true");
    sheet->setCell(App::CellAddress(2, 1), huge.c_str());
    sheet->setCell(App::CellAddress(2, 3), "0.5");
    sheet->setCell(App::CellAddress(3, 0), "true");
    sheet->setCell(App::CellAddress(3, 1), sibling.c_str());
    sheet->setCell(App::CellAddress(3, 3), "0.5");
    _doc->recompute();

    EXPECT_NO_THROW({
        auto table = Assembly::parseInterferenceClearanceSheet(sheet, _assembly);
        EXPECT_EQ(table.invalidRuleCount, 3);
        for (const auto& rule : table.rules) {
            EXPECT_FALSE(rule.valid);
        }
        auto scan = runHostClearanceScan(_assembly, table);
        EXPECT_FALSE(scan.complete);
        EXPECT_EQ(scan.counts.invalidRules, 3);
    });

    auto* defBox = makeBox(_doc, "PartArrInner", Base::Vector3d(0, 0, 0), 10, 10, 10);
    auto* partLink = freecad_cast<App::Link*>(_doc->addObject("App::Link", "PartArr"));
    partLink->setLink(-1, defBox);
    partLink->ElementCount.setValue(1);
    partLink->ShowElement.setValue(false);
    partLink->PlacementList.setValues({Base::Placement()});
    _assembly->addObject(partLink);
    _doc->recompute();

    auto partLeaves = Assembly::collectInterferenceLeaves(_assembly, false);
    std::string compoundFace;
    for (const auto& leaf : partLeaves) {
        if (leaf.occurrenceSubName.rfind(partLink->getNameInDocument(), 0) == 0) {
            compoundFace = leaf.occurrenceSubName + "Face1";
            break;
        }
    }
    ASSERT_FALSE(compoundFace.empty());
    const std::string childAlias = std::string(partLink->getNameInDocument()) + ".0."
        + defBox->getNameInDocument() + ".Face1";

    auto* partSheet = makeClearanceSheet(_doc, "PartArrSheet");
    partSheet->setCell(App::CellAddress(1, 0), "true");
    partSheet->setCell(App::CellAddress(1, 1), compoundFace.c_str());
    partSheet->setCell(App::CellAddress(1, 3), "0.2");
    partSheet->setCell(App::CellAddress(2, 0), "true");
    partSheet->setCell(App::CellAddress(2, 1), childAlias.c_str());
    partSheet->setCell(App::CellAddress(2, 3), "0.2");
    _doc->recompute();

    auto partTable = Assembly::parseInterferenceClearanceSheet(partSheet, _assembly);
    ASSERT_EQ(partTable.rules.size(), 2u);
    EXPECT_TRUE(partTable.rules[0].valid);
    EXPECT_EQ(partTable.rules[0].faceA, compoundFace);
    EXPECT_FALSE(partTable.rules[1].valid);
    EXPECT_EQ(partTable.invalidRuleCount, 1);
}

TEST_F(InterferenceScanTest, clearanceFacePathNestedOrganizerAndDuplicateInstances)
{
    auto* folder = _doc->addObject<App::Part>("OrgFolder");
    auto* nest = _doc->addObject<App::Part>("OrgNest");
    auto* box = makeBox(_doc, "OrgBox", Base::Vector3d(0, 0, 0), 10, 10, 10);
    nest->addObject(box);
    folder->addObject(nest);
    _assembly->addObject(folder);
    _doc->recompute();

    const std::string hierarchical = std::string(folder->getNameInDocument()) + "."
        + nest->getNameInDocument() + "." + box->getNameInDocument() + ".Face1";
    const std::string flatAlias = std::string(box->getNameInDocument()) + ".Face1";

    auto* hierSheet = makeClearanceSheet(_doc, "OrgHier");
    hierSheet->setCell(App::CellAddress(1, 0), "true");
    hierSheet->setCell(App::CellAddress(1, 1), hierarchical.c_str());
    hierSheet->setCell(App::CellAddress(1, 3), "0.1");
    _doc->recompute();
    auto hierTable = Assembly::parseInterferenceClearanceSheet(hierSheet, _assembly);
    EXPECT_TRUE(hierTable.rules[0].valid);
    EXPECT_EQ(hierTable.rules[0].faceA, hierarchical);

    auto* flatSheet = makeClearanceSheet(_doc, "OrgFlat");
    flatSheet->setCell(App::CellAddress(1, 0), "true");
    flatSheet->setCell(App::CellAddress(1, 1), flatAlias.c_str());
    flatSheet->setCell(App::CellAddress(1, 3), "0.1");
    _doc->recompute();
    EXPECT_NO_THROW({
        auto flatTable = Assembly::parseInterferenceClearanceSheet(flatSheet, _assembly);
        EXPECT_TRUE(flatTable.rules[0].valid);
        EXPECT_EQ(flatTable.rules[0].faceA, hierarchical);
        EXPECT_EQ(flatTable.invalidRuleCount, 0);
    });

    auto* source = makeBox(_doc, "DupSrc", Base::Vector3d(0, 0, 0), 10, 10, 10);
    auto* instA = freecad_cast<App::Link*>(_doc->addObject("App::Link", "DupInstA"));
    auto* instB = freecad_cast<App::Link*>(_doc->addObject("App::Link", "DupInstB"));
    instA->setLink(-1, source);
    instB->setLink(-1, source);
    instB->Placement.setValue(Base::Placement(Base::Vector3d(30, 0, 0), Base::Rotation()));
    _assembly->addObject(instA);
    _assembly->addObject(instB);
    _doc->recompute();

    const std::string faceA =
        std::string(instA->getNameInDocument()) + ".Face1";
    const std::string faceB =
        std::string(instB->getNameInDocument()) + ".Face1";
    const std::string wrongAlias = std::string(source->getNameInDocument()) + ".Face1";

    auto* dupSheet = makeClearanceSheet(_doc, "DupSheet");
    dupSheet->setCell(App::CellAddress(1, 0), "true");
    dupSheet->setCell(App::CellAddress(1, 1), faceA.c_str());
    dupSheet->setCell(App::CellAddress(1, 2), faceB.c_str());
    dupSheet->setCell(App::CellAddress(1, 3), "0.5");
    dupSheet->setCell(App::CellAddress(1, 4), "dup-pair");
    dupSheet->setCell(App::CellAddress(2, 0), "true");
    dupSheet->setCell(App::CellAddress(2, 1), wrongAlias.c_str());
    dupSheet->setCell(App::CellAddress(2, 3), "0.5");
    _doc->recompute();

    auto dupTable = Assembly::parseInterferenceClearanceSheet(dupSheet, _assembly);
    ASSERT_GE(dupTable.rules.size(), 2u);
    EXPECT_TRUE(dupTable.rules[0].valid);
    EXPECT_EQ(dupTable.rules[0].faceA, faceA);
    EXPECT_EQ(dupTable.rules[0].faceB, faceB);
    EXPECT_FALSE(dupTable.rules[1].valid);
}

TEST_F(InterferenceScanTest, clearanceFacePathTnpAndFaceBCanonicalization)
{
    auto* nest = _doc->addObject<App::Part>("TnpNest");
    nest->Placement.setValue(
        Base::Placement(Base::Vector3d(100, 20, 5), Base::Rotation())
    );
    auto* box = makeBox(_doc, "TnpBox", Base::Vector3d(0, 0, 0), 10, 10, 10);
    nest->addObject(box);
    _assembly->addObject(nest);
    _doc->recompute();

    const std::string canonical =
        std::string(nest->getNameInDocument()) + "." + box->getNameInDocument() + ".Face1";
    const std::string mapped =
        std::string(nest->getNameInDocument()) + "." + box->getNameInDocument()
        + ".;#a:1;:G0;XTR;:Hc94:8,F.Face1";
    const std::string stale =
        std::string(nest->getNameInDocument()) + "." + box->getNameInDocument()
        + ".;#dead:1;:G0;XTR;:Hbad:8,F.Face99";

    auto* sheet = makeClearanceSheet(_doc, "TnpSheet");
    sheet->setCell(App::CellAddress(1, 0), "true");
    sheet->setCell(App::CellAddress(1, 1), mapped.c_str());
    sheet->setCell(App::CellAddress(1, 3), "0.3");
    sheet->setCell(App::CellAddress(2, 0), "true");
    sheet->setCell(App::CellAddress(2, 1), stale.c_str());
    sheet->setCell(App::CellAddress(2, 3), "0.3");
    _doc->recompute();

    EXPECT_NO_THROW({
        auto table = Assembly::parseInterferenceClearanceSheet(sheet, _assembly);
        ASSERT_EQ(table.rules.size(), 2u);
        EXPECT_TRUE(table.rules[0].valid);
        EXPECT_EQ(table.rules[0].faceA, canonical);
        EXPECT_FALSE(table.rules[1].valid);
        EXPECT_EQ(table.invalidRuleCount, 1);
    });

    auto* other = makeBox(_doc, "TnpOther", Base::Vector3d(110.4, 20, 5), 10, 10, 10);
    _assembly->addObject(other);
    _doc->recompute();

    auto leaves = Assembly::collectInterferenceLeaves(_assembly, false);
    ASSERT_GE(leaves.size(), 2u);
    const Assembly::InterferenceLeaf* nestLeaf = nullptr;
    const Assembly::InterferenceLeaf* otherLeaf = nullptr;
    for (const auto& leaf : leaves) {
        if (leaf.occurrenceSubName.find(nest->getNameInDocument()) != std::string::npos) {
            nestLeaf = &leaf;
        }
        if (leaf.occurrenceSubName.find(other->getNameInDocument()) != std::string::npos) {
            otherLeaf = &leaf;
        }
    }
    ASSERT_NE(nestLeaf, nullptr);
    ASSERT_NE(otherLeaf, nullptr);
    const auto [closeA, closeB] = closestFacePairBetween(*nestLeaf, *otherLeaf);
    ASSERT_FALSE(closeA.empty());

    const std::string faceElem = closeA.substr(closeA.find_last_of('.') + 1);
    const std::string mappedForPair =
        std::string(nest->getNameInDocument()) + "." + box->getNameInDocument()
        + ".;#a:1;:G0;XTR;:Hc94:8,F." + faceElem;
    const std::string otherFace = closeB;
    auto* pairSheet = makeClearanceSheet(_doc, "TnpPair");
    pairSheet->setCell(App::CellAddress(1, 0), "true");
    pairSheet->setCell(App::CellAddress(1, 1), mappedForPair.c_str());
    pairSheet->setCell(App::CellAddress(1, 2), otherFace.c_str());
    pairSheet->setCell(App::CellAddress(1, 3), "0.5");
    pairSheet->setCell(App::CellAddress(1, 4), "tnp-pair");
    _doc->recompute();

    auto pairTable = Assembly::parseInterferenceClearanceSheet(pairSheet, _assembly);
    ASSERT_EQ(pairTable.rules.size(), 1u);
    EXPECT_TRUE(pairTable.rules[0].valid);
    EXPECT_EQ(pairTable.rules[0].faceA, closeA);
    EXPECT_EQ(pairTable.rules[0].faceB, otherFace);

    auto scan = runHostClearanceScan(_assembly, pairTable);
    EXPECT_TRUE(scan.complete);
    EXPECT_EQ(scan.counts.invalidRules, 0);
    EXPECT_GE(scan.counts.clearanceViolations, 1);
    const auto* hit = findFaceHitBetween(scan, closeA, closeB);
    ASSERT_NE(hit, nullptr);
    EXPECT_EQ(hit->ruleKind, Assembly::InterferenceClearanceRuleKind::ExactPair);
    EXPECT_NEAR(hit->appliedClearance, 0.5, 1e-12);
    ASSERT_FALSE(hit->sourceRows.empty());
    EXPECT_EQ(hit->sourceRows[0], 2);
    EXPECT_EQ(hit->sourceComments[0], "tnp-pair");
    ASSERT_EQ(scan.pairs.size(), 1u);
    const auto& pair = scan.pairs.front();
    ASSERT_LT(pair.governingFaceHitIndex, pair.faceHits.size());
    EXPECT_EQ(&pair.faceHits[pair.governingFaceHitIndex], hit);
    EXPECT_TRUE(hit->closestPointsValid);
    EXPECT_GT(hit->pointOnFirst.x, 99.0);
    EXPECT_GT(hit->pointOnSecond.x, 99.0);
    EXPECT_GT(hit->pointOnFirst.y, 19.0);
    EXPECT_GT(hit->pointOnSecond.y, 19.0);
}

TEST_F(InterferenceScanTest, clearanceFacePathAliasNeverCompletesClearExpanded)
{
    auto* link = makeTwoElementGapArray(_doc, _assembly, "FalseClearExp", true, 0.4);
    assertExpandedGapFixture(link, _assembly, 0.4);
    auto leaves = Assembly::collectInterferenceLeaves(_assembly, false);
    ASSERT_EQ(leaves.size(), 2u);
    const auto [closeA, closeB] = closestFacePairBetween(leaves[0], leaves[1]);
    ASSERT_FALSE(closeA.empty());
    ASSERT_FALSE(closeB.empty());
    const std::string alias = std::string(link->getNameInDocument()) + ".0."
        + closeA.substr(closeA.find_last_of('.') + 1);
    auto* elt0 = freecad_cast<App::LinkElement*>(link->ElementList.getValues()[0]);
    ASSERT_NE(elt0, nullptr);
    const std::string canonicalFaceA = std::string(link->getNameInDocument()) + "."
        + elt0->getNameInDocument() + "." + closeA.substr(closeA.find_last_of('.') + 1);

    auto runAliasScan = [&](const std::string& faceInput, const char* comment) {
        auto* sheet = makeClearanceSheet(_doc, comment);
        sheet->setCell(App::CellAddress(1, 0), "true");
        sheet->setCell(App::CellAddress(1, 1), faceInput.c_str());
        sheet->setCell(App::CellAddress(1, 3), "0.5");
        sheet->setCell(App::CellAddress(1, 4), comment);
        _doc->recompute();
        auto table = Assembly::parseInterferenceClearanceSheet(sheet, _assembly);
        ASSERT_EQ(table.rules.size(), 1u) << comment;
        EXPECT_TRUE(table.rules[0].valid) << comment;
        EXPECT_EQ(table.rules[0].faceA, closeA) << comment;
        EXPECT_EQ(table.invalidRuleCount, 0) << comment;
        EXPECT_EQ(table.rules[0].comment, comment);
        auto lookup =
            Assembly::lookupInterferenceClearance(table, closeA, closeB, 0.0);
        EXPECT_NEAR(lookup.clearanceMm, 0.5, 1e-12) << comment;
        EXPECT_NE(lookup.kind, Assembly::InterferenceClearanceRuleKind::AssemblyGlobal) << comment;
        auto scan = runHostClearanceScan(_assembly, table, 0.0);
        EXPECT_TRUE(scan.complete) << comment;
        EXPECT_EQ(scan.counts.invalidRules, 0) << comment;
        EXPECT_EQ(scan.counts.penetrations, 0) << comment;
        EXPECT_GE(scan.counts.clearanceViolations, 1) << comment;
        const auto* hit = findFaceHitBetween(scan, closeA, closeB);
        ASSERT_NE(hit, nullptr) << comment;
        EXPECT_NEAR(hit->appliedClearance, 0.5, 1e-12) << comment;
        EXPECT_NE(hit->ruleKind, Assembly::InterferenceClearanceRuleKind::AssemblyGlobal) << comment;
        ASSERT_FALSE(hit->sourceRows.empty());
        EXPECT_EQ(hit->sourceComments[0], comment);
    };

    runAliasScan(alias, "alias-exp");
    runAliasScan(canonicalFaceA, "canon-exp");

    auto* pairSheet = makeClearanceSheet(_doc, "FCEpair");
    pairSheet->setCell(App::CellAddress(1, 0), "true");
    pairSheet->setCell(App::CellAddress(1, 1), alias.c_str());
    pairSheet->setCell(App::CellAddress(1, 2), closeB.c_str());
    pairSheet->setCell(App::CellAddress(1, 3), "0.5");
    pairSheet->setCell(App::CellAddress(1, 4), "alias-exp-pair");
    _doc->recompute();
    auto pairTable = Assembly::parseInterferenceClearanceSheet(pairSheet, _assembly);
    ASSERT_EQ(pairTable.rules.size(), 1u);
    EXPECT_TRUE(pairTable.rules[0].valid);
    EXPECT_EQ(pairTable.rules[0].faceA, closeA);
    EXPECT_EQ(pairTable.rules[0].faceB, closeB);
    auto pairScan = runHostClearanceScan(_assembly, pairTable, 0.0);
    EXPECT_TRUE(pairScan.complete);
    EXPECT_EQ(pairScan.counts.penetrations, 0);
    EXPECT_GE(pairScan.counts.clearanceViolations, 1);
    const auto* pairHit = findFaceHitBetween(pairScan, closeA, closeB);
    ASSERT_NE(pairHit, nullptr);
    EXPECT_EQ(pairHit->ruleKind, Assembly::InterferenceClearanceRuleKind::ExactPair);
    EXPECT_NEAR(pairHit->appliedClearance, 0.5, 1e-12);
    EXPECT_EQ(pairHit->sourceComments[0], "alias-exp-pair");
}

TEST_F(InterferenceScanTest, clearanceFacePathAliasNeverCompletesClearCollapsed)
{
    auto* link = makeTwoElementGapArray(_doc, _assembly, "FalseClearCol", false, 0.4);
    auto leaves = Assembly::collectInterferenceLeaves(_assembly, false);
    ASSERT_EQ(leaves.size(), 2u);
    const auto [closeA, closeB] = closestFacePairBetween(leaves[0], leaves[1]);
    const std::string alias = std::string(link->getNameInDocument()) + ".0."
        + closeA.substr(closeA.find_last_of('.') + 1);

    auto* sheet = makeClearanceSheet(_doc, "FCC");
    sheet->setCell(App::CellAddress(1, 0), "true");
    sheet->setCell(App::CellAddress(1, 1), alias.c_str());
    sheet->setCell(App::CellAddress(1, 3), "0.5");
    sheet->setCell(App::CellAddress(1, 4), "alias-col");
    _doc->recompute();

    EXPECT_NO_THROW({
        auto table = Assembly::parseInterferenceClearanceSheet(sheet, _assembly);
        ASSERT_EQ(table.rules.size(), 1u);
        EXPECT_TRUE(table.rules[0].valid);
        EXPECT_EQ(table.rules[0].faceA, closeA);
        EXPECT_EQ(table.invalidRuleCount, 0);
        auto lookup =
            Assembly::lookupInterferenceClearance(table, closeA, closeB, 0.0);
        EXPECT_NEAR(lookup.clearanceMm, 0.5, 1e-12);
        auto scan = runHostClearanceScan(_assembly, table, 0.0);
        EXPECT_TRUE(scan.complete);
        EXPECT_EQ(scan.counts.invalidRules, 0);
        EXPECT_GE(scan.counts.clearanceViolations, 1);
        const auto* hit = findFaceHitBetween(scan, closeA, closeB);
        ASSERT_NE(hit, nullptr);
        EXPECT_NEAR(hit->appliedClearance, 0.5, 1e-12);
        EXPECT_NE(hit->ruleKind, Assembly::InterferenceClearanceRuleKind::AssemblyGlobal);
        ASSERT_FALSE(hit->sourceRows.empty());
        EXPECT_EQ(hit->sourceComments[0], "alias-col");
    });
}

TEST_F(InterferenceScanTest, clearanceFacePathLinkedPartTraversalCanonicalAndScan)
{
    auto expectRuleScan = [&](const std::string& inputFace,
                              const std::string& expectedCanonical,
                              const std::string& leafPathA,
                              const std::string& leafPathB,
                              const char* comment,
                              bool runScan = true) {
        auto* sheet = makeClearanceSheet(_doc, comment);
        sheet->setCell(App::CellAddress(1, 0), "true");
        sheet->setCell(App::CellAddress(1, 1), inputFace.c_str());
        sheet->setCell(App::CellAddress(1, 3), "0.5");
        sheet->setCell(App::CellAddress(1, 4), comment);
        _doc->recompute();
        auto table = Assembly::parseInterferenceClearanceSheet(sheet, _assembly);
        ASSERT_EQ(table.rules.size(), 1u) << comment;
        EXPECT_TRUE(table.rules[0].valid) << comment;
        EXPECT_EQ(table.rules[0].faceA, expectedCanonical) << comment;
        EXPECT_EQ(table.invalidRuleCount, 0) << comment;
        if (!runScan) {
            return;
        }
        auto leavesNow = Assembly::collectInterferenceLeaves(_assembly, false);
        const bool leavesReadyForScan = leavesNow.size() == 2 && leavesNow[0].shapeValid
            && leavesNow[1].shapeValid;
        if (!leavesReadyForScan) {
            return;
        }
        auto scan = runHostClearanceScan(_assembly, table, 0.0);
        EXPECT_TRUE(scan.complete) << comment;
        EXPECT_EQ(scan.counts.invalidRules, 0) << comment;
        EXPECT_GE(scan.counts.clearanceViolations, 1) << comment;
        const auto* hit = findFaceHitBetween(scan, leafPathA, leafPathB);
        ASSERT_NE(hit, nullptr) << comment;
        EXPECT_NEAR(hit->appliedClearance, 0.5, 1e-12) << comment;
        EXPECT_NE(hit->ruleKind, Assembly::InterferenceClearanceRuleKind::AssemblyGlobal) << comment;
        ASSERT_FALSE(hit->sourceRows.empty());
        EXPECT_EQ(hit->sourceComments[0], comment);
    };

    // Ordinary Link -> App::Part -> Box
    auto* partA = _doc->addObject<App::Part>("TraversePartA");
    auto* boxA = makeBox(_doc, "TraverseBoxA", Base::Vector3d(0, 0, 0), 10, 10, 10);
    partA->addObject(boxA);
    auto* linkA = freecad_cast<App::Link*>(_doc->addObject("App::Link", "TraverseLinkA"));
    linkA->setLink(-1, partA);
    _assembly->addObject(linkA);

    auto* boxFar = makeBox(_doc, "TraverseFar", Base::Vector3d(10.4, 0, 0), 10, 10, 10);
    _assembly->addObject(boxFar);
    _doc->recompute();

    auto leaves = Assembly::collectInterferenceLeaves(_assembly, false);
    ASSERT_GE(leaves.size(), 2u);
    const Assembly::InterferenceLeaf* linkLeaf = nullptr;
    const Assembly::InterferenceLeaf* farLeaf = nullptr;
    for (const auto& leaf : leaves) {
        if (leaf.occurrenceSubName.find(linkA->getNameInDocument()) != std::string::npos) {
            linkLeaf = &leaf;
        }
        if (leaf.occurrenceSubName.find(boxFar->getNameInDocument()) != std::string::npos) {
            farLeaf = &leaf;
        }
    }
    ASSERT_NE(linkLeaf, nullptr);
    ASSERT_NE(farLeaf, nullptr);
    const auto [linkCloseA, linkCloseB] = closestFacePairBetween(*linkLeaf, *farLeaf);
    const std::string canonicalLinkBox =
        std::string(linkA->getNameInDocument()) + "." + boxA->getNameInDocument() + "."
        + linkCloseA.substr(linkCloseA.find_last_of('.') + 1);
    EXPECT_EQ(linkCloseA, canonicalLinkBox);
    expectRuleScan(canonicalLinkBox, canonicalLinkBox, linkCloseA, linkCloseB, "ord-link-part");

    _assembly->removeObject(partA);
    _assembly->removeObject(linkA);
    _assembly->removeObject(boxFar);
    _doc->recompute();

    auto* expLink = makeTwoElementGapArray(_doc, _assembly, "TraverseExp", true, 0.4);
    leaves = Assembly::collectInterferenceLeaves(_assembly, false);
    ASSERT_EQ(leaves.size(), 2u);
    const auto [expCloseA, expCloseB] = closestFacePairBetween(leaves[0], leaves[1]);
    const std::string numericAlias =
        std::string(expLink->getNameInDocument()) + ".0."
        + expCloseA.substr(expCloseA.find_last_of('.') + 1);
    expectRuleScan(numericAlias, expCloseA, expCloseA, expCloseB, "exp-numeric-alias", true);

    _assembly->removeObject(expLink);
    _doc->recompute();
    auto* colLink = makeTwoElementGapArray(_doc, _assembly, "TraverseExpCol", false, 0.4);
    leaves = Assembly::collectInterferenceLeaves(_assembly, false);
    ASSERT_EQ(leaves.size(), 2u);
    const auto [colCloseA, colCloseB] = closestFacePairBetween(leaves[0], leaves[1]);
    const std::string colAlias = std::string(colLink->getNameInDocument()) + ".0."
        + colCloseA.substr(colCloseA.find_last_of('.') + 1);
    expectRuleScan(colAlias, colCloseA, colCloseA, colCloseB, "exp-alias-scan-col");

    // FaceB exact pair: collapsed array element vs Link -> Part -> Box
    auto* partB = _doc->addObject<App::Part>("TraversePartB");
    auto* boxB = makeBox(_doc, "TraverseBoxB", Base::Vector3d(0, 0, 0), 10, 10, 10);
    partB->addObject(boxB);
    auto* linkB = freecad_cast<App::Link*>(_doc->addObject("App::Link", "TraverseLinkB"));
    linkB->setLink(-1, partB);
    linkB->Placement.setValue(Base::Placement(Base::Vector3d(10.4, 0, 0), Base::Rotation()));
    _assembly->addObject(linkB);
    _doc->recompute();

    leaves = Assembly::collectInterferenceLeaves(_assembly, false);
    const Assembly::InterferenceLeaf* leafCol0 = nullptr;
    const Assembly::InterferenceLeaf* leafLinkB = nullptr;
    const std::string colPrefix = std::string(colLink->getNameInDocument()) + ".0.";
    for (const auto& leaf : leaves) {
        if (leaf.occurrenceSubName.rfind(colPrefix, 0) == 0) {
            leafCol0 = &leaf;
        }
        if (leaf.occurrenceSubName.find(linkB->getNameInDocument()) != std::string::npos) {
            leafLinkB = &leaf;
        }
    }
    ASSERT_NE(leafCol0, nullptr);
    ASSERT_NE(leafLinkB, nullptr);
    const auto [pairA, pairB] = closestFacePairBetween(*leafCol0, *leafLinkB);
    const std::string faceElemA = pairA.substr(pairA.find_last_of('.') + 1);
    const std::string faceElemB = pairB.substr(pairB.find_last_of('.') + 1);
    const std::string inputFaceA = std::string(colLink->getNameInDocument()) + ".0." + faceElemA;
    const std::string inputFaceB =
        std::string(linkB->getNameInDocument()) + "." + boxB->getNameInDocument() + "."
        + faceElemB;

    auto* pairSheet = makeClearanceSheet(_doc, "TraverseFaceB");
    pairSheet->setCell(App::CellAddress(1, 0), "true");
    pairSheet->setCell(App::CellAddress(1, 1), inputFaceA.c_str());
    pairSheet->setCell(App::CellAddress(1, 2), inputFaceB.c_str());
    pairSheet->setCell(App::CellAddress(1, 3), "0.5");
    pairSheet->setCell(App::CellAddress(1, 4), "faceb-pair");
    _doc->recompute();
    auto pairTable = Assembly::parseInterferenceClearanceSheet(pairSheet, _assembly);
    ASSERT_EQ(pairTable.rules.size(), 1u);
    EXPECT_TRUE(pairTable.rules[0].valid);
    EXPECT_EQ(pairTable.rules[0].faceA, pairA);
    EXPECT_EQ(pairTable.rules[0].faceB, pairB);
    auto pairScan = runHostClearanceScan(_assembly, pairTable, 0.0);
    EXPECT_TRUE(pairScan.complete);
    EXPECT_EQ(pairScan.counts.invalidRules, 0);
    EXPECT_GE(pairScan.counts.clearanceViolations, 1);
    const auto* pairHit = findFaceHitBetween(pairScan, pairA, pairB);
    ASSERT_NE(pairHit, nullptr);
    EXPECT_EQ(pairHit->ruleKind, Assembly::InterferenceClearanceRuleKind::ExactPair);
    ASSERT_FALSE(pairHit->sourceComments.empty());
    EXPECT_EQ(pairHit->sourceComments[0], "faceb-pair");
}

TEST_F(InterferenceScanTest, clearanceFacePathRejectsUnsafeArrayTokensExhaustive)
{
    auto* link = makeTwoElementGapArray(_doc, _assembly, "UnsafeArr", true, 0.4);
    const std::string prefix = std::string(link->getNameInDocument()) + ".";
    const std::string intMax = std::to_string(std::numeric_limits<int>::max());
    const std::string intMaxPlus =
        std::to_string(static_cast<long long>(std::numeric_limits<int>::max()) + 1);
    const std::string huge21 = "999999999999999999999";
    const std::string hugeMany(500, '9');

    std::vector<std::string> badFaces = {
        prefix + huge21 + ".Face1",
        prefix + hugeMany + ".Face1",
        prefix + intMax + ".Face1",
        prefix + intMaxPlus + ".Face1",
        prefix + "2.Face1",
        prefix + "-1.Face1",
        prefix + "+1.Face1",
        prefix + " 1.Face1",
        prefix + "0x10.Face1",
        prefix + "1.5.Face1",
        prefix + "1a.Face1",
        prefix + "01.Face1",
    };
    auto* sheet = makeClearanceSheet(_doc, "UnsafeSheet");
    int row = 1;
    for (const auto& face : badFaces) {
        sheet->setCell(App::CellAddress(row, 0), "true");
        sheet->setCell(App::CellAddress(row, 1), face.c_str());
        sheet->setCell(App::CellAddress(row, 3), "0.5");
        ++row;
    }
    sheet->setCell(App::CellAddress(row, 0), "true");
    sheet->setCell(App::CellAddress(row, 1), (prefix + "01.Face1").c_str());
    sheet->setCell(App::CellAddress(row, 2), (prefix + huge21 + ".Face1").c_str());
    sheet->setCell(App::CellAddress(row, 3), "0.5");
    _doc->recompute();

    EXPECT_NO_THROW({
        auto table = Assembly::parseInterferenceClearanceSheet(sheet, _assembly);
        EXPECT_EQ(table.invalidRuleCount, static_cast<int>(badFaces.size()) + 1);
        for (const auto& rule : table.rules) {
            EXPECT_FALSE(rule.valid);
        }
        auto scan = runHostClearanceScan(_assembly, table);
        EXPECT_FALSE(scan.complete);
        EXPECT_EQ(scan.counts.invalidRules, static_cast<int>(badFaces.size()) + 1);
    });
}

TEST_F(InterferenceScanTest, clearanceFacePathRejectsAmbiguousDuplicateLinkAliases)
{
    auto* part = _doc->addObject<App::Part>("AmbPart");
    auto* box = makeBox(_doc, "AmbChild", Base::Vector3d(0, 0, 0), 10, 10, 10);
    part->addObject(box);
    _doc->recompute();

    auto* linkShort = freecad_cast<App::Link*>(_doc->addObject("App::Link", "L"));
    auto* linkLong = freecad_cast<App::Link*>(_doc->addObject("App::Link", "LongLinkName"));
    linkShort->setLink(-1, part);
    linkLong->setLink(-1, part);
    linkLong->Placement.setValue(Base::Placement(Base::Vector3d(40, 0, 0), Base::Rotation()));

    auto expectAmbiguousFaceRule = [&](const std::string& facePath, const char* tag) {
        auto* sheet = makeClearanceSheet(_doc, tag);
        sheet->setCell(App::CellAddress(1, 0), "true");
        sheet->setCell(App::CellAddress(1, 1), facePath.c_str());
        sheet->setCell(App::CellAddress(1, 3), "0.5");
        _doc->recompute();
        auto table = Assembly::parseInterferenceClearanceSheet(sheet, _assembly);
        ASSERT_EQ(table.rules.size(), 1u) << tag;
        EXPECT_FALSE(table.rules[0].valid) << tag;
        EXPECT_EQ(table.invalidRuleCount, 1) << tag;
        EXPECT_TRUE(table.rules[0].diagnostic.find("Ambiguous") != std::string::npos
                    || table.rules[0].diagnostic.find("ambiguous") != std::string::npos)
            << table.rules[0].diagnostic;
    };

    auto addLinksAndRun = [&](bool reverseOrder, bool hideSecond) {
        _assembly->removeObject(linkShort);
        _assembly->removeObject(linkLong);
        if (reverseOrder) {
            _assembly->addObject(linkLong);
            _assembly->addObject(linkShort);
        }
        else {
            _assembly->addObject(linkShort);
            _assembly->addObject(linkLong);
        }
        linkShort->Visibility.setValue(true);
        linkLong->Visibility.setValue(!hideSecond);
        _doc->recompute();

        const std::string childFace = std::string(box->getNameInDocument()) + ".Face1";
        expectAmbiguousFaceRule(childFace, reverseOrder ? "rev-child" : "fwd-child");

        auto* pairSheet = makeClearanceSheet(_doc, "AmbPair");
        pairSheet->setCell(App::CellAddress(1, 0), "true");
        pairSheet->setCell(App::CellAddress(1, 1), childFace.c_str());
        pairSheet->setCell(
            App::CellAddress(1, 2),
            (std::string(linkShort->getNameInDocument()) + "." + box->getNameInDocument()
             + ".Face2")
                .c_str()
        );
        pairSheet->setCell(App::CellAddress(1, 3), "0.5");
        _doc->recompute();
        EXPECT_NO_THROW({
            auto pairTable = Assembly::parseInterferenceClearanceSheet(pairSheet, _assembly);
            EXPECT_EQ(pairTable.invalidRuleCount, 1);
            ASSERT_EQ(pairTable.rules.size(), 1u);
            EXPECT_FALSE(pairTable.rules[0].valid);
        });

        auto* faceBSheet = makeClearanceSheet(_doc, "AmbFaceB");
        faceBSheet->setCell(App::CellAddress(1, 0), "true");
        faceBSheet->setCell(
            App::CellAddress(1, 1),
            (std::string(linkShort->getNameInDocument()) + "." + box->getNameInDocument()
             + ".Face1")
                .c_str()
        );
        faceBSheet->setCell(App::CellAddress(1, 2), childFace.c_str());
        faceBSheet->setCell(App::CellAddress(1, 3), "0.5");
        _doc->recompute();
        auto faceBTable = Assembly::parseInterferenceClearanceSheet(faceBSheet, _assembly);
        EXPECT_EQ(faceBTable.invalidRuleCount, 1);
        EXPECT_FALSE(faceBTable.rules[0].valid);
    };

    addLinksAndRun(false, false);
    addLinksAndRun(true, false);
    addLinksAndRun(false, true);
}

TEST_F(InterferenceScanTest, strictArrayAliasWinsBeforeAmbiguousSuffixFallback)
{
    auto* direct =
        makeTwoElementGapArray(_doc, _assembly, "PrecedenceArray", true, 0.4);
    ASSERT_EQ(direct->ElementList.getSize(), 2);
    auto* directElement0 =
        freecad_cast<App::LinkElement*>(direct->ElementList.getValues()[0]);
    auto* directElement1 =
        freecad_cast<App::LinkElement*>(direct->ElementList.getValues()[1]);
    ASSERT_NE(directElement0, nullptr);
    ASSERT_NE(directElement1, nullptr);

    const std::string otherDocName =
        App::GetApplication().getUniqueDocumentName("asmInterferencePrecedenceOther");
    App::Document* otherDoc = App::GetApplication().newDocument(
        otherDocName.c_str(),
        "asmInterferencePrecedenceOtherUser"
    );
    auto* foreignPart = otherDoc->addObject<App::Part>("ForeignPart");
    auto* foreignSource =
        makeBox(otherDoc, "PrecedenceSource", Base::Vector3d(0, 0, 0), 10, 10, 10);
    auto* foreignArray =
        freecad_cast<App::Link*>(otherDoc->addObject("App::Link", "PrecedenceArray"));
    foreignArray->setLink(-1, foreignSource);
    foreignArray->ShowElement.setValue(false);
    foreignArray->ElementCount.setValue(2);
    foreignArray->PlacementList.setValues(
        {
            Base::Placement(Base::Vector3d(0, 0, 0), Base::Rotation()),
            Base::Placement(Base::Vector3d(10.4, 0, 0), Base::Rotation()),
        }
    );
    foreignPart->addObject(foreignArray);
    otherDoc->recompute();

    // External links need persisted owner/target documents.
    const std::string ownerPath = std::string("/tmp/") + _docName + "_precedence.FCStd";
    const std::string otherPath =
        std::string("/tmp/") + otherDocName + "_precedence.FCStd";
    _doc->FileName.setValue(ownerPath.c_str());
    otherDoc->FileName.setValue(otherPath.c_str());
    ASSERT_TRUE(_doc->save());
    ASSERT_TRUE(otherDoc->save());

    auto* instanceA =
        freecad_cast<App::Link*>(_doc->addObject("App::Link", "PrecedenceInstanceA"));
    auto* instanceB =
        freecad_cast<App::Link*>(_doc->addObject("App::Link", "PrecedenceInstanceB"));
    instanceA->setLink(-1, foreignPart);
    instanceB->setLink(-1, foreignPart);
    instanceB->Placement.setValue(
        Base::Placement(Base::Vector3d(100, 0, 0), Base::Rotation())
    );
    _assembly->addObject(instanceA);
    _assembly->addObject(instanceB);
    _doc->recompute();

    const std::string aliasA =
        std::string(direct->getNameInDocument()) + ".0.Face1";
    const std::string aliasB =
        std::string(direct->getNameInDocument()) + ".1.Face2";
    const std::string expectedA =
        std::string(direct->getNameInDocument()) + "."
        + directElement0->getNameInDocument() + ".Face1";
    const std::string expectedB =
        std::string(direct->getNameInDocument()) + "."
        + directElement1->getNameInDocument() + ".Face2";

    auto* sheet = makeClearanceSheet(_doc, "StrictBeforeSuffix");
    sheet->setCell(App::CellAddress(1, 0), "true");
    sheet->setCell(App::CellAddress(1, 1), aliasA.c_str());
    sheet->setCell(App::CellAddress(1, 2), aliasB.c_str());
    sheet->setCell(App::CellAddress(1, 3), "0.5");
    _doc->recompute();

    Assembly::InterferenceClearanceSheetParseStats stats;
    EXPECT_NO_THROW({
        const auto table =
            Assembly::parseInterferenceClearanceSheet(sheet, _assembly, &stats);
        ASSERT_EQ(table.rules.size(), 1u);
        EXPECT_EQ(table.invalidRuleCount, 0);
        EXPECT_TRUE(table.rules.front().valid) << table.rules.front().diagnostic;
        EXPECT_EQ(table.rules.front().faceA, expectedA);
        EXPECT_EQ(table.rules.front().faceB, expectedB);
    });
    EXPECT_EQ(stats.hostLeafCollectionPasses, 0);
    EXPECT_EQ(stats.structuralOccurrenceTraversalPasses, 1);
    EXPECT_EQ(stats.leafShapeExtractions, 2);
    EXPECT_EQ(stats.faceEnumerationPasses, 2);

    App::GetApplication().closeDocument(otherDocName.c_str());
    std::remove(ownerPath.c_str());
    std::remove(otherPath.c_str());
}

TEST_F(InterferenceScanTest, clearanceSheetParsingUsesSingleHostTraversal)
{
    auto* boxA = makeBox(_doc, "ParseBoxA", Base::Vector3d(0, 0, 0), 10, 10, 10);
    auto* boxB = makeBox(_doc, "ParseBoxB", Base::Vector3d(10.4, 0, 0), 10, 10, 10);
    _assembly->addObject(boxA);
    _assembly->addObject(boxB);
    _doc->recompute();
    auto leaves = Assembly::collectInterferenceLeaves(_assembly, false);
    ASSERT_EQ(leaves.size(), 2u);
    const auto [closeA, closeB] = closestFacePairBetween(leaves[0], leaves[1]);

    Assembly::InterferenceClearanceSheetParseStats oneRuleStats;
    {
        auto* sheet = makeClearanceSheet(_doc, "ParseOne");
        sheet->setCell(App::CellAddress(1, 0), "true");
        sheet->setCell(App::CellAddress(1, 1), closeA.c_str());
        sheet->setCell(App::CellAddress(1, 3), "0.5");
        _doc->recompute();
        auto table = Assembly::parseInterferenceClearanceSheet(sheet, _assembly, &oneRuleStats);
        EXPECT_EQ(table.invalidRuleCount, 0);
        EXPECT_TRUE(table.rules[0].valid);
    }
    EXPECT_EQ(oneRuleStats.hostLeafCollectionPasses, 0);
    EXPECT_EQ(oneRuleStats.structuralOccurrenceTraversalPasses, 1);
    EXPECT_EQ(oneRuleStats.leafShapeExtractions, 1);
    EXPECT_EQ(oneRuleStats.faceEnumerationPasses, 1);

    auto* manySheet = makeClearanceSheet(_doc, "ParseMany");
    for (int row = 1; row <= 100; ++row) {
        manySheet->setCell(App::CellAddress(row, 0), "true");
        if (row % 2 == 1) {
            manySheet->setCell(App::CellAddress(row, 1), closeA.c_str());
        }
        else {
            manySheet->setCell(App::CellAddress(row, 1), closeB.c_str());
            manySheet->setCell(App::CellAddress(row, 2), closeA.c_str());
        }
        manySheet->setCell(App::CellAddress(row, 3), "0.5");
    }
    _doc->recompute();
    Assembly::InterferenceClearanceSheetParseStats manyRuleStats;
    auto manyTable =
        Assembly::parseInterferenceClearanceSheet(manySheet, _assembly, &manyRuleStats);
    EXPECT_EQ(manyTable.invalidRuleCount, 0);
    EXPECT_EQ(manyTable.rules.size(), 100u);
    EXPECT_EQ(manyRuleStats.hostLeafCollectionPasses, 0);
    EXPECT_EQ(manyRuleStats.structuralOccurrenceTraversalPasses, 1);
    EXPECT_EQ(manyRuleStats.leafShapeExtractions, 2);
    EXPECT_GE(manyRuleStats.occurrenceValidationCacheHits, 98);

    auto* badSheet = makeClearanceSheet(_doc, "ParseBadMany");
    for (int row = 1; row <= 40; ++row) {
        badSheet->setCell(App::CellAddress(row, 0), "true");
        badSheet->setCell(
            App::CellAddress(row, 1),
            (std::string("Missing") + std::to_string(row) + ".Face1").c_str()
        );
        badSheet->setCell(App::CellAddress(row, 3), "0.5");
    }
    _doc->recompute();
    Assembly::InterferenceClearanceSheetParseStats badStats;
    EXPECT_NO_THROW({
        auto badTable = Assembly::parseInterferenceClearanceSheet(badSheet, _assembly, &badStats);
        EXPECT_EQ(badTable.invalidRuleCount, 40);
        EXPECT_EQ(badTable.rules.size(), 40u);
    });
    EXPECT_EQ(badStats.hostLeafCollectionPasses, 0);
    EXPECT_EQ(badStats.structuralOccurrenceTraversalPasses, 1);
    EXPECT_EQ(badStats.leafShapeExtractions, 0);
}

TEST_F(InterferenceScanTest, clearanceSheetParseSkipsHostGeometryWhenUnneeded)
{
    auto* box = makeBox(_doc, "SkipGeomBox", Base::Vector3d(0, 0, 0), 10, 10, 10);
    _assembly->addObject(box);
    _doc->recompute();

    auto expectNoHostWork = [&](Spreadsheet::Sheet* sheet, const char* tag) {
        Assembly::InterferenceClearanceSheetParseStats stats;
        auto table = Assembly::parseInterferenceClearanceSheet(sheet, _assembly, &stats);
        EXPECT_EQ(stats.hostLeafCollectionPasses, 0) << tag;
        EXPECT_EQ(stats.structuralOccurrenceTraversalPasses, 0) << tag;
        EXPECT_EQ(stats.leafShapeExtractions, 0) << tag;
        EXPECT_EQ(stats.faceEnumerationPasses, 0) << tag;
        return table;
    };

    expectNoHostWork(nullptr, "null-sheet");

    auto* empty = makeClearanceSheet(_doc, "SkipEmpty");
    expectNoHostWork(empty, "empty");

    auto* headersOnly = makeClearanceSheet(_doc, "SkipHdrOnly");
    _doc->recompute();
    expectNoHostWork(headersOnly, "header-only");

    auto* disabled = makeClearanceSheet(_doc, "SkipDisabled");
    disabled->setCell(App::CellAddress(1, 0), "false");
    disabled->setCell(App::CellAddress(1, 1), "Garbage.OversizedPath.Face9999999999");
    disabled->setCell(App::CellAddress(1, 3), "not-a-number");
    _doc->recompute();
    expectNoHostWork(disabled, "disabled-only");

    auto* starOnly = makeClearanceSheet(_doc, "SkipStar");
    starOnly->setCell(App::CellAddress(1, 0), "true");
    starOnly->setCell(App::CellAddress(1, 1), "*");
    starOnly->setCell(App::CellAddress(1, 3), "0.25");
    _doc->recompute();
    auto starTable = expectNoHostWork(starOnly, "star-only");
    EXPECT_EQ(starTable.invalidRuleCount, 0);
    EXPECT_TRUE(starTable.hasDefaultStar);

    auto* badHdr = _doc->addObject<Spreadsheet::Sheet>("SkipBadHdr");
    badHdr->setCell(App::CellAddress(0, 0), "Enabled");
    badHdr->setCell(App::CellAddress(0, 1), "Comment");
    _doc->recompute();
    auto badHdrTable = expectNoHostWork(badHdr, "malformed-header");
    EXPECT_GE(badHdrTable.invalidRuleCount, 1);
}

TEST_F(
    InterferenceScanTest,
    clearanceSheetDuplicateSemanticHeadersFailClosedWithoutHostWork
)
{
    auto* box = makeBox(_doc, "DuplicateHeaderBox", Base::Vector3d(0, 0, 0), 10, 10, 10);
    _assembly->addObject(box);
    _doc->recompute();

    struct DuplicateCase
    {
        const char* name;
        const char* first;
        const char* second;
        bool reverse;
    };
    const std::vector<DuplicateCase> cases {
        {"DupEnabled", "Enabled", "Enabled", false},
        {"DupFaceAlias", "Face", "FaceA", false},
        {"DupFaceB", "FaceB", "FaceB", false},
        {"DupToleranceAlias", "Tolerance", "Clearance", false},
        {"DupComment", "Comment", "Comment", false},
        {"DupFaceAliasReverse", "FaceA", "Face", true},
        {"DupToleranceAliasReverse", "Clearance", "Tolerance", true},
        {"DupEnabledReverse", "Enabled", "Enabled", true},
    };

    for (const auto& testCase : cases) {
        auto* sheet = _doc->addObject<Spreadsheet::Sheet>(testCase.name);
        const int duplicateColA = testCase.reverse ? 6 : 5;
        const int duplicateColB = testCase.reverse ? 5 : 6;
        sheet->setCell(App::CellAddress(0, 0), "Enabled");
        sheet->setCell(App::CellAddress(0, 1), "Face");
        sheet->setCell(App::CellAddress(0, 2), "FaceB");
        sheet->setCell(App::CellAddress(0, 3), "Tolerance");
        sheet->setCell(App::CellAddress(0, 4), "Comment");
        sheet->setCell(App::CellAddress(0, duplicateColA), testCase.first);
        sheet->setCell(App::CellAddress(0, duplicateColB), testCase.second);

        const std::string facePath =
            std::string(box->getNameInDocument()) + ".Face1";
        sheet->setCell(App::CellAddress(1, 0), "true");
        sheet->setCell(App::CellAddress(1, 1), facePath.c_str());
        sheet->setCell(App::CellAddress(1, 2), facePath.c_str());
        sheet->setCell(App::CellAddress(1, 3), "0.5");
        sheet->setCell(App::CellAddress(1, 4), "must-not-be-accepted");
        sheet->setCell(App::CellAddress(1, duplicateColA), "true");
        sheet->setCell(App::CellAddress(1, duplicateColB), "999");
        _doc->recompute();

        Assembly::InterferenceClearanceSheetParseStats stats;
        EXPECT_NO_THROW({
            const auto table =
                Assembly::parseInterferenceClearanceSheet(sheet, _assembly, &stats);
            EXPECT_EQ(table.invalidRuleCount, 1) << testCase.name;
            EXPECT_TRUE(table.rules.empty()) << testCase.name;
            EXPECT_FALSE(table.hasDefaultStar) << testCase.name;
            EXPECT_DOUBLE_EQ(table.maxEnabledClearance, 0.0) << testCase.name;
            ASSERT_EQ(table.diagnostics.size(), 1u) << testCase.name;
            EXPECT_NE(
                table.diagnostics.front().find("Duplicate"),
                std::string::npos
            ) << table.diagnostics.front();
        });
        EXPECT_EQ(stats.hostLeafCollectionPasses, 0) << testCase.name;
        EXPECT_EQ(stats.structuralOccurrenceTraversalPasses, 0) << testCase.name;
        EXPECT_EQ(stats.leafShapeExtractions, 0) << testCase.name;
        EXPECT_EQ(stats.faceEnumerationPasses, 0) << testCase.name;
    }
}

TEST_F(InterferenceScanTest, clearanceSheetParseReusesPreparedLeaves)
{
    auto* boxA = makeBox(_doc, "ReuseBoxA", Base::Vector3d(0, 0, 0), 10, 10, 10);
    auto* boxB = makeBox(_doc, "ReuseBoxB", Base::Vector3d(12, 0, 0), 10, 10, 10);
    _assembly->addObject(boxA);
    _assembly->addObject(boxB);
    _doc->recompute();

    auto snap = Assembly::prepareInterferenceComponentScanSnapshot(_assembly, false);
    ASSERT_GE(snap.leaves.size(), 2u);
    const std::string faceA = snap.leaves[0].occurrenceSubName + "Face1";

    auto* sheet = makeClearanceSheet(_doc, "ReusePrepared");
    sheet->setCell(App::CellAddress(1, 0), "true");
    sheet->setCell(App::CellAddress(1, 1), faceA.c_str());
    sheet->setCell(App::CellAddress(1, 3), "0.5");
    _doc->recompute();

    Assembly::InterferenceClearanceSheetParseStats stats;
    auto table = Assembly::parseInterferenceClearanceSheet(
        sheet,
        _assembly,
        &stats,
        &snap.leaves
    );
    EXPECT_EQ(table.invalidRuleCount, 0);
    EXPECT_EQ(stats.leafShapeExtractions, 0);
    EXPECT_GE(stats.occurrenceValidationCacheHits, 1);
    EXPECT_EQ(stats.faceEnumerationPasses, 1);
}

TEST_F(InterferenceScanTest, emptyLinkedSheetThenHostClearanceScanSucceeds)
{
    auto* boxA = makeBox(_doc, "EmptySheetA", Base::Vector3d(0, 0, 0), 10, 10, 10);
    auto* boxB = makeBox(_doc, "EmptySheetB", Base::Vector3d(10.4, 0, 0), 10, 10, 10);
    _assembly->addObject(boxA);
    _assembly->addObject(boxB);
    _doc->recompute();

    auto* sheet = makeClearanceSheet(_doc, "EmptyLinked");
    _assembly->setInterferenceClearanceSheet(sheet);
    Assembly::setInterferenceClearance(_assembly, 0.5);
    _doc->recompute();

    Assembly::InterferenceClearanceSheetParseStats stats;
    auto rules = Assembly::parseInterferenceClearanceSheet(sheet, _assembly, &stats);
    EXPECT_EQ(stats.hostLeafCollectionPasses, 0);
    EXPECT_EQ(stats.structuralOccurrenceTraversalPasses, 0);

    auto leaves = Assembly::collectInterferenceLeaves(_assembly, false);
    ASSERT_EQ(leaves.size(), 2u);
    Assembly::InterferenceScanOptions options;
    options.clearance = 0.5;
    options.clearanceRules = rules;
    auto result = Assembly::runInterferenceScan(leaves, options);
    EXPECT_TRUE(result.complete);
    EXPECT_GE(result.counts.clearanceViolations, 1);
}

TEST_F(InterferenceScanTest, clearanceSheetHiddenOccurrenceReferencedByRule)
{
    auto* box = makeBox(_doc, "HiddenRuleBox", Base::Vector3d(0, 0, 0), 10, 10, 10);
    auto* link = freecad_cast<App::Link*>(_doc->addObject("App::Link", "HiddenRuleLink"));
    link->setLink(-1, box);
    link->Visibility.setValue(false);
    _assembly->addObject(link);
    _doc->recompute();

    const std::string facePath = std::string(link->getNameInDocument()) + ".Face1";
    auto* sheet = makeClearanceSheet(_doc, "HiddenRuleSheet");
    sheet->setCell(App::CellAddress(1, 0), "true");
    sheet->setCell(App::CellAddress(1, 1), facePath.c_str());
    sheet->setCell(App::CellAddress(1, 3), "0.5");
    _doc->recompute();

    Assembly::InterferenceClearanceSheetParseStats stats;
    auto table = Assembly::parseInterferenceClearanceSheet(sheet, _assembly, &stats);
    EXPECT_EQ(table.invalidRuleCount, 0);
    ASSERT_EQ(table.rules.size(), 1u);
    EXPECT_TRUE(table.rules[0].valid);
    EXPECT_EQ(stats.hostLeafCollectionPasses, 0);
    EXPECT_EQ(stats.structuralOccurrenceTraversalPasses, 1);
    EXPECT_EQ(stats.leafShapeExtractions, 1);
}

TEST_F(InterferenceScanTest, collapsedArrayPrefixExtractionIsScopedAndRejectsBadPaths)
{
    auto* link = makeTwoElementGapArray(_doc, _assembly, "CollapsedScope", false, 0.4);
    auto* noise =
        makeBox(_doc, "CollapsedScopeNoise", Base::Vector3d(10.4, 0, 0), 10, 10, 10);
    _assembly->addObject(noise);
    _doc->recompute();

    const std::string prefix0 =
        std::string(link->getNameInDocument()) + ".0.";
    const std::string prefix1 =
        std::string(link->getNameInDocument()) + ".1.";
    std::vector<Assembly::InterferenceLeaf> leaves0;
    std::vector<Assembly::InterferenceLeaf> leaves1;
    EXPECT_NO_THROW({
        leaves0 =
            Assembly::collectInterferenceLeavesUnderPrefix(_assembly, prefix0, true);
        leaves1 =
            Assembly::collectInterferenceLeavesUnderPrefix(_assembly, prefix1, true);
    });
    ASSERT_EQ(leaves0.size(), 1u);
    ASSERT_EQ(leaves1.size(), 1u);
    EXPECT_EQ(leaves0.front().occurrenceSubName, prefix0);
    EXPECT_EQ(leaves1.front().occurrenceSubName, prefix1);
    EXPECT_NEAR(leaves0.front().worldBoundBox.MinX, 0.0, 0.05);
    EXPECT_NEAR(leaves1.front().worldBoundBox.MinX, 10.4, 0.05);
    EXPECT_EQ(
        leaves0.front().occurrenceSubName.find(noise->getNameInDocument()),
        std::string::npos
    );
    EXPECT_EQ(
        leaves1.front().occurrenceSubName.find(noise->getNameInDocument()),
        std::string::npos
    );

    Assembly::InterferenceScanOptions options;
    auto result = Assembly::runInterferenceScanBetweenLeafSets(leaves0, leaves1, options);
    EXPECT_TRUE(result.complete);
    EXPECT_EQ(result.counts.penetrations, 0);

    const std::vector<std::string> badPrefixes {
        std::string(link->getNameInDocument()) + ".2.",
        std::string(link->getNameInDocument()) + ".01.",
        std::string(link->getNameInDocument()) + ".999999999999999999999.",
        std::string(link->getNameInDocument()) + ".1.Extra.",
        "MissingCollapsedScope.0.",
    };
    for (const auto& badPrefix : badPrefixes) {
        EXPECT_NO_THROW({
            const auto leaves =
                Assembly::collectInterferenceLeavesUnderPrefix(_assembly, badPrefix, true);
            EXPECT_TRUE(leaves.empty()) << badPrefix;
        });
    }
}

TEST_F(InterferenceScanTest, clearanceSheetSelectedPairScopeDoesNotExtractUnrelated)
{
    auto* source = makeBox(_doc, "SelScopeSrc", Base::Vector3d(0, 0, 0), 20, 20, 20);
    auto* link = freecad_cast<App::Link*>(_doc->addObject("App::Link", "SelScopeArr"));
    link->setLink(-1, source);
    link->ShowElement.setValue(true);
    link->ElementCount.setValue(2);
    auto* noise = makeBox(_doc, "SelScopeNoise", Base::Vector3d(200, 0, 0), 10, 10, 10);
    auto* bad = makeBox(_doc, "SelScopeBad", Base::Vector3d(0, 0, 0), 10, 10, 10);
    bad->Visibility.setValue(false);
    _assembly->addObject(link);
    _assembly->addObject(noise);
    _assembly->addObject(bad);
    _doc->recompute();
    ASSERT_EQ(link->ElementList.getSize(), 2);
    auto* elt0 = freecad_cast<App::LinkElement*>(link->ElementList.getValues()[0]);
    auto* elt1 = freecad_cast<App::LinkElement*>(link->ElementList.getValues()[1]);
    ASSERT_NE(elt0, nullptr);
    ASSERT_NE(elt1, nullptr);
    elt0->Placement.setValue(Base::Placement(Base::Vector3d(0, 0, 0), Base::Rotation()));
    elt1->Placement.setValue(Base::Placement(Base::Vector3d(5, 0, 0), Base::Rotation()));
    _doc->recompute();

    const std::string prefix0 =
        std::string("SelScopeArr.") + elt0->getNameInDocument() + ".";
    const std::string prefix1 =
        std::string("SelScopeArr.") + elt1->getNameInDocument() + ".";
    const std::string face0 = prefix0 + "Face1";
    const std::string face1 = prefix1 + "Face1";

    auto* sheet = makeClearanceSheet(_doc, "SelScopeSheet");
    sheet->setCell(App::CellAddress(1, 0), "true");
    sheet->setCell(App::CellAddress(1, 1), face0.c_str());
    sheet->setCell(App::CellAddress(1, 2), face1.c_str());
    sheet->setCell(App::CellAddress(1, 3), "0.5");
    _doc->recompute();

    auto leavesA = Assembly::collectInterferenceLeavesUnderPrefix(_assembly, prefix0, true);
    auto leavesB = Assembly::collectInterferenceLeavesUnderPrefix(_assembly, prefix1, true);
    ASSERT_FALSE(leavesA.empty());
    ASSERT_FALSE(leavesB.empty());

    std::vector<Assembly::InterferenceLeaf> preparedPairLeaves;
    preparedPairLeaves.reserve(leavesA.size() + leavesB.size());
    preparedPairLeaves.insert(preparedPairLeaves.end(), leavesA.begin(), leavesA.end());
    preparedPairLeaves.insert(preparedPairLeaves.end(), leavesB.begin(), leavesB.end());

    Assembly::InterferenceClearanceSheetParseStats parseStats;
    auto table = Assembly::parseInterferenceClearanceSheet(
        sheet,
        _assembly,
        &parseStats,
        &preparedPairLeaves
    );
    EXPECT_EQ(table.invalidRuleCount, 0);
    EXPECT_EQ(parseStats.hostLeafCollectionPasses, 0);
    EXPECT_EQ(parseStats.leafShapeExtractions, 0);
    EXPECT_EQ(parseStats.faceEnumerationPasses, 2);
    EXPECT_GE(parseStats.occurrenceValidationCacheHits, 2);

    Assembly::InterferenceScanOptions options;
    options.clearance = 0.0;
    options.clearanceRules = table;
    auto result = Assembly::runInterferenceScanBetweenLeafSets(leavesA, leavesB, options);
    EXPECT_TRUE(result.complete);
    EXPECT_GE(result.counts.penetrations, 1);
    for (const auto& leaf : result.leaves) {
        EXPECT_TRUE(leaf.occurrenceSubName.rfind(prefix0, 0) == 0
                    || leaf.occurrenceSubName.rfind(prefix1, 0) == 0);
    }
}

TEST_F(InterferenceScanTest, badClearanceSheetsAvoidHostLeafCollection)
{
    auto* box = makeBox(_doc, "BadNoHostBox", Base::Vector3d(0, 0, 0), 10, 10, 10);
    _assembly->addObject(box);
    _doc->recompute();

    Assembly::InterferenceClearanceSheetParseStats stats;
    auto* sheet = makeClearanceSheet(_doc, "BadNoHost");
    sheet->setCell(App::CellAddress(1, 0), "true");
    sheet->setCell(App::CellAddress(1, 1), "NoSuch.Face1");
    sheet->setCell(App::CellAddress(1, 3), "1 kg");
    sheet->setCell(App::CellAddress(2, 0), "true");
    sheet->setCell(App::CellAddress(2, 1), "*");
    sheet->setCell(App::CellAddress(2, 2), "Other.Face1");
    sheet->setCell(App::CellAddress(2, 3), "0.5");
    _doc->recompute();
    auto table = Assembly::parseInterferenceClearanceSheet(sheet, _assembly, &stats);
    EXPECT_GE(table.invalidRuleCount, 2);
    EXPECT_EQ(stats.hostLeafCollectionPasses, 0);
    EXPECT_EQ(stats.structuralOccurrenceTraversalPasses, 0);
    EXPECT_EQ(stats.leafShapeExtractions, 0);
}
