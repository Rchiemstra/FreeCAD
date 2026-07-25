// SPDX-License-Identifier: LGPL-2.1-or-later

#include <gtest/gtest.h>

#include <BRepPrimAPI_MakeBox.hxx>
#include <Standard_Failure.hxx>
#include <gp_Pnt.hxx>
#include <atomic>
#include <cmath>
#include <limits>
#include <cstdio>
#include <sstream>

#include <App/Document.h>
#include <App/DocumentObjectGroup.h>
#include <App/Link.h>
#include <App/Part.h>
#include <Base/Interpreter.h>
#include <Base/Placement.h>
#include <Mod/Assembly/App/AssemblyLink.h>
#include <Mod/Assembly/App/AssemblyObject.h>
#include <Mod/Assembly/App/InterferenceScan.h>
#include <Mod/Assembly/App/InterferenceScanSession.h>
#include <Mod/Part/App/FeaturePartBox.h>
#include <Mod/PartDesign/App/Body.h>
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
        // Body::execute bakes Tip geometry into local Body space (strips tip placement),
        // then assembly path applies Body.Placement.
        EXPECT_NEAR(leaves[0].worldBoundBox.MinX, 100.0, 1e-4);
        EXPECT_NEAR(leaves[0].worldBoundBox.MaxX, 110.0, 1e-4);
        EXPECT_NEAR(leaves[0].worldBoundBox.MinY, 0.0, 1e-4);
        EXPECT_NEAR(leaves[0].worldBoundBox.MaxY, 4.0, 1e-4);
        EXPECT_NEAR(leaves[0].worldBoundBox.MinZ, 0.0, 1e-4);
        EXPECT_NEAR(leaves[0].worldBoundBox.MaxZ, 5.0, 1e-4);

        auto* other = makeBox(_doc, "OtherTip", Base::Vector3d(0, 0, 0), 2, 2, 2);
        body->BaseFeature.setValue(other);
        _doc->recompute();
        leaves = Assembly::collectInterferenceLeaves(_assembly, false);
        ASSERT_EQ(leaves.size(), 1u);
        EXPECT_NEAR(leaves[0].worldBoundBox.MinX, 100.0, 1e-4);
        EXPECT_NEAR(leaves[0].worldBoundBox.MaxX, 102.0, 1e-4);
        EXPECT_NEAR(leaves[0].worldBoundBox.MaxY, 2.0, 1e-4);
        EXPECT_NEAR(leaves[0].worldBoundBox.MaxZ, 2.0, 1e-4);
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
    EXPECT_EQ(wholes.mode, InterferenceScanScopeMode::AllComponents);
    EXPECT_EQ(wholes.subelementHandleCount, 0);
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
