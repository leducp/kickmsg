// macOS uses POSIX shared memory (same as Linux).
// shm_open / ftruncate / mmap are available on all supported macOS versions.
#include "kickmsg/os/SharedMemory.h"

#include <cerrno>
#include <fcntl.h>
#include <stdexcept>
#include <string>
#include <system_error>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>

namespace kickmsg
{
    static void throw_system_error(char const* context)
    {
        throw std::system_error(errno, std::system_category(), context);
    }

    SharedMemory::SharedMemory(SharedMemory&& other) noexcept
        : size_{other.size_}
        , address_{other.address_}
        , fd_{other.fd_}
    {
        other.size_    = 0;
        other.address_ = nullptr;
        other.fd_      = INVALID_SHM_HANDLE;
    }

    SharedMemory& SharedMemory::operator=(SharedMemory&& other) noexcept
    {
        if (this != &other)
        {
            close();
            size_    = other.size_;
            address_ = other.address_;
            fd_      = other.fd_;
            other.size_    = 0;
            other.address_ = nullptr;
            other.fd_      = INVALID_SHM_HANDLE;
        }
        return *this;
    }

    SharedMemory::~SharedMemory()
    {
        close();
    }

    void SharedMemory::map(std::size_t size)
    {
        if (::ftruncate(fd_, static_cast<off_t>(size)) < 0)
        {
            ::close(fd_);
            fd_ = INVALID_SHM_HANDLE;
            throw_system_error("SharedMemory: ftruncate()");
        }

        address_ = ::mmap(nullptr, size, PROT_READ | PROT_WRITE, MAP_SHARED, fd_, 0);
        if (address_ == MAP_FAILED)
        {
            address_ = nullptr;
            ::close(fd_);
            fd_ = INVALID_SHM_HANDLE;
            throw_system_error("SharedMemory: mmap()");
        }

        size_ = size;
    }

    void SharedMemory::create(std::string const& name, std::size_t size)
    {
        // macOS quirk: shm_open returns EINVAL when passed O_TRUNC on an
        // existing SHM object — Linux accepts it but Darwin rejects it.
        // This matters on the create_or_open() fast path, where try_create
        // first opens the object with O_CREAT|O_EXCL and closes the fd,
        // then this function reopens it to size and map.  Calling
        // shm_unlink() first makes create() idempotent from the caller's
        // point of view and sidesteps the O_TRUNC incompatibility: the
        // subsequent shm_open sees a name that either didn't exist or
        // was just detached, and then ftruncate(size) is always the
        // first sizing call on the fresh object (which macOS also
        // requires — a SHM object can only be ftruncated once).
        ::shm_unlink(name.c_str());
        fd_ = ::shm_open(name.c_str(), O_RDWR | O_CREAT | O_EXCL, 0666);
        if (fd_ < 0)
        {
            throw_system_error("SharedMemory: shm_open(create)");
        }
        map(size);
    }

    bool SharedMemory::try_create(std::string const& name, std::size_t size)
    {
        // Keep the fd and do the full setup (ftruncate + mmap) inline.
        // We must NOT close the fd and call create() — create() would
        // shm_unlink the name (to sidestep Darwin's O_TRUNC quirk) and
        // recreate a different object, racing any concurrent caller that
        // observed the original name between our close and create's CAS.
        fd_ = ::shm_open(name.c_str(), O_RDWR | O_CREAT | O_EXCL, 0666);
        if (fd_ < 0)
        {
            if (errno == EEXIST)
            {
                fd_ = INVALID_SHM_HANDLE;
                return false;
            }
            throw_system_error("SharedMemory: shm_open(try_create)");
        }
        map(size);
        return true;
    }

    void SharedMemory::open(std::string const& name)
    {
        fd_ = ::shm_open(name.c_str(), O_RDWR, 0);
        if (fd_ < 0)
        {
            throw_system_error("SharedMemory: shm_open(open)");
        }

        struct stat st{};
        if (::fstat(fd_, &st) < 0)
        {
            ::close(fd_);
            fd_ = INVALID_SHM_HANDLE;
            throw_system_error("SharedMemory: fstat()");
        }

        size_ = static_cast<std::size_t>(st.st_size);
        address_ = ::mmap(nullptr, size_, PROT_READ | PROT_WRITE, MAP_SHARED, fd_, 0);
        if (address_ == MAP_FAILED)
        {
            address_ = nullptr;
            ::close(fd_);
            fd_ = INVALID_SHM_HANDLE;
            throw_system_error("SharedMemory: mmap()");
        }
    }

    void SharedMemory::close()
    {
        if (address_ != nullptr)
        {
            ::munmap(address_, size_);
            address_ = nullptr;
        }
        if (fd_ != INVALID_SHM_HANDLE)
        {
            ::close(fd_);
            fd_ = INVALID_SHM_HANDLE;
        }
        size_ = 0;
    }

    void SharedMemory::unlink(std::string const& name)
    {
        ::shm_unlink(name.c_str());
    }
}
