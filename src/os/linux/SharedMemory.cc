#include "kickmsg/os/SharedMemory.h"

#include <cerrno>
#include <fcntl.h>
#include <string>
#include <system_error>
#include <sys/mman.h>

namespace kickmsg
{
    void SharedMemory::create(std::string const& name, std::size_t size)
    {
        // Unlink + exclusive-create, never O_TRUNC: truncation zeroes an
        // object live peers still have mapped; orphaning leaves them intact.
        // The sequence is single-creator only (two concurrent callers could
        // both unlink) -- concurrent creators go through try_create.
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
