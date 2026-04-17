// Parts of SharedMemory that are identical on Linux and macOS.
// Platform-specific create() lives in src/os/{linux,darwin}/SharedMemory.cc.
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
    namespace
    {
        [[noreturn]] void throw_system_error(char const* context)
        {
            throw std::system_error(errno, std::system_category(), context);
        }
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

    bool SharedMemory::try_create(std::string const& name, std::size_t size)
    {
        // Keep the fd and do the full setup (ftruncate + mmap) inline.
        // SharedRegion::create_or_open consumes the resulting mapping
        // directly — there's no reason to close here and re-enter create(),
        // and the old round-trip pattern caused a subtle race on Darwin.
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
        if (not try_open(name))
        {
            throw_system_error("SharedMemory: shm_open(open)");
        }
    }

    bool SharedMemory::try_open(std::string const& name)
    {
        fd_ = ::shm_open(name.c_str(), O_RDWR, 0);
        if (fd_ < 0)
        {
            if (errno == ENOENT)
            {
                fd_ = INVALID_SHM_HANDLE;
                return false;
            }
            throw_system_error("SharedMemory: shm_open(try_open)");
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
        return true;
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
