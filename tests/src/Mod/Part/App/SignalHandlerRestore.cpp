// SPDX-License-Identifier: LGPL-2.1-or-later

#include <gtest/gtest.h>

#include <FCConfig.h>

#if defined(__GNUC__) && defined(FC_OS_LINUX)
# include <array>
# include <csignal>
# include <BRepPrimAPI_MakeBox.hxx>
# include <gp_Pnt.hxx>
# include "Mod/Part/App/FCBRepAlgoAPI_Cut.h"
# include "Mod/Part/App/SignalException.h"
#endif

#include <src/App/InitApplication.h>

// Part::SignalException swaps these handlers on Linux GCC/Clang. SIGABRT is
// intentionally absent: the crash reporter owns it and setSignal does not.
#if defined(__GNUC__) && defined(FC_OS_LINUX)
namespace
{

constexpr std::array<int, 8> kSignals {
    SIGFPE,
    SIGHUP,
    SIGINT,
    SIGQUIT,
    SIGILL,
    SIGBUS,
    SIGSYS,
    SIGSEGV,
};

void knownSignalHandler(int, siginfo_t*, void*)
{}

struct sigaction queryAction(int signo)
{
    struct sigaction action {};
    EXPECT_EQ(sigaction(signo, nullptr, &action), 0) << "sigaction query failed for " << signo;
    return action;
}

void installKnownHandler(int signo)
{
    struct sigaction action {};
    action.sa_sigaction = knownSignalHandler;
    action.sa_flags = SA_SIGINFO | SA_ONSTACK;
    sigemptyset(&action.sa_mask);
    // A bit the OCC installer does not set, so a mask restore is observable.
    sigaddset(&action.sa_mask, SIGUSR1);
    ASSERT_EQ(sigaction(signo, &action, nullptr), 0) << "sigaction install failed for " << signo;
}

// SA_RESTORER is Linux-specific. glibc sets it on every sigaction() install; mask it out so the
// comparison accepts that bit without requiring a kernel rt_sigaction layout.
int flagsForCompare(int flags)
{
#ifdef SA_RESTORER
    return flags & ~SA_RESTORER;
#else
    return flags;
#endif
}

testing::AssertionResult actionMatches(int signo, const struct sigaction& expected)
{
    struct sigaction actual {};
    if (sigaction(signo, nullptr, &actual) != 0) {
        return testing::AssertionFailure() << "sigaction query failed for signal " << signo;
    }
    const bool sameHandler = actual.sa_sigaction == expected.sa_sigaction;
    const int actualFlags = flagsForCompare(actual.sa_flags);
    const int expectedFlags = flagsForCompare(expected.sa_flags);
    const bool sameFlags = actualFlags == expectedFlags;
    // glibc's sigset_t is 128 bytes and sigaction() does not define the bytes past the
    // kernel's 64 signals, so compare membership instead of the whole object.
    bool sameMask = true;
    int mismatchedSignal = 0;
    for (int bit = 1; bit < NSIG; ++bit) {
        if (sigismember(&actual.sa_mask, bit) != sigismember(&expected.sa_mask, bit)) {
            sameMask = false;
            mismatchedSignal = bit;
            break;
        }
    }
    if (sameHandler && sameFlags && sameMask) {
        return testing::AssertionSuccess();
    }
    return testing::AssertionFailure()
        << "signal " << signo << " handler " << reinterpret_cast<void*>(actual.sa_sigaction)
        << " flags 0x" << std::hex << actual.sa_flags << " expected handler "
        << reinterpret_cast<void*>(expected.sa_sigaction) << " flags 0x" << expected.sa_flags
        << " masked 0x" << actualFlags << " expected 0x" << expectedFlags << std::dec
        << " mask=" << sameMask << " mismatchedSignal=" << mismatchedSignal;
}

class RestoreProcessHandlers
{
public:
    RestoreProcessHandlers()
    {
        for (std::size_t index = 0; index < kSignals.size(); ++index) {
            saved[index] = queryAction(kSignals[index]);
        }
    }

    ~RestoreProcessHandlers()
    {
        for (std::size_t index = 0; index < kSignals.size(); ++index) {
            sigaction(kSignals[index], &saved[index], nullptr);
        }
    }

    RestoreProcessHandlers(const RestoreProcessHandlers&) = delete;
    RestoreProcessHandlers& operator=(const RestoreProcessHandlers&) = delete;

private:
    std::array<struct sigaction, kSignals.size()> saved {};
};

void runBoxCut()
{
    const TopoDS_Shape base = BRepPrimAPI_MakeBox(10.0, 10.0, 10.0).Shape();
    const TopoDS_Shape tool = BRepPrimAPI_MakeBox(gp_Pnt(5.0, 0.0, 0.0), 10.0, 10.0, 10.0).Shape();
    FCBRepAlgoAPI_Cut cut(base, tool);
    EXPECT_TRUE(cut.IsDone());
}

std::array<struct sigaction, kSignals.size()> installAndSnapshot()
{
    std::array<struct sigaction, kSignals.size()> installed {};
    for (std::size_t index = 0; index < kSignals.size(); ++index) {
        installKnownHandler(kSignals[index]);
        // Read back the installed action. Flag checks below mask SA_RESTORER.
        installed[index] = queryAction(kSignals[index]);
        EXPECT_EQ(installed[index].sa_sigaction, knownSignalHandler);
        EXPECT_EQ(installed[index].sa_flags & SA_SIGINFO, SA_SIGINFO);
        EXPECT_EQ(installed[index].sa_flags & SA_ONSTACK, SA_ONSTACK);
    }
    return installed;
}

void expectRestored(const std::array<struct sigaction, kSignals.size()>& installed)
{
    for (std::size_t index = 0; index < kSignals.size(); ++index) {
        EXPECT_TRUE(actionMatches(kSignals[index], installed[index]));
    }
}

}  // namespace

class SignalHandlerRestoreTest: public ::testing::Test
{
protected:
    static void SetUpTestSuite()
    {
        tests::initApplication();
    }
};

TEST_F(SignalHandlerRestoreTest, booleanRestoresInstalledHandlers)
{
    [[maybe_unused]] const RestoreProcessHandlers restore;
    const auto installed = installAndSnapshot();

    runBoxCut();
    expectRestored(installed);

    runBoxCut();
    expectRestored(installed);
}

TEST_F(SignalHandlerRestoreTest, nestedGuardRestoresOnlyAfterOutermost)
{
    [[maybe_unused]] const RestoreProcessHandlers restore;
    const auto installed = installAndSnapshot();

    {
        Part::SignalException outer;
        for (int signo : kSignals) {
            const struct sigaction during = queryAction(signo);
            EXPECT_NE(during.sa_sigaction, knownSignalHandler) << signo;
        }
        {
            Part::SignalException inner;
        }
        for (int signo : kSignals) {
            const struct sigaction during = queryAction(signo);
            EXPECT_NE(during.sa_sigaction, knownSignalHandler) << signo;
        }
    }

    expectRestored(installed);
}
#else
TEST(SignalHandlerRestoreTest, booleanRestoresInstalledHandlers)
{
    GTEST_SKIP() << "Part signal-handler swap is compiled only for Linux GCC/Clang";
}
#endif
