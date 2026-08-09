// SPDX-License-Identifier: LGPL-2.1-or-later

#include <gtest/gtest.h>

#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>
#include <string_view>
#include <vector>

namespace
{

std::string readText(const std::filesystem::path& path)
{
    std::ifstream stream(path, std::ios::binary);
    EXPECT_TRUE(stream.is_open()) << path.string();
    return {std::istreambuf_iterator<char>(stream), std::istreambuf_iterator<char>()};
}

TEST(CollaborationAuthorityRemovalTest, productionAuthoritySurfaceIsUnreachable)
{
    const std::filesystem::path root(TEST_SOURCE_DIR);
    const std::vector<std::filesystem::path> removedPaths {
        "src/App/DocumentMutationAuthority.h",
        "src/App/DocumentMutationAuthority.cpp",
        "src/App/MutationCapability.h",
        "src/App/MutationCapability.cpp",
        "src/Gui/Dialogs/DlgMutationTakeover.h",
        "src/Gui/Dialogs/DlgMutationTakeover.cpp",
    };
    for (const auto& relative : removedPaths) {
        EXPECT_FALSE(std::filesystem::exists(root / relative)) << relative.string();
    }

    const std::vector<std::string_view> retiredSymbols {
        "DocumentMutationAuthority",
        "MutationCapability",
        "MutationOwner",
        "MutationOrigin",
        "MutationAuthorityTLS",
        "MutationInternalScope",
        "DlgMutationTakeover",
        "setMutationOwner",
        "clearMutationOwner",
        "openMutationCapability",
        "bumpMutationGeneration",
        "mutationAuthorityStatus",
        "sync_gui_lease_takeover",
    };
    for (const auto& relative : {std::filesystem::path("src/App"),
                                 std::filesystem::path("src/Gui")}) {
        for (const auto& entry : std::filesystem::recursive_directory_iterator(root / relative)) {
            if (!entry.is_regular_file()) {
                continue;
            }
            const auto extension = entry.path().extension();
            if (extension != ".h" && extension != ".cpp" && extension != ".pyi"
                && entry.path().filename() != "CMakeLists.txt") {
                continue;
            }
            const auto contents = readText(entry.path());
            for (const auto symbol : retiredSymbols) {
                EXPECT_EQ(contents.find(symbol), std::string::npos)
                    << entry.path().string() << " still contains " << symbol;
            }
        }
    }
}

TEST(CollaborationAuthorityRemovalTest, alterDocumentFlagRetainsOrdinaryCommandSemantics)
{
    const std::filesystem::path root(TEST_SOURCE_DIR);
    const auto commandHeader = readText(root / "src/Gui/Command.h");
    const auto commandSource = readText(root / "src/Gui/Command.cpp");
    EXPECT_NE(commandHeader.find("AlterDoc = 1"), std::string::npos);
    EXPECT_NE(commandSource.find("eType & AlterDoc"), std::string::npos);
    EXPECT_NE(commandSource.find("isAllowedAlterDocument"), std::string::npos);
    EXPECT_EQ(commandSource.find("takeover"), std::string::npos);
}

}  // namespace
