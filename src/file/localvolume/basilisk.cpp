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


FileRepPtr BasiliskItemFactory::createRepForFile(ItemCache& itemcache, FileItem& item,
    const fs::directory_entry& e, ProbeTrace& trace)
{
    fs::path rsrc = e.path().parent_path() / ".rsrc" / e.path().filename();
    fs::path finf = e.path().parent_path() / ".finf" / e.path().filename();

    bool hasRsrc = fs::is_regular_file(rsrc);
    bool hasFinf = fs::is_regular_file(finf);

    if(hasRsrc)
        trace.push_back(RegularFileObservation { rsrc });
    else
        trace.push_back(NoRegularFileObservation { rsrc });
    if(hasFinf)
        trace.push_back(RegularFileObservation { finf });
    else
        trace.push_back(NoRegularFileObservation { finf });

    if(hasRsrc || hasFinf)
        return std::make_shared<BasiliskRep>(item);
    return {};
}

void BasiliskItemFactory::createFile(const fs::path& newPath)
{
    fs::path parentPath = newPath.parent_path();
    fs::path fn = newPath.filename();

    fs::ofstream(parentPath / fn);

    fs::create_directory(parentPath / ".rsrc");
    fs::create_directory(parentPath / ".finf");

    fs::ofstream rsrc(parentPath / ".rsrc" / fn, std::ios::binary);
    if(!rsrc)
        throw OSErrorException(ioErr);

    FInfo info = {};
    fs::ofstream finf(parentPath / ".finf" / fn, std::ios::binary);
    finf.write((char*)&info, sizeof(info));
    if(!finf)
        throw OSErrorException(ioErr);
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

    // The base checks the destination before renaming the primary, so a
    // collision leaves the companions untouched.
    FileRep::move(newPath);

    // A companion at a free primary name is orphaned; overwrite it.
    boost::system::error_code ec;
    fs::rename(rsrcOld, rsrcNew, ec);
    fs::rename(finfOld, finfNew, ec);
}
