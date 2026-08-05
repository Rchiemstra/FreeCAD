/***************************************************************************
 *   Copyright (c) 2010 Werner Mayer <wmayer[at]users.sourceforge.net>     *
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


#pragma once

#include <Base/Persistence.h>
#include <map>
#include <memory>
#include <vector>
#include <fastsignals/signal.h>

#include <App/Document.h>

namespace zipios
{
class ZipInputStream;
}
namespace App
{
class Document;
class DocumentObject;
}  // namespace App

namespace Gui
{
class Document;
class GuiExport MergeDocuments:
    public Base::Persistence,
    public App::Internal::CollaborationImportReplay
{
public:
    explicit MergeDocuments(App::Document* doc);
    ~MergeDocuments() override;
    unsigned int getMemSize() const override;
    std::vector<App::DocumentObject*> importObjects(std::istream&);
    void importObject(const std::vector<App::DocumentObject*>& o, Base::XMLReader& r);
    void exportObject(const std::vector<App::DocumentObject*>& o, Base::Writer& w);
    void Save(Base::Writer& w) const override;
    void Restore(Base::XMLReader& r) override;
    void SaveDocFile(Base::Writer& w) const override;
    void RestoreDocFile(Base::Reader& r) override;

private:
    std::vector<App::DocumentObject*> importObjectsDeferred(
        std::istream&,
        const std::shared_ptr<MergeDocuments>& owner);
    Base::XMLReader& reader() noexcept override;
    void restoreImportedModelFiles() override;
    void restoreImportedFiles(
        const std::vector<App::DocumentObject*>& importedObjects) override;

    std::unique_ptr<std::istream> deferredInput;
    std::unique_ptr<zipios::ZipInputStream> stream;
    std::shared_ptr<Base::XMLReader> deferredReader;
    std::string deferredArchive;
    std::size_t precommitFileCount {0};
    App::Document* appdoc;
    Gui::Document* document;
    std::vector<App::DocumentObject*> objects;
    std::map<std::string, std::string> nameMap;
    using Connection = fastsignals::connection;
    Connection connectExport;
    Connection connectImport;
};

}  // namespace Gui
