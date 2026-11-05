// SPDX-License-Identifier: LGPL-2.1-or-later

#include <gtest/gtest.h>

#include <Python.h>

#include <atomic>
#include <chrono>
#include <future>
#include <thread>

#include "Base/Interpreter.h"

using namespace std::chrono_literals;

/**
 * @brief Test fixture for PyGILStateLocker and the GUI GIL handoff (M6).
 *
 * The test thread holds the GIL for the whole test, like a Python thread that runs bytecode.
 */
class PyGILStateLockerTest: public ::testing::Test
{
protected:
    void SetUp() override
    {
        Py_Initialize();
        gilState = PyGILState_Ensure();
    }

    void TearDown() override
    {
        PyGILState_Release(gilState);
    }

private:
    PyGILState_STATE gilState {};
};

/**
 * Reserves the handoff from a thread of its own, as the GUI thread does; the reserving thread
 * itself never yields. The thread stays alive until the reservation ends, so that no new thread
 * reuses its id.
 */
class HandoffReservation
{
public:
    HandoffReservation()
    {
        std::promise<void> reserved;
        auto isReserved = reserved.get_future();
        _thread = std::thread([reserved = std::move(reserved), done = _done.get_future()]() mutable {
            Base::GilGuiHandoff::reserve();
            reserved.set_value();
            done.wait();
        });
        isReserved.wait();
    }

    ~HandoffReservation()
    {
        Base::GilGuiHandoff::release();
        _done.set_value();
        _thread.join();
    }

    HandoffReservation(const HandoffReservation&) = delete;
    HandoffReservation& operator=(const HandoffReservation&) = delete;

private:
    std::promise<void> _done;
    std::thread _thread;
};

TEST_F(PyGILStateLockerTest, threadHoldingTheGilDoesNotYieldToTheHandoff)  // NOLINT
{
    // Arrange: the GUI thread has reserved the handoff and waits for the GIL this thread holds.
    HandoffReservation reservation;
    std::promise<void> locked;
    std::atomic<bool> releasedByWatchdog {false};
    std::thread watchdog([&releasedByWatchdog, done = locked.get_future()] {
        if (done.wait_for(5s) == std::future_status::timeout) {
            releasedByWatchdog = true;
            Base::GilGuiHandoff::release();
        }
    });

    // Act
    {
        Base::PyGILStateLocker locker;
        locked.set_value();
    }
    watchdog.join();

    // Assert: waiting would never end, because the GUI thread needs the GIL to release the handoff.
    EXPECT_FALSE(releasedByWatchdog);
}

TEST_F(PyGILStateLockerTest, threadWithoutTheGilWaitsForTheHandoff)  // NOLINT
{
    // Arrange
    HandoffReservation reservation;
    PyThreadState* testThreadState = PyEval_SaveThread();
    std::atomic<bool> locked {false};

    // Act
    std::thread worker([&locked] {
        Base::PyGILStateLocker locker;
        locked = true;
    });
    std::this_thread::sleep_for(100ms);
    const bool lockedWhileReserved = locked;
    Base::GilGuiHandoff::release();
    worker.join();
    PyEval_RestoreThread(testThreadState);

    // Assert
    EXPECT_FALSE(lockedWhileReserved);
    EXPECT_TRUE(locked);
}
