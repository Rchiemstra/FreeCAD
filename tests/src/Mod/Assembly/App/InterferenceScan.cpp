// SPDX-License-Identifier: LGPL-2.1-or-later

#include <gtest/gtest.h>

#include <BRepPrimAPI_MakeBox.hxx>
#include <Standard_Failure.hxx>
#include <gp_Pnt.hxx>
#include <cstdio>
#include <sstream>

#include <App/Document.h>
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
