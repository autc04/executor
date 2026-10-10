#include "basilisk.h"
#include "plain.h"

using namespace Executor;

bool BasiliskItemFactory::isHidden(const fs::directory_entry& e)
{
    if(e.path().filename().string() == ".rsrc")
        return true;
    else if(e.path().filename().string() == ".finf")
        return true;
    return false;
}


ItemPtr BasiliskItemFactory::createItemForDirEntry(ItemCache& itemcache, CNID parID, CNID cnid,
    const fs::directory_entry& e, mac_string_view macname)
{
    if(fs::is_regular_file(e.path()))
    {
        fs::path rsrc = e.path().parent_path() / ".rsrc" / e.path().filename();
        fs::path finf = e.path().parent_path() / ".finf" / e.path().filename();
        
        if(fs::is_regular_file(rsrc) || fs::is_regular_file(finf))
        {
            auto item = std::make_shared<FileItem>(itemcache, parID, cnid, e.path(), macname);
            item->setRep(std::make_shared<BasiliskRep>(*item));
            return item;
        }
    }
    return nullptr;
}

void BasiliskItemFactory::createFile(const fs::path& newPath)
{
    fs::path parentPath = newPath.parent_path();
    fs::path fn = newPath.filename();

    fs::ofstream(parentPath / fn);

    fs::create_directory(parentPath / ".rsrc");
    fs::create_directory(parentPath / ".finf");

    fs::ofstream(parentPath / ".rsrc" / fn, std::ios::binary);
    FInfo info = {};
    fs::ofstream(parentPath / ".finf" / fn, std::ios::binary).write((char*)&info, sizeof(info));
}

std::unique_ptr<OpenFile> BasiliskRep::open(int8_t permission)
{
    return std::make_unique<PlainDataFork>(item_.path(), permission);
}
std::unique_ptr<OpenFile> BasiliskRep::openRF(int8_t permission)
{
    fs::path rsrc = item_.path().parent_path() / ".rsrc" / item_.path().filename();
    return std::make_unique<PlainDataFork>(rsrc, permission);
}

void BasiliskRep::getInfo(ItemInfo& info)
{
    fs::path finf = item_.path().parent_path() / ".finf" / item_.path().filename();

    fs::ifstream(finf, std::ios::binary).read((char*)&info, sizeof(info.file));
}

void BasiliskRep::setInfo(const ItemInfo& info)
{
    fs::path finf = item_.path().parent_path() / ".finf" / item_.path().filename();

    fs::ofstream(finf, std::ios::binary).write((char*)&info, sizeof(info.file));
}

void BasiliskRep::remove()
{
    fs::remove(item_.path());
    fs::path rsrc = item_.path().parent_path() / ".rsrc" / item_.path().filename();
    fs::path finf = item_.path().parent_path() / ".finf" / item_.path().filename();
    boost::system::error_code ec;
    fs::remove(rsrc, ec);
    fs::remove(finf, ec);
}

void BasiliskRep::move(const fs::path& newPath)
{
    fs::path rsrcOld = item_.path().parent_path() / ".rsrc" / item_.path().filename();
    fs::path finfOld = item_.path().parent_path() / ".finf" / item_.path().filename();
    fs::path rsrcNew = newPath.parent_path() / ".rsrc" / newPath.filename();
    fs::path finfNew = newPath.parent_path() / ".finf" / newPath.filename();

    fs::rename(item_.path(), newPath);

    boost::system::error_code ec;
    fs::rename(rsrcOld, rsrcNew, ec);
    fs::rename(finfOld, finfNew, ec);
}
