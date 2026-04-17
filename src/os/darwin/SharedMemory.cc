// macOS-specific SharedMemory::create().  Other methods live in
// src/os/posix/SharedMemory.cc.
#include "kickmsg/os/SharedMemory.h"

#include <cerrno>
#include <fcntl.h>
#include <stdexcept>
#include <string>
#include <system_error>
#include <sys/mman.h>

namespace kickmsg
{
    void SharedMemory::create(std::string const& name, std::size_t size)
    {
        // Darwin's shm_open(O_CREAT|O_TRUNC) returns EINVAL on an existing
        // object, and ftruncate can only be called once per object.  Unlink
        // first, then exclusive-create, to sidestep both.
        ::shm_unlink(name.c_str());
        fd_ = ::shm_open(name.c_str(), O_RDWR | O_CREAT | O_EXCL, 0666);
        if (fd_ < 0)
        {
            throw std::system_error(errno, std::system_category(),
                                    "SharedMemory: shm_open(create)");
        }
        map(size);
    }
}
