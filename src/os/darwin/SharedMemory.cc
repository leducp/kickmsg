#include "kickmsg/os/SharedMemory.h"

#include <cerrno>
#include <fcntl.h>
#include <system_error>
#include <sys/mman.h>

namespace kickmsg
{
    void SharedMemory::create(std::string const& name, std::size_t size)
    {
        // Unlink + exclusive-create: Darwin returns EINVAL on
        // O_CREAT|O_TRUNC of an existing object and allows only one
        // ftruncate per object.  Single-creator only (two concurrent
        // callers could both unlink) -- concurrent creators go through
        // try_create.
        ::shm_unlink(name.c_str());
        fd_ = ::shm_open(name.c_str(), O_RDWR | O_CREAT | O_EXCL, kickmsg_shm_mode());
        if (fd_ < 0)
        {
            throw std::system_error(errno, std::system_category(),
                                    "SharedMemory: shm_open(create) '" + name + "'");
        }
        map(size);
    }
}
