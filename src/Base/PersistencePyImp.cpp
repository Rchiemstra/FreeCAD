// SPDX-License-Identifier: LGPL-2.1-or-later

/***************************************************************************
 *   Copyright (c) 2007 Jürgen Riegel <juergen.riegel@web.de>              *
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

#include <cstdint>
#include <limits>
#include <memory>
#include <sstream>

#include <boost/iostreams/stream.hpp>

#include "Persistence.h"
#include "PyWrapParseTupleAndKeywords.h"
#include "Writer.h"

// generated out of Persistence.pyi
#include "PersistencePy.h"
#include "PersistencePy.cpp"

using namespace Base;

namespace
{
#if defined(FREECAD_PERSISTENCE_TEST_API)
using BufferAcquire = int (*)(PyObject*, Py_buffer*, int);
thread_local BufferAcquire bufferAcquireForTest = nullptr;
#endif

class ScopedPythonBuffer final
{
public:
    int acquire(PyObject* object, int flags)
    {
        int result;
#if defined(FREECAD_PERSISTENCE_TEST_API)
        if (bufferAcquireForTest) {
            result = bufferAcquireForTest(object, &view, flags);
        }
        else
#endif
        {
            result = PyObject_GetBuffer(object, &view, flags);
        }
        acquired = result == 0;
        return result;
    }
    ~ScopedPythonBuffer()
    {
        if (acquired) {
            PyObject* errorType = nullptr;
            PyObject* errorValue = nullptr;
            PyObject* errorTraceback = nullptr;
            const bool hadError = PyErr_Occurred() != nullptr;
            if (hadError) {
                PyErr_Fetch(&errorType, &errorValue, &errorTraceback);
            }
            PyBuffer_Release(&view);
            if (hadError) {
                PyErr_Restore(errorType, errorValue, errorTraceback);
            }
        }
    }
    Py_buffer view {};
private:
    bool acquired {false};
};

#if defined(FREECAD_PERSISTENCE_TEST_API)
using ByteArrayFactory = PyObject* (*)(Py_ssize_t);
using BeforeByteArrayRead = void (*)(PyObject*, std::istream&);
thread_local ByteArrayFactory byteArrayFactoryForTest = nullptr;
thread_local BeforeByteArrayRead beforeByteArrayReadForTest = nullptr;
#endif
}  // namespace

#if defined(FREECAD_PERSISTENCE_TEST_API)
namespace Base::Internal
{
BaseExport void setPersistenceContentByteArrayFactoryForTest(PyObject* (*factory)(Py_ssize_t))
{
    byteArrayFactoryForTest = factory;
}
BaseExport void setPersistenceContentBufferAcquireForTest(int (*hook)(PyObject*, Py_buffer*, int))
{
    bufferAcquireForTest = hook;
}
BaseExport void setPersistenceContentBeforeReadForTest(void (*hook)(PyObject*, std::istream&))
{
    beforeByteArrayReadForTest = hook;
}
}  // namespace Base::Internal
#endif


// returns a string which represent the object e.g. when printed in python
std::string PersistencePy::representation() const
{
    return {"<persistence object>"};
}


Py::String PersistencePy::getContent() const
{
    StringWriter writer;
    // force all objects to write pure XML without files
    writer.setForceXML(true);
    getPersistencePtr()->Save(writer);

    return {writer.getString()};
}

Py::Long PersistencePy::getMemSize() const
{
    return Py::Long((long)getPersistencePtr()->getMemSize());
}

PyObject* PersistencePy::dumpContent(PyObject* args, PyObject* kwds) const
{
    int compression = 3;
    static const std::array<const char*, 2> kwds_def {"Compression", nullptr};
    PyErr_Clear();
    if (!Wrapped_ParseTupleAndKeywords(args, kwds, "|i", kwds_def, &compression)) {
        return nullptr;
    }

    // setup the stream. the in flag is needed to make "read" work
    std::stringstream stream(
        std::stringstream::out | std::stringstream::in | std::stringstream::binary
    );
    try {
        getPersistencePtr()->dumpToStream(stream, compression);
    }
    catch (NotImplementedError&) {
        if (!PyErr_Occurred()) {
            PyErr_SetString(
                PyExc_NotImplementedError,
                "Dumping content of this object type is not implemented"
            );
        }
        return nullptr;
    }
    catch (...) {
        if (!PyErr_Occurred()) {
            PyErr_SetString(PyExc_IOError, "Unable to parse content into binary representation");
        }
        return nullptr;
    }

    // build the byte array with correct size
    if (!stream.seekp(0, std::stringstream::end)) {
        PyErr_SetString(PyExc_IOError, "Unable to find end of stream");
        return nullptr;
    }

    std::stringstream::pos_type offset = stream.tellp();
    if (!stream.seekg(0, std::stringstream::beg)) {
        PyErr_SetString(PyExc_IOError, "Unable to find begin of stream");
        return nullptr;
    }

    const auto size = static_cast<std::streamoff>(offset);
    if (size < 0 || static_cast<std::uintmax_t>(size)
            > static_cast<std::uintmax_t>(std::numeric_limits<Py_ssize_t>::max())
        || static_cast<std::uintmax_t>(size)
            > static_cast<std::uintmax_t>(std::numeric_limits<std::streamsize>::max())) {
        PyErr_SetString(PyExc_OverflowError, "Serialized content is too large");
        return nullptr;
    }
    PyObject* result = nullptr;
#if defined(FREECAD_PERSISTENCE_TEST_API)
    if (byteArrayFactoryForTest) {
        result = byteArrayFactoryForTest(static_cast<Py_ssize_t>(size));
    }
    else
#endif
    {
        result = PyByteArray_FromStringAndSize(nullptr, static_cast<Py_ssize_t>(size));
    }
    std::unique_ptr<PyObject, decltype(&Py_DecRef)> ba(result, &Py_DecRef);
    if (!ba) {
        return nullptr;
    }

    ScopedPythonBuffer buf;
    if (buf.acquire(ba.get(), PyBUF_WRITABLE) < 0) {
        return nullptr;
    }
    try {
#if defined(FREECAD_PERSISTENCE_TEST_API)
        if (beforeByteArrayReadForTest) {
            beforeByteArrayReadForTest(ba.get(), stream);
        }
#endif
        if (!stream.read(static_cast<char*>(buf.view.buf), static_cast<std::streamsize>(size))) {
            if (!PyErr_Occurred()) {
                PyErr_SetString(PyExc_IOError, "Error copying data into byte array");
            }
            return nullptr;
        }
    }
    catch (...) {
        if (!PyErr_Occurred()) {
            PyErr_SetString(PyExc_IOError, "Error copying data into byte array");
        }
        return nullptr;
    }

    return ba.release();
}

PyObject* PersistencePy::restoreContent(PyObject* args)
{
    PyObject* buffer = nullptr;
    if (!PyArg_ParseTuple(args, "O", &buffer)) {
        return nullptr;
    }

    // check if it really is a buffer
    if (!PyObject_CheckBuffer(buffer)) {
        PyErr_SetString(PyExc_TypeError, "Must be a buffer object");
        return nullptr;
    }

    ScopedPythonBuffer buf;
    if (buf.acquire(buffer, PyBUF_SIMPLE) < 0) {
        return nullptr;
    }

    if (!PyBuffer_IsContiguous(&buf.view, 'C')) {
        PyErr_SetString(PyExc_TypeError, "Buffer must be contiguous");
        return nullptr;
    }

    try {
        using Device = boost::iostreams::basic_array_source<char>;
        boost::iostreams::stream<Device> stream(static_cast<char*>(buf.view.buf), buf.view.len);
        getPersistencePtr()->restoreFromStream(stream);
    }
    catch (...) {
        if (!PyErr_Occurred()) {
            PyErr_SetString(PyExc_IOError, "Unable to restore content");
        }
        return nullptr;
    }

    Py_Return;
}

PyObject* PersistencePy::getCustomAttributes(const char*) const
{
    return nullptr;
}

int PersistencePy::setCustomAttributes(const char*, PyObject*)
{
    return 0;
}
