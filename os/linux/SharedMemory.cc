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
            fd_ = -1;
            throw_system_error("SharedMemory.cc: ftruncate()");
        }

        address_ = ::mmap(nullptr, size, PROT_READ | PROT_WRITE, MAP_SHARED, fd_, 0);
        if (address_ == MAP_FAILED)
        {
            address_ = nullptr;
            ::close(fd_);
            fd_ = -1;
            throw_system_error("SharedMemory.cc: mmap()");
        }

        size_ = size;
    }

    void SharedMemory::create(std::string const& name, std::size_t size)
    {
        fd_ = ::shm_open(name.c_str(), O_RDWR | O_CREAT | O_TRUNC, 0666);
        if (fd_ < 0)
        {
            throw_system_error("SharedMemory.cc: shm_open(create)");
        }
        map(size);
    }

    bool SharedMemory::try_create(std::string const& name, std::size_t size)
    {
        int fd = ::shm_open(name.c_str(), O_RDWR | O_CREAT | O_EXCL, 0666);
        if (fd < 0)
        {
            if (errno == EEXIST)
            {
                return false;
            }
            throw_system_error("SharedMemory.cc: shm_open(try_create)");
        }
        ::close(fd);
        create(name, size);
        return true;
    }

    void SharedMemory::open(std::string const& name)
    {
        fd_ = ::shm_open(name.c_str(), O_RDWR, 0);
        if (fd_ < 0)
        {
            throw_system_error("SharedMemory.cc: shm_open(open)");
        }

        struct stat st{};
        if (::fstat(fd_, &st) < 0)
        {
            ::close(fd_);
            fd_ = -1;
            throw_system_error("SharedMemory.cc: fstat()");
        }

        size_ = static_cast<std::size_t>(st.st_size);
        address_ = ::mmap(nullptr, size_, PROT_READ | PROT_WRITE, MAP_SHARED, fd_, 0);
        if (address_ == MAP_FAILED)
        {
            address_ = nullptr;
            ::close(fd_);
            fd_ = -1;
            throw_system_error("SharedMemory.cc: mmap()");
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
            fd_ = -1;
        }
        size_ = 0;
    }

    void SharedMemory::unlink(std::string const& name)
    {
        ::shm_unlink(name.c_str());
    }
}
