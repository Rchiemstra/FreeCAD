// SPDX-License-Identifier: LGPL-2.1-or-later

/***************************************************************************************************
 *                                                                                                 *
 *   Copyright (c) 2022 Zheng, Lei (realthunder) <realthunder.dev@gmail.com>                       *
 *   Copyright (c) 2023 FreeCAD Project Association                                                *
 *                                                                                                 *
 *   This file is part of FreeCAD.                                                                 *
 *                                                                                                 *
 *   FreeCAD is free software: you can redistribute it and/or modify it under the terms of the     *
 *   GNU Lesser General Public License as published by the Free Software Foundation, either        *
 *   version 2.1 of the License, or (at your option) any later version.                            *
 *                                                                                                 *
 *   FreeCAD is distributed in the hope that it will be useful, but WITHOUT ANY WARRANTY;          *
 *   without even the implied warranty of MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.     *
 *   See the GNU Lesser General Public License for more details.                                   *
 *                                                                                                 *
 *   You should have received a copy of the GNU Lesser General Public License along with           *
 *   FreeCAD. If not, see <https://www.gnu.org/licenses/>.                                         *
 *                                                                                                 *
 **************************************************************************************************/


#include <QCryptographicHash>
#include <QHash>
#include <algorithm>
#include <deque>
#include <functional>
#include <limits>
#include <unordered_set>
#include <unordered_map>
#include <map>
#include <string>

#include <Base/Console.h>
#include <Base/Reader.h>
#include <Base/Stream.h>
#include <Base/Writer.h>

#include <boost/algorithm/string/classification.hpp>
#include <boost/algorithm/string/split.hpp>
#include <boost/bimap.hpp>
#include <boost/bimap/set_of.hpp>
#include <boost/bimap/unordered_set_of.hpp>
#include <boost/io/ios_state.hpp>
#include <boost/iostreams/stream.hpp>

#include "MappedElement.h"
#include "StringHasher.h"
#include "StringHasherPy.h"
#include "StringIDPy.h"


FC_LOG_LEVEL_INIT("App", true, true)

namespace bio = boost::iostreams;
using namespace App;

///////////////////////////////////////////////////////////

struct StringIDHasher
{
    std::size_t operator()(const StringID* sid) const
    {
        if (!sid) {
            return 0;
        }
        return qHash(sid->data(), qHash(sid->postfix()));
    }

    bool operator()(const StringID* IDa, const StringID* IDb) const
    {
        if (IDa == IDb) {
            return true;
        }
        if (!IDa || !IDb) {
            return false;
        }
        return IDa->data() == IDb->data() && IDa->postfix() == IDb->postfix();
    }
};

using HashMapBase =
    boost::bimap<boost::bimaps::unordered_set_of<StringID*, StringIDHasher, StringIDHasher>,
                 boost::bimaps::set_of<long>>;

class StringHasher::HashMap: public HashMapBase
{
public:
    bool SaveAll = false;
    int Threshold = 0;
};

///////////////////////////////////////////////////////////

TYPESYSTEM_SOURCE_ABSTRACT(App::StringID, Base::BaseClass)

StringID::~StringID()
{
    if (_hasher) {
        _hasher->_hashes->right.erase(_id);
    }
}

PyObject* StringID::getPyObject()
{
    return new StringIDPy(this);
}

PyObject* StringID::getPyObjectWithIndex(int index)
{
    auto res = new StringIDPy(this);
    res->_index = index;
    return res;
}

std::string StringID::toString(int index) const
{
    std::ostringstream ss;
    ss << '#' << std::hex << value();
    if (index != 0) {
        ss << ':' << index;
    }
    return ss.str();
}

StringID::IndexID StringID::fromString(const char* name, bool eof, int size)
{
    IndexID res {};
    res.id = 0;
    res.index = 0;
    if (!name) {
        res.id = -1;
        return res;
    }
    if (size < 0) {
        size = static_cast<int>(std::strlen(name));
    }
    bio::stream<bio::array_source> iss(name, size);
    char sep = 0;
    char sep2 = 0;
    iss >> sep >> std::hex >> res.id >> sep2 >> res.index;
    if ((eof && !iss.eof()) || sep != '#' || (sep2 != 0 && sep2 != ':')) {
        res.id = -1;
        return res;
    }
    return res;
}

std::string StringID::dataToText(int index) const
{
    if (isHashed() || isBinary()) {
        return _data.toBase64().constData();
    }

    std::string res(_data.constData());
    if (index != 0) {
        res += std::to_string(index);
    }
    if (_postfix.size() != 0) {
        res += _postfix.constData();
    }
    return res;
}

void StringID::mark() const
{
    if (isMarked()) {
        return;
    }
    _flags.setFlag(Flag::Marked);
    for (auto& sid : _sids) {
        sid.deref().mark();
    }
}

///////////////////////////////////////////////////////////

TYPESYSTEM_SOURCE(App::StringHasher, Base::Persistence)

StringHasher::StringHasher()
    : _hashes(new HashMap)
{}

StringHasher::~StringHasher()
{
    clear();
}

void StringHasher::setSaveAll(bool enable)
{
    if (_hashes->SaveAll == enable) {
        return;
    }
    _hashes->SaveAll = enable;
    compact();
}

void StringHasher::compact()
{
    if (_hashes->SaveAll) {
        return;
    }

    // Make a list of all the table entries that have only a single reference and are not marked
    // "persistent"
    std::deque<StringIDRef> pendings;
    for (auto& hasher : _hashes->right) {
        if (!hasher.second->isPersistent() && hasher.second->getRefCount() == 1) {
            pendings.emplace_back(hasher.second);
        }
    }

    // Recursively remove the unused StringIDs
    while (!pendings.empty()) {
        StringIDRef sid = pendings.front();
        pendings.pop_front();
        // Try to erase the map entry for this StringID
        if (_hashes->right.erase(sid.value()) == 0U) {
            continue;  // If nothing was erased, there's nothing more to do
        }
        sid._sid->_hasher = nullptr;
        sid._sid->unref();
        for (auto& hasher : sid._sid->_sids) {
            if (hasher._sid->_hasher == this && !hasher._sid->isPersistent()
                && hasher._sid->getRefCount() == 2) {
                // If the related StringID also uses this hasher, is not marked persistent, and has
                // a current reference count of 2 (which will be its hasher reference and its entry
                // in the related SIDs list), then prep it for removal as well.
                pendings.push_back(hasher);
            }
        }
    }
}

bool StringHasher::getSaveAll() const
{
    return _hashes->SaveAll;
}

void StringHasher::setThreshold(int threshold)
{
    _hashes->Threshold = threshold;
}

int StringHasher::getThreshold() const
{
    return _hashes->Threshold;
}

long StringHasher::lastID() const
{
    long mapLast = 0;
    if (!_hashes->right.empty()) {
        auto it = _hashes->right.end();
        --it;
        mapLast = it->first;
    }
    return std::max(mapLast, _reservedHighWater);
}

void StringHasher::reserveHighWater(uint64_t highWaterId)
{
    if (highWaterId > static_cast<uint64_t>(std::numeric_limits<long>::max())) {
        return;
    }
    const long hw = static_cast<long>(highWaterId);
    if (hw > _reservedHighWater) {
        _reservedHighWater = hw;
    }
}

StringIDRef StringHasher::getID(const char* text, int len, bool hashable)
{
    if (len < 0) {
        len = static_cast<int>(strlen(text));
    }
    return getID(QByteArray::fromRawData(text, len), hashable ? Option::Hashable : Option::None);
}

StringIDRef StringHasher::getID(const QByteArray& data, Options options)
{
    bool binary = options.testFlag(Option::Binary);
    bool hashable = options.testFlag(Option::Hashable);
    bool nocopy = options.testFlag(Option::NoCopy);

    bool hashed = hashable && _hashes->Threshold > 0 && (int)data.size() > _hashes->Threshold;

    StringID dataID;
    if (hashed) {
        QCryptographicHash hasher(QCryptographicHash::Sha1);
        hasher.addData(data);
        dataID._data = hasher.result();
    }
    else {
        dataID._data = data;
    }

    auto it = _hashes->left.find(&dataID);
    if (it != _hashes->left.end()) {
        return {it->first};
    }

    if (!hashed && !nocopy) {
        // if not hashed, make a deep copy of the data
        dataID._data = QByteArray(data.constData(), data.size());
    }

    StringID::Flags flags(StringID::Flag::None);
    if (binary) {
        flags.setFlag(StringID::Flag::Binary);
    }
    if (hashed) {
        flags.setFlag(StringID::Flag::Hashed);
    }
    StringIDRef sid(new StringID(lastID() + 1, dataID._data, flags));
    return {insert(sid)};
}

StringIDRef StringHasher::getID(const Data::MappedName& name, const QVector<StringIDRef>& sids)
{
    StringID tempID;
    tempID._postfix = name.postfixBytes();

    Data::IndexedName indexed;
    if (tempID._postfix.size() != 0) {
        // Only check for IndexedName if there is postfix, because of the way
        // we restore the StringID. See StringHasher::saveStream/restoreStreamNew()
        indexed = Data::IndexedName(name.dataBytes());
    }
    if (indexed) {
        // If this is an IndexedName, then _data only stores the base part of the name, without the
        // integer index
        tempID._data =
            QByteArray::fromRawData(indexed.getType(), static_cast<int>(strlen(indexed.getType())));
    }
    else {
        // Store the entire name in _data, but temporarily reuse the existing memory
        tempID._data = name.dataBytes();
    }

    // Check to see if there is already an entry in the hash table for this StringID
    auto it = _hashes->left.find(&tempID);
    if (it != _hashes->left.end()) {
        auto res = StringIDRef(it->first);
        if (indexed) {
            res._index = indexed.getIndex();
        }
        return res;
    }

    if (!indexed && name.isRaw()) {
        // Make a copy of the memory if we didn't do so earlier
        tempID._data = QByteArray(name.dataBytes().constData(), name.dataBytes().size());
    }

    // If the postfix is not already encoded, use getID to encode it:
    StringIDRef postfixRef;
    if ((tempID._postfix.size() != 0) && tempID._postfix.indexOf("#") < 0) {
        postfixRef = getID(tempID._postfix);
        postfixRef.toBytes(tempID._postfix);
    }

    // If _data is an IndexedName, use getID to encode it:
    StringIDRef indexRef;
    if (indexed) {
        indexRef = getID(tempID._data);
    }

    // The real StringID object that we are going to insert
    StringIDRef newStringIDRef(new StringID(lastID() + 1, tempID._data));
    StringID& newStringID = *newStringIDRef._sid;
    if (tempID._postfix.size() != 0) {
        newStringID._flags.setFlag(StringID::Flag::Postfixed);
        newStringID._postfix = tempID._postfix;
    }

    // Count the related SIDs that use this hasher
    int numSIDs = 0;
    for (const auto& relatedID : sids) {
        if (relatedID && relatedID._sid->_hasher == this) {
            ++numSIDs;
        }
    }

    int numAddedSIDs = (postfixRef ? 1 : 0) + (indexRef ? 1 : 0);
    if (numSIDs == sids.size() && !postfixRef && !indexRef) {
        // The simplest case: just copy the whole list
        newStringID._sids = sids;
    }
    else {
        // Put the added SIDs at the front of the SID list
        newStringID._sids.reserve(numSIDs + numAddedSIDs);
        if (postfixRef) {
            newStringID._flags.setFlag(StringID::Flag::PostfixEncoded);
            newStringID._sids.push_back(postfixRef);
        }
        if (indexRef) {
            newStringID._flags.setFlag(StringID::Flag::Indexed);
            newStringID._sids.push_back(indexRef);
        }
        // Append the sids from the input list whose hasher is this one
        for (const auto& relatedID : sids) {
            if (relatedID && relatedID._sid->_hasher == this) {
                newStringID._sids.push_back(relatedID);
            }
        }
    }

    // If the number of related IDs is larger than some threshold (hardcoded to 10 right now), then
    // remove any duplicates (ignoring the new SIDs we may have just added)
    const int relatedIDSizeThreshold {10};
    if (newStringID._sids.size() > relatedIDSizeThreshold) {
        std::sort(newStringID._sids.begin() + numAddedSIDs, newStringID._sids.end());
        newStringID._sids.erase(
            std::unique(newStringID._sids.begin() + numAddedSIDs, newStringID._sids.end()),
            newStringID._sids.end());
    }

    // If the new StringID has a postfix, but is not indexed, see if the data string itself
    // contains an index.
    if ((newStringID._postfix.size() != 0) && !indexed) {
        // Use the fromString function to parse the new StringID's data field for a possible index
        StringID::IndexID res = StringID::fromString(newStringID._data);
        if (res.id > 0) {  // If the data had an index
            if (res.index != 0) {
                indexed.setIndex(res.index);
                newStringID._data.resize(newStringID._data.lastIndexOf(':') + 1);
            }
            int offset = newStringID.isPostfixEncoded() ? 1 : 0;
            // Search for the SID with that index
            for (int i = offset; i < newStringID._sids.size(); ++i) {
                if (newStringID._sids[i].value() == res.id) {
                    if (i != offset) {
                        // If this SID is not already the first element in sids, move it there by
                        // swapping it with whatever WAS there
                        std::swap(newStringID._sids[offset], newStringID._sids[i]);
                    }
                    if (res.index != 0) {
                        newStringID._flags.setFlag(StringID::Flag::PrefixIDIndex);
                    }
                    else {
                        newStringID._flags.setFlag(StringID::Flag::PrefixID);
                    }
                    break;
                }
            }
        }
    }

    return {insert(newStringIDRef), indexed.getIndex()};
}

StringIDRef StringHasher::getID(long id, int index) const
{
    if (id <= 0) {
        return {};
    }
    auto it = _hashes->right.find(id);
    if (it == _hashes->right.end()) {
        return {};
    }
    StringIDRef res(it->second);
    res._index = index;
    return res;
}

void StringHasher::setPersistenceFileName(const char* filename) const
{
    if (!filename) {
        filename = "";
    }
    _filename = filename;
}

const std::string& StringHasher::getPersistenceFileName() const
{
    return _filename;
}

void StringHasher::Save(Base::Writer& writer) const
{

    std::size_t count = _hashes->SaveAll ? _hashes->size() : this->count();

    writer.Stream() << writer.ind() << "<StringHasher saveall=\"" << _hashes->SaveAll
                    << "\" threshold=\"" << _hashes->Threshold << "\"";

    if (count == 0U) {
        writer.Stream() << " count=\"0\"></StringHasher>\n";
        return;
    }

    writer.Stream() << " count=\"0\" new=\"1\"/>\n";

    writer.Stream() << writer.ind() << "<StringHasher2 ";
    if (!_filename.empty()) {
        writer.Stream() << " file=\"" << writer.addFile((_filename + ".txt").c_str(), this)
                        << "\"/>\n";
        return;
    }

    writer.Stream() << " count=\"" << count << "\">\n";
    saveStream(writer.beginCharStream() << '\n');
    writer.endCharStream() << '\n';
    writer.Stream() << writer.ind() << "</StringHasher2>\n";
}

void StringHasher::SaveDocFile(Base::Writer& writer) const
{
    std::size_t count = _hashes->SaveAll ? this->size() : this->count();
    writer.Stream() << "StringTableStart v1 " << count << '\n';
    saveStream(writer.Stream());
}

void StringHasher::saveStream(std::ostream& stream) const
{
    Base::TextOutputStream textStreamWrapper(stream);
    boost::io::ios_flags_saver ifs(stream);
    stream << std::hex;

    long anchor = 0;
    const StringID* last = nullptr;
    long lastID = 0;
    bool relative = false;

    for (auto& hasher : _hashes->right) {
        auto& d = *hasher.second;
        long id = d._id;
        if (!_hashes->SaveAll && !d.isMarked() && !d.isPersistent()) {
            continue;
        }

        // We use relative coding to save space. But in order to have some
        // minimum protection against corruption, write an absolute value every
        // once a while.
        relative = (id - anchor) < 1000;
        if (relative) {
            stream << '-' << id - lastID;
        }
        else {
            anchor = id;
            stream << id;
        }
        lastID = id;

        int offset = d.isPostfixEncoded() ? 1 : 0;

        StringID::IndexID prefixID {};
        prefixID.id = 0;
        prefixID.index = 0;
        if (d.isPrefixID()) {
            assert(d._sids.size() > offset);
            prefixID.id = d._sids[offset].value();
        }
        else if (d.isPrefixIDIndex()) {
            prefixID = StringID::fromString(d._data);
            assert(d._sids.size() > offset && d._sids[offset].value() == prefixID.id);
        }

        auto flags = d._flags;
        flags.setFlag(StringID::Flag::Marked, false);
        stream << '.' << flags.toUnderlyingType();

        int position = 0;
        if (!relative) {
            for (; position < d._sids.size(); ++position) {
                stream << '.' << d._sids[position].value();
            }
        }
        else {
            if (last) {
                for (; position < d._sids.size() && position < last->_sids.size(); ++position) {
                    long m = last->_sids[position].value();
                    long n = d._sids[position].value();
                    if (n < m) {
                        stream << ".-" << m - n;
                    }
                    else {
                        stream << '.' << n - m;
                    }
                }
            }
            for (; position < d._sids.size(); ++position) {
                stream << '.' << id - d._sids[position].value();
            }
        }

        last = &d;

        // Having postfix means it is a geometry element name, which
        // guarantees to be a single line without space. So it is safe to
        // store in raw stream.
        if (d.isPostfixed()) {
            if (!d.isPrefixIDIndex() && !d.isIndexed() && !d.isPrefixID()) {
                stream << ' ' << d._data.constData();
            }

            if (!d.isPostfixEncoded()) {
                stream << ' ' << d._postfix.constData();
            }
            stream << '\n';
        }
        else {
            // Hashed/binary payloads are raw octets and must round-trip as base64;
            // restoreStreamNew() always decodes them with fromBase64(). Plaintext may
            // contain spaces/newlines, so we rely on TextOutputStream for both cases.
            stream << ' ';
            if (d.isHashed() || d.isBinary()) {
                const QByteArray encoded = d._data.toBase64();
                textStreamWrapper << std::string(encoded.constData(), encoded.size());
            }
            else {
                textStreamWrapper << std::string(d._data.constData(), d._data.size());
            }
        }
    }
}

void StringHasher::RestoreDocFile(Base::Reader& reader)
{
    std::string marker;
    std::string ver;
    reader >> marker;
    std::size_t count = 0;
    _hashes->clear();
    if (marker == "StringTableStart") {
        reader >> ver >> count;
        if (ver != "v1") {
            FC_WARN("Unknown string table format");
        }
        restoreStreamNew(reader, count);
        return;
    }
    reader >> count;
    restoreStream(reader, count);
}

void StringHasher::restoreStreamNew(std::istream& stream, std::size_t count)
{
    Base::TextInputStream asciiStream(stream);
    _hashes->clear();
    std::string content;
    boost::io::ios_flags_saver ifs(stream);
    stream >> std::hex;
    std::vector<std::string> tokens;
    long lastid = 0;
    const StringID* last = nullptr;

    std::string tmp;

    for (uint32_t i = 0; i < count; ++i) {
        if (!(stream >> tmp)) {
            FC_THROWM(Base::RuntimeError, "Invalid string table");
        }

        tokens.clear();
        boost::split(tokens, tmp, boost::is_any_of("."));
        if (tokens.size() < 2) {
            FC_THROWM(Base::RuntimeError, "Invalid string table");
        }

        long id = 0;
        bool relative = false;
        if (tokens[0][0] == '-') {
            relative = true;
            id = lastid + strtol(tokens[0].c_str() + 1, nullptr, 16);
        }
        else {
            id = strtol(tokens[0].c_str(), nullptr, 16);
        }

        lastid = id;

        unsigned long flag = strtol(tokens[1].c_str(), nullptr, 16);
        StringIDRef sid(new StringID(id, QByteArray(), static_cast<StringID::Flag>(flag)));

        StringID& d = *sid._sid;
        d._sids.reserve(tokens.size() - 2);

        int j = 2;
        if (relative && last) {
            for (; j < (int)tokens.size() && j - 2 < last->_sids.size(); ++j) {
                long m = last->_sids[j - 2].value();
                long n;
                if (tokens[j][0] == '-') {
                    n = -strtol(&tokens[j][1], nullptr, 16);
                }
                else {
                    n = strtol(&tokens[j][0], nullptr, 16);
                }
                StringIDRef sid = getID(m + n);
                if (!sid) {
                    FC_THROWM(Base::RuntimeError, "Invalid string id reference");
                }
                d._sids.push_back(sid);
            }
        }
        for (; j < (int)tokens.size(); ++j) {
            long n = strtol(tokens[j].data(), nullptr, 16);
            StringIDRef sid = getID(relative ? id - n : n);
            if (!sid) {
                FC_THROWM(Base::RuntimeError, "Invalid string id reference");
            }
            d._sids.push_back(sid);
        }

        if (!d.isPostfixed()) {
            asciiStream >> content;
            if (d.isHashed() || d.isBinary()) {
                d._data = QByteArray::fromBase64(content.c_str());
            }
            else {
                d._data = content.c_str();
            }
        }
        else {
            int offset = 0;
            if (d.isPostfixEncoded()) {
                offset = 1;
                if (d._sids.empty()) {
                    FC_THROWM(Base::RuntimeError, "Missing string postfix");
                }
                d._postfix = d._sids[0]._sid->_data;
            }
            if (d.isIndexed()) {
                if (d._sids.size() <= offset) {
                    FC_THROWM(Base::RuntimeError, "Missing string prefix");
                }
                d._data = d._sids[offset]._sid->_data;
            }
            else if (d.isPrefixID() || d.isPrefixIDIndex()) {
                if (d._sids.size() <= offset) {
                    FC_THROWM(Base::RuntimeError, "Missing string prefix id");
                }
                d._data = d._sids[offset]._sid->toString(0).c_str();
                if (d.isPrefixIDIndex()) {
                    d._data += ":";
                }
            }
            else {
                stream >> content;
                d._data = content.c_str();
            }
            if (!d.isPostfixEncoded()) {
                stream >> content;
                d._postfix = content.c_str();
            }
        }

        last = insert(sid);
    }
}

StringID* StringHasher::insert(const StringIDRef& sid)
{
    assert(sid && sid._sid->_hasher == nullptr);
    auto& hasher = *sid._sid;
    hasher._hasher = this;
    hasher.ref();
    auto res = _hashes->right.insert(_hashes->right.end(),
                                     HashMap::right_map::value_type(sid.value(), &hasher));
    if (res->second != &hasher) {
        hasher._hasher = nullptr;
        hasher.unref();
    }
    return res->second;
}

void StringHasher::restoreStream(std::istream& stream, std::size_t count)
{
    _hashes->clear();
    std::string content;
    for (uint32_t i = 0; i < count; ++i) {
        int32_t id = 0;
        uint8_t type = 0;
        stream >> id >> type >> content;
        StringIDRef sid = new StringID(id, QByteArray(), static_cast<StringID::Flag>(type));
        if (sid.isHashed() || sid.isBinary()) {
            sid._sid->_data = QByteArray::fromBase64(content.c_str());
        }
        else {
            sid._sid->_data = QByteArray(content.c_str());
        }
        insert(sid);
    }
}

void StringHasher::clear()
{
    for (auto& hasher : _hashes->right) {
        hasher.second->_hasher = nullptr;
        hasher.second->unref();
    }
    _hashes->clear();
    _reservedHighWater = 0;
}

size_t StringHasher::size() const
{
    return _hashes->size();
}

size_t StringHasher::count() const
{
    size_t count = 0;
    for (auto& hasher : _hashes->right) {
        if (hasher.second->isMarked() || hasher.second->isPersistent()) {
            ++count;
        }
    }
    return count;
}

void StringHasher::Restore(Base::XMLReader& reader)
{
    clear();
    reader.readElement("StringHasher");
    _hashes->SaveAll = reader.getAttribute<long>("saveall") != 0L;
    _hashes->Threshold = reader.getAttribute<int>("threshold");

    bool newTag = false;
    if (reader.hasAttribute("new") && reader.getAttribute<bool>("new")) {
        reader.readElement("StringHasher2");
        newTag = true;
    }

    if (reader.hasAttribute("file")) {
        const char* file = reader.getAttribute<const char*>("file");
        if (*file != '\0') {
            reader.addFile(file, this);
        }
        return;
    }

    std::size_t count = reader.getAttribute<unsigned long>("count");
    if (newTag) {
        try {
            restoreStreamNew(reader.beginCharStream(), count);
        }
        catch (const Base::Exception& e) {
            e.reportException();
            FC_ERR("Failed to restore string table: full-document recompute strongly recommended.");
        }
        reader.readEndElement("StringHasher2");
        return;
    }
    if ((count != 0U) && reader.FileVersion > 1) {
        restoreStream(reader.beginCharStream(), count);
    }
    else {
        for (std::size_t i = 0; i < count; ++i) {
            reader.readElement("Item");
            StringIDRef sid;
            long id = reader.getAttribute<long>("id");
            bool hashed = reader.hasAttribute("hash");
            if (hashed || reader.hasAttribute("data")) {
                const char* value =
                    hashed ? reader.getAttribute<const char*>("hash") : reader.getAttribute<const char*>("data");
                sid = new StringID(id, QByteArray::fromBase64(value), StringID::Flag::Hashed);
            }
            else {
                sid = new StringID(id, QByteArray(reader.getAttribute<const char*>("text")));
            }
            insert(sid);
        }
    }
    reader.readEndElement("StringHasher");
}

unsigned int StringHasher::getMemSize() const
{
    return (_hashes->SaveAll ? size() : count()) * 10;
}

PyObject* StringHasher::getPyObject()
{
    return new StringHasherPy(this);
}

std::map<long, StringIDRef> StringHasher::getIDMap() const
{
    std::map<long, StringIDRef> ret;
    for (auto& hasher : _hashes->right) {
        ret.emplace_hint(ret.end(), hasher.first, StringIDRef(hasher.second));
    }
    return ret;
}

void StringHasher::clearMarks() const
{
    for (auto& hasher : _hashes->right) {
        hasher.second->_flags.setFlag(StringID::Flag::Marked, false);
    }
}

StringHasherClosure StringHasher::captureClosure(bool markedOnly) const
{
    StringHasherClosure closure;
    closure.highWaterId = static_cast<uint64_t>(lastID());
    closure.revision = _revision;
    closure.threshold = getThreshold();
    closure.entries.reserve(_hashes->size());

    for (auto& hasher : _hashes->right) {
        auto& d = *hasher.second;
        if (markedOnly && !d.isMarked() && !d.isPersistent()) {
            continue;
        }

        StringHasherClosureEntry entry;
        entry.id = d._id;
        auto flags = d._flags;
        flags.setFlag(StringID::Flag::Marked, false);
        entry.flags = flags.toUnderlyingType();
        entry.data = d._data;
        entry.postfix = d._postfix;
        entry.relatedIds.reserve(d._sids.size());
        for (const auto& sid : d._sids) {
            entry.relatedIds.push_back(sid.value());
        }
        closure.entries.push_back(std::move(entry));
    }
    return closure;
}

std::unordered_map<long, bool> StringHasher::snapshotMarks() const
{
    std::unordered_map<long, bool> marks;
    marks.reserve(_hashes->size());
    for (auto& hasher : _hashes->right) {
        marks.emplace(hasher.first, hasher.second->isMarked());
    }
    return marks;
}

void StringHasher::restoreMarks(const std::unordered_map<long, bool>& marks) const
{
    for (auto& hasher : _hashes->right) {
        auto it = marks.find(hasher.first);
        const bool marked = (it != marks.end()) ? it->second : false;
        hasher.second->_flags.setFlag(StringID::Flag::Marked, marked);
    }
}

StringHasherMergeResult StringHasher::materializeExactClosure(const StringHasherClosure& closure)
{
    StringHasherMergeResult result;
    if (size() != 0 || _revision != 0 || _reservedHighWater != 0) {
        result.errorCode = "HasherNotEmpty";
        result.errorMessage = "materializeExactClosure requires a fresh empty hasher";
        return result;
    }
    if (closure.threshold < 0) {
        result.errorCode = "InvalidThreshold";
        result.errorMessage = "Hasher threshold metadata must be non-negative";
        return result;
    }
    return commitExactClosure(closure, /*bootstrap=*/true, /*expectedRevision=*/0);
}

StringHasherMergeResult StringHasher::mergeExactClosure(const StringHasherClosure& closure,
                                                        uint64_t expectedRevision)
{
    StringHasherMergeResult result;
    if (closure.threshold < 0) {
        result.errorCode = "InvalidThreshold";
        result.errorMessage = "Hasher threshold metadata must be non-negative";
        return result;
    }
    // Canonical document merge: worker must not silently change hashing policy.
    if (closure.threshold != getThreshold()) {
        result.errorCode = "ThresholdMismatch";
        result.errorMessage = "Hasher threshold does not match the canonical document hasher";
        return result;
    }

    const uint64_t expected = (expectedRevision != 0) ? expectedRevision : _revision;
    if (expected != 0 && closure.revision != 0 && closure.revision != expected) {
        result.errorCode = "RevisionMismatch";
        result.errorMessage = "Hasher revision does not match expected document revision";
        return result;
    }
    return commitExactClosure(closure, /*bootstrap=*/false, expected);
}

StringHasherMergeResult StringHasher::commitExactClosure(const StringHasherClosure& closure,
                                                         bool bootstrap,
                                                         uint64_t /*expectedRevision*/)
{
    StringHasherMergeResult result;

    std::vector<const StringHasherClosureEntry*> ordered;
    ordered.reserve(closure.entries.size());
    for (const auto& entry : closure.entries) {
        ordered.push_back(&entry);
    }
    std::sort(ordered.begin(), ordered.end(), [](const StringHasherClosureEntry* a,
                                                 const StringHasherClosureEntry* b) {
        return a->id < b->id;
    });

    // Preflight uniqueness: numeric ID and value identity (data, postfix).
    std::unordered_set<long> seenIds;
    seenIds.reserve(ordered.size());
    struct ValueIdentity
    {
        QByteArray data;
        QByteArray postfix;
        bool operator==(const ValueIdentity& other) const
        {
            return data == other.data && postfix == other.postfix;
        }
    };
    struct ValueIdentityHash
    {
        size_t operator()(const ValueIdentity& value) const noexcept
        {
            // Length-prefixed mix so embedded NULs cannot alias distinct pairs.
            size_t h = std::hash<int>{}(value.data.size());
            h ^= std::hash<int>{}(value.postfix.size()) + 0x9e3779b9 + (h << 6) + (h >> 2);
            const auto mixBytes = [&h](const QByteArray& bytes) {
                for (int i = 0; i < bytes.size(); ++i) {
                    h ^= static_cast<unsigned char>(bytes.at(i))
                        + 0x9e3779b9 + (h << 6) + (h >> 2);
                }
            };
            mixBytes(value.data);
            mixBytes(value.postfix);
            return h;
        }
    };
    std::unordered_map<ValueIdentity, long, ValueIdentityHash> valueToId;
    valueToId.reserve(ordered.size());

    for (const auto* entryPtr : ordered) {
        const auto& entry = *entryPtr;
        if (entry.id <= 0) {
            result.errorCode = "NullStringId";
            result.errorMessage = "Closure contains a non-positive StringID";
            return result;
        }
        if (!seenIds.insert(entry.id).second) {
            result.errorCode = "DuplicateId";
            result.errorMessage = "Closure contains duplicate StringID values";
            return result;
        }
        if (closure.highWaterId != 0
            && static_cast<uint64_t>(entry.id) > closure.highWaterId) {
            result.errorCode = "IdAboveHighWater";
            result.errorMessage = "Closure contains a StringID above the authenticated high-water";
            return result;
        }
        const ValueIdentity key {entry.data, entry.postfix};
        auto vit = valueToId.find(key);
        if (vit == valueToId.end()) {
            valueToId.emplace(key, entry.id);
        }
        else if (vit->second != entry.id) {
            result.errorCode = "ValueCollision";
            result.errorMessage =
                "Closure assigns the same data/postfix to different StringID values";
            return result;
        }
    }

    std::unordered_set<long> pendingIds = seenIds;
    std::vector<const StringHasherClosureEntry*> toInsert;
    toInsert.reserve(ordered.size());

    for (const auto* entryPtr : ordered) {
        const auto& entry = *entryPtr;

        auto existing = getID(entry.id);
        if (existing) {
            auto flags = existing._sid->_flags;
            flags.setFlag(StringID::Flag::Marked, false);
            const auto existingFlags = flags.toUnderlyingType();
            bool relatedMatch =
                existing._sid->_sids.size() == static_cast<int>(entry.relatedIds.size());
            if (relatedMatch) {
                for (int i = 0; i < existing._sid->_sids.size(); ++i) {
                    if (existing._sid->_sids[i].value()
                        != entry.relatedIds[static_cast<size_t>(i)]) {
                        relatedMatch = false;
                        break;
                    }
                }
            }
            if (static_cast<uint32_t>(existingFlags) != entry.flags
                || existing._sid->_data != entry.data
                || existing._sid->_postfix != entry.postfix || !relatedMatch) {
                result.errorCode = "IdCollision";
                result.errorMessage = "Exact-ID hasher delta collided with a different value";
                return result;
            }
            continue;
        }

        // Value already owned by a different ID on the canonical hasher.
        {
            StringID probe(0, entry.data, static_cast<StringID::Flag>(entry.flags));
            probe._postfix = entry.postfix;
            auto lit = _hashes->left.find(&probe);
            if (lit != _hashes->left.end() && lit->first && lit->first->value() != entry.id) {
                result.errorCode = "ValueCollision";
                result.errorMessage =
                    "Exact-ID hasher delta reassigns an existing value under a different ID";
                return result;
            }
        }

        for (long relatedId : entry.relatedIds) {
            if (relatedId <= 0) {
                result.errorCode = "InvalidRelatedId";
                result.errorMessage = "Closure references a non-positive related StringID";
                return result;
            }
            if (getID(relatedId)) {
                continue;
            }
            if (relatedId < entry.id && pendingIds.count(relatedId) != 0U) {
                continue;
            }
            result.errorCode = "MissingRelatedId";
            result.errorMessage = "Closure references a related StringID that is not present";
            return result;
        }
        toInsert.push_back(entryPtr);
    }

    // Commit phase. Track mutations for rollback if insert fails unexpectedly.
    const int thresholdBefore = getThreshold();
    const uint64_t revisionBefore = _revision;
    const long highWaterBefore = _reservedHighWater;
    std::vector<long> insertedIds;
    insertedIds.reserve(toInsert.size());

    auto rollback = [&]() {
        for (long id : insertedIds) {
            auto it = _hashes->right.find(id);
            if (it == _hashes->right.end()) {
                continue;
            }
            StringID* sid = it->second;
            _hashes->right.erase(it);
            if (sid) {
                _hashes->left.erase(sid);
                sid->_hasher = nullptr;
                sid->unref();
            }
        }
        insertedIds.clear();
        _hashes->Threshold = thresholdBefore;
        _revision = revisionBefore;
        _reservedHighWater = highWaterBefore;
    };

    if (bootstrap) {
        _hashes->Threshold = closure.threshold;
    }

    for (const auto* entryPtr : toInsert) {
        const auto& entry = *entryPtr;
        QVector<StringIDRef> related;
        related.reserve(static_cast<int>(entry.relatedIds.size()));
        for (long relatedId : entry.relatedIds) {
            auto sid = getID(relatedId);
            if (!sid) {
                rollback();
                result.errorCode = "InsertFailed";
                result.errorMessage = "Related StringID disappeared during exact-ID commit";
                result.appendedCount = 0;
                return result;
            }
            related.push_back(sid);
        }

        StringIDRef sid(new StringID(entry.id, entry.data,
                                     static_cast<StringID::Flag>(entry.flags)));
        sid._sid->_postfix = entry.postfix;
        sid._sid->_sids = related;
        auto* inserted = insert(sid);
        if (!inserted || inserted != sid._sid) {
            rollback();
            result.errorCode = "InsertFailed";
            result.errorMessage = "Failed to insert exact-ID StringID into hasher";
            result.appendedCount = 0;
            return result;
        }
        insertedIds.push_back(entry.id);
        ++result.appendedCount;
    }

    if (closure.highWaterId != 0) {
        reserveHighWater(closure.highWaterId);
    }
    if (bootstrap && closure.revision != 0) {
        _revision = closure.revision;
    }
    result.success = true;
    return result;
}
