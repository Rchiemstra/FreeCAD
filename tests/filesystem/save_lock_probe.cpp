// SPDX-License-Identifier: LGPL-2.1-or-later

/***************************************************************************
 *   Copyright (c) 2026                                                    *
 *                                                                         *
 *   This file is part of the FreeCAD CAx development system.              *
 *                                                                         *
 *   This library is free software; you can redistribute it and/or         *
 *   modify it under the terms of the GNU Library General Public           *
 *   License as published by the Free Software Foundation; either          *
 *   version 2 of the License, or (at your option) any later version.      *
 *                                                                         *
 *   This library  is distributed in the hope that it will be useful,      *
 *   but WITHOUT ANY WARRANTY; without even the implied warranty of        *
 *   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the         *
 *   GNU Library General Public License for more details.                  *
 *                                                                         *
 *   You should have received a copy of the GNU Library General Public     *
 *   License along with this library; see the file COPYING.LIB. If not,    *
 *   write to the Free Software Foundation, Inc., 59 Temple Place,         *
 *   Suite 330, Boston, MA  02111-1307, USA                                *
 *                                                                         *
 ***************************************************************************/

/**
 * Cross-process helper for the save-lock anchor gate.
 *
 * `DocumentFileLock` combines a process-local `std::timed_mutex` with a kernel
 * byte-range lock (`LockFileEx` on Windows, `fcntl(F_SETLK)` on POSIX) taken on
 * a persistent zero-byte `<document>.FreeCAD-save.lock` sibling. Only the kernel
 * half arbitrates between processes, and POSIX record locks are per-process by
 * definition: two locks taken in one process never conflict. A single-process
 * test therefore cannot observe the cross-process contract at all, which is why
 * this helper exists as a separate executable.
 *
 * It speaks one command per line on stdin and answers one line on stdout,
 * flushed immediately. The driver blocks on those answers, so ownership
 * transitions are observed rather than waited out with sleeps.
 *
 *   LOCK      -> LOCKED | BLOCKED | ALREADY | ERROR <detail>
 *   RELEASE   -> RELEASED | NOTHELD
 *   HELD      -> HELD yes | HELD no
 *   PID       -> PID <pid>
 *   EXIT      -> BYE, then exits 0 after unwinding normally
 *   ABANDON   -> ABANDONING, then dies immediately while still holding the lock
 *
 * ABANDON deliberately skips every destructor so the gate can prove the kernel
 * releases ownership on process death, rather than on a tidy unlock() call.
 */

#include <cstdlib>
#include <exception>
#include <iostream>
#include <memory>
#include <string>

#ifdef _WIN32
# include <process.h>
#else
# include <unistd.h>
#endif

#include <App/DocumentFileWriter.h>

namespace
{

void emit(const std::string& line)
{
    std::cout << line << '\n';
    std::cout.flush();
}

long currentProcessId()
{
#ifdef _WIN32
    return static_cast<long>(_getpid());
#else
    return static_cast<long>(::getpid());
#endif
}

}  // namespace

int main(int argc, char** argv)
{
    if (argc < 2) {
        emit("ERROR usage: save_lock_probe <destination>");
        return 2;
    }
    const std::string destination = argv[1];

    std::unique_ptr<App::Internal::DocumentFileLock> held;

    emit("READY");

    std::string command;
    while (std::getline(std::cin, command)) {
        while (!command.empty() && (command.back() == '\r' || command.back() == '\n')) {
            command.pop_back();
        }

        if (command == "LOCK") {
            if (held) {
                emit("ALREADY");
                continue;
            }
            try {
                // Non-blocking: a contended lock must report BLOCKED promptly
                // instead of stalling the driver behind a timeout.
                auto candidate = std::make_unique<App::Internal::DocumentFileLock>(destination, 0);
                if (candidate->isLocked()) {
                    held = std::move(candidate);
                    emit("LOCKED");
                }
                else {
                    emit("BLOCKED");
                }
            }
            catch (const std::exception& error) {
                emit(std::string("ERROR ") + error.what());
            }
            catch (...) {
                emit("ERROR unknown failure while acquiring the lock");
            }
        }
        else if (command == "RELEASE") {
            if (!held) {
                emit("NOTHELD");
                continue;
            }
            held.reset();
            emit("RELEASED");
        }
        else if (command == "HELD") {
            emit(held && held->isLocked() ? "HELD yes" : "HELD no");
        }
        else if (command == "PID") {
            emit("PID " + std::to_string(currentProcessId()));
        }
        else if (command == "EXIT") {
            emit("BYE");
            return 0;
        }
        else if (command == "ABANDON") {
            emit("ABANDONING");
            // No unwinding, no unlock(), no destructor: only the kernel can
            // release what this process still owns.
            std::_Exit(9);
        }
        else if (!command.empty()) {
            emit("ERROR unknown command: " + command);
        }
    }

    return 0;
}
