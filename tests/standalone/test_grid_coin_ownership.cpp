// SPDX-License-Identifier: LGPL-2.1-or-later
// Repeated rejected/failed grid builds must not leak node refs.
// Compile: g++ -std=c++20 -fsanitize=address -fno-sanitize-recover=all

#include <cassert>
#include <iostream>

#include "Mod/Part/Gui/ViewProviderGridExtensionInternal.h"

namespace
{

int gLive = 0;

struct FakeNode
{
    int refs {0};
    FakeNode()
    {
        ++gLive;
    }
    void ref()
    {
        ++refs;
    }
    void unref()
    {
        --refs;
        if (refs <= 0) {
            delete this;
        }
    }

private:
    ~FakeNode()
    {
        --gLive;
    }
};

void rejectedBuild()
{
    const bool allocated = PartGui::GridExtensionInternal::allocateGridNodesAfterPlan(
        false,
        [] {
            FakeNode* leaked = new FakeNode();
            (void)leaked;
        }
    );
    assert(!allocated);
}

void failedAfterAlloc()
{
    const bool allocated = PartGui::GridExtensionInternal::allocateGridNodesAfterPlan(
        true,
        [] {
            PartGui::GridExtensionInternal::ScopedCoinRef<FakeNode> grid(new FakeNode());
            PartGui::GridExtensionInternal::ScopedCoinRef<FakeNode> vts(new FakeNode());
            (void)grid;
            (void)vts;
        }
    );
    assert(allocated);
}

}  // namespace

int main()
{
    for (int i = 0; i < 64; ++i) {
        rejectedBuild();
        failedAfterAlloc();
    }
    assert(gLive == 0);
    std::cout << "grid_coin_ownership ok live=" << gLive << "\n";
    return 0;
}
