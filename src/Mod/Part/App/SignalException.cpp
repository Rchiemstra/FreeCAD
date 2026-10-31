// SPDX-License-Identifier: LGPL-2.1-or-later

/***************************************************************************
 *   Copyright (c) 2025 Werner Mayer <wmayer[at]users.sourceforge.net>     *
 *                                                                         *
 *   This file is part of FreeCAD.                                         *
 *                                                                         *
 *   FreeCAD is free software: you can redistribute it and/or modify it    *
 *   under the terms of the GNU Lesser General Public License as           *
 *   published by the Free Software Foundation, either version 2.1 of the  *
 *   License, or (at your option) any later version.                       *
 *                                                                         *
 *   FreeCAD is distributed in the hope that it will be useful, but        *
 *   WITHOUT ANY WARRANTY; without even the implied warranty of            *
 *   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the GNU      *
 *   Lesser General Public License for more details.                       *
 *                                                                         *
 *   You should have received a copy of the GNU Lesser General Public      *
 *   License along with FreeCAD. If not, see                               *
 *   <https://www.gnu.org/licenses/>.                                      *
 *                                                                         *
 **************************************************************************/

#include "SignalException.h"
#include <FCConfig.h>
#if defined(__GNUC__) && defined(FC_OS_LINUX)
# include <array>
# include <mutex>
# include <boost/stacktrace.hpp>
# include <stdexcept>
# include <iostream>
# include <csignal>
# include <OSD.hxx>
# include <OSD_WhoAmI.hxx>
# include <OSD_SIGHUP.hxx>
# include <OSD_SIGINT.hxx>
# include <OSD_SIGQUIT.hxx>
# include <OSD_SIGILL.hxx>
# include <OSD_SIGKILL.hxx>
# include <OSD_SIGBUS.hxx>
# include <OSD_SIGSEGV.hxx>
# include <OSD_SIGSYS.hxx>
# include <Standard.hxx>
# include <Standard_NumericError.hxx>
# include <Standard_ErrorHandler.hxx>
# include <Standard_Assert.hxx>
# include <Standard_Version.hxx>
// OCCT 8 removed NewInstance/Jump in favor of Standard_ErrorHandler::Abort
# if OCC_VERSION_HEX >= 0x080000
#  define OSD_SIGNAL_THROW(Type, msg) Standard_ErrorHandler::Abort(Type(msg))
# else
#  define OSD_SIGNAL_THROW(Type, msg) Type::NewInstance(msg)->Jump()
# endif
#endif

using namespace Part;

// NOLINTBEGIN(concurrency-mt-unsafe, cppcoreguidelines-pro-type-member-init)
#if defined(__GNUC__) && defined(FC_OS_LINUX)
static OSD_SignalMode OSD_WasSetSignal = OSD_SignalMode_AsIs;  // NOLINT

static void SegvHandler(const int theSignal, siginfo_t* theSigInfo, const Standard_Address /*theContext*/)
{
    std::cerr << "\nStacktrace:" << std::endl;
    std::cerr << "SIGSEGV signal raised: " << theSignal << std::endl;
    std::cerr << boost::stacktrace::stacktrace() << std::endl;
    std::cerr << "\n" << std::endl;
    if (theSigInfo != nullptr) {
        sigset_t set;
        sigemptyset(&set);
        sigaddset(&set, SIGSEGV);
        sigprocmask(SIG_UNBLOCK, &set, nullptr);
        OSD_SIGNAL_THROW(OSD_SIGSEGV, "SIGSEGV 'segmentation violation' detected.");
    }
    exit(SIGSEGV);
}

static void throw_exc(const int theSignal, siginfo_t* /*theSigInfo*/, const Standard_Address /*theContext*/)
{
    struct sigaction oldact;
    struct sigaction act;
    // re-install the signal
    if (!sigaction(theSignal, nullptr, &oldact)) {
        if (sigaction(theSignal, &oldact, &act)) {
            perror("sigaction");
        }
    }
    else {
        perror("sigaction");
    }

    sigset_t set;
    sigemptyset(&set);
    switch (theSignal) {
        case SIGHUP:
            OSD_SIGNAL_THROW(OSD_SIGHUP, "SIGHUP 'hangup' detected.");
            exit(SIGHUP);
            break;
        case SIGINT:
            // For safe handling of Control-C as stop event, arm a variable but do not
            // generate longjump (we are out of context anyway)
            OSD_SIGNAL_THROW(OSD_SIGINT, "SIGINT 'interrupt' detected.");
            exit(SIGINT);
            break;
        case SIGQUIT:
            OSD_SIGNAL_THROW(OSD_SIGQUIT, "SIGQUIT 'quit' detected.");
            exit(SIGQUIT);
            break;
        case SIGILL:
            OSD_SIGNAL_THROW(OSD_SIGILL, "SIGILL 'illegal instruction' detected.");
            exit(SIGILL);
            break;
        case SIGKILL:
            OSD_SIGNAL_THROW(OSD_SIGKILL, "SIGKILL 'kill' detected.");
            exit(SIGKILL);
            break;
        case SIGBUS:
            sigaddset(&set, SIGBUS);
            sigprocmask(SIG_UNBLOCK, &set, nullptr);
            OSD_SIGNAL_THROW(OSD_SIGBUS, "SIGBUS 'bus error' detected.");
            exit(SIGBUS);
            break;
        case SIGSEGV:
            OSD_SIGNAL_THROW(OSD_SIGSEGV, "SIGSEGV 'segmentation violation' detected.");
            exit(SIGSEGV);
            break;
# ifdef SIGSYS
        case SIGSYS:
            OSD_SIGNAL_THROW(OSD_SIGSYS, "SIGSYS 'bad argument to system call' detected.");
            exit(SIGSYS);
            break;
# endif
        case SIGFPE:
            sigaddset(&set, SIGFPE);
            sigprocmask(SIG_UNBLOCK, &set, nullptr);
            OSD::SetFloatingSignal(Standard_True);
            OSD_SIGNAL_THROW(Standard_NumericError, "SIGFPE Arithmetic exception detected");
            break;
        default:
            break;
    }
}

// Handlers are process-wide. FCBRepAlgoAPI_BooleanOperation::Build, which constructs this guard,
// is a public Part entry point and can overlap the document execution lane's thread. Only the
// outermost live guard switches modes. A mutex, not an atomic counter, covers that swap: the saved
// sigaction array and the sigaction() calls have to be published together with the nest count. The
// Boolean runs outside the lock. OCC handlers must not take the mutex; locking from a signal
// handler is not async-signal-safe.
constexpr static int kNbSignals = 8;
constexpr static std::array<int, kNbSignals> kSignalTypes
    = {SIGFPE, SIGHUP, SIGINT, SIGQUIT, SIGILL, SIGBUS, SIGSYS, SIGSEGV};
static std::mutex signalMutex;
static int nestCount = 0;

// Same save/restore as Base::SignalException: sigaction(sig, nullptr, &saved) on the way in,
// sigaction(sig, &saved, nullptr) on the way out. glibc ORs SA_RESTORER into every install (its
// return trampoline). That bit has no user-visible effect, so a round-trip may set it on a
// disposition that did not have it. Matching the kernel record would need an rt_sigaction layout
// per architecture, which is not worth it.
static std::array<struct sigaction, kNbSignals> savedActions {};
static std::array<bool, kNbSignals> savedActionValid {};

static int captureAction(int signo, std::size_t index)
{
    const int retcode = sigaction(signo, nullptr, &savedActions[index]);
    savedActionValid[index] = retcode == 0;
    return retcode;
}

static int restoreAction(std::size_t index)
{
    const int retcode = sigaction(kSignalTypes[index], &savedActions[index], nullptr);
    if (retcode == 0) {
        savedActionValid[index] = false;
    }
    return retcode;
}

static void setSignal(OSD_SignalMode theSignalMode)
{
    OSD_WasSetSignal = theSignalMode;
    if (theSignalMode == OSD_SignalMode_AsIs) {
        return;  // nothing to be done with signal handlers
    }

    // Caller holds signalMutex. Unset puts back the actions captured when this nest went 0 -> 1,
    // including handler, flags (SA_SIGINFO / SA_ONSTACK) and mask. It must not install SIG_DFL or
    // Base::SystemHandler::installSegfaultHandler(), which drops the crash reporter.
    if (theSignalMode == OSD_SignalMode_Unset) {
        for (std::size_t index = 0; index < kSignalTypes.size(); ++index) {
            if (!savedActionValid[index]) {
                continue;
            }
            const int retcode = restoreAction(index);
            Standard_ASSERT(
                retcode == 0,
                "sigaction() failed",
                std::cout << "OSD::SetSignal(): sigaction() failed to restore "
                          << kSignalTypes[index] << std::endl
            );
        }
        return;
    }

    // Prepare signal descriptors
    struct sigaction anActSet;
    struct sigaction anActOld;
    sigemptyset(&anActSet.sa_mask);
    sigemptyset(&anActOld.sa_mask);
# ifdef SA_RESTART
    anActSet.sa_flags = anActOld.sa_flags = SA_RESTART;
# else
    anActSet.sa_flags = anActOld.sa_flags = 0;
# endif
# ifdef SA_SIGINFO
    anActSet.sa_flags = anActSet.sa_flags | SA_SIGINFO;
    anActSet.sa_sigaction = throw_exc;
# else
    anActSet.sa_handler = throw_exc;
# endif

    // SIGSEGV stays last and keeps its own handler. Other signals use throw_exc.
    for (std::size_t index = 0; index < kSignalTypes.size(); ++index) {
        const int aSignalType = kSignalTypes[index];
        if (aSignalType == SIGSEGV) {
# ifdef SA_SIGINFO
            anActSet.sa_sigaction = /*(void(*)(int, siginfo_t *, void*))*/ SegvHandler;
# else
            anActSet.sa_handler = /*(SIG_PFV)*/ SegvHandler;
# endif
        }
        else {
# ifdef SA_SIGINFO
            anActSet.sa_sigaction = throw_exc;
# else
            anActSet.sa_handler = throw_exc;
# endif
        }

        // set handler according to specified mode and current handler
        int retcode = -1;
        if (theSignalMode == OSD_SignalMode_Set) {
            // Capture first. captureAction does not install, so a failed install leaves the
            // previous handler in place and the snapshot unused.
            retcode = captureAction(aSignalType, index);
            if (retcode == 0) {
                struct sigaction ignoredOld {};
                retcode = sigaction(aSignalType, &anActSet, &ignoredOld);
                if (retcode != 0) {
                    savedActionValid[index] = false;
                }
            }
        }
        else if (theSignalMode == OSD_SignalMode_SetUnhandled) {
            retcode = sigaction(aSignalType, &anActSet, &anActOld);
            if (retcode == 0 && anActOld.sa_handler != SIG_DFL) {
                struct sigaction anActOld2;
                sigemptyset(&anActOld2.sa_mask);
                retcode = sigaction(aSignalType, &anActOld, &anActOld2);
            }
        }
        Standard_ASSERT(
            retcode == 0,
            "sigaction() failed",
            std::cout << "OSD::SetSignal(): sigaction() failed for " << aSignalType << std::endl
        );
    }
}
#endif

// ----------------------------------------------------------------------------

SignalException::SignalException()
{
#if defined(__GNUC__) && defined(FC_OS_LINUX)
    const std::lock_guard<std::mutex> lock(signalMutex);
    if (nestCount == 0) {
        setSignal(OSD_SignalMode_Set);
    }
    ++nestCount;
    enabled = true;
#endif
}

SignalException::~SignalException()
{
#if defined(__GNUC__) && defined(FC_OS_LINUX)
    if (!enabled) {
        return;
    }
    const std::lock_guard<std::mutex> lock(signalMutex);
    enabled = false;
    if (nestCount <= 0) {
        return;
    }
    --nestCount;
    if (nestCount == 0) {
        setSignal(OSD_SignalMode_Unset);
    }
#endif
}
// NOLINTEND(concurrency-mt-unsafe, cppcoreguidelines-pro-type-member-init)
