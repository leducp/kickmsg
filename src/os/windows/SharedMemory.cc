#include "kickmsg/os/SharedMemory.h"

#include <stdexcept>
#include <string>
#include <system_error>

namespace kickmsg
{
    static void throw_last_error(char const* context)
    {
        throw std::system_error(
            static_cast<int>(GetLastError()), std::system_category(), context);
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
        address_ = MapViewOfFile(fd_, FILE_MAP_ALL_ACCESS, 0, 0, size);
        if (address_ == nullptr)
        {
            CloseHandle(fd_);
            fd_ = INVALID_SHM_HANDLE;
            throw_last_error("SharedMemory: MapViewOfFile()");
        }
        size_ = size;
    }

    static HANDLE create_file_mapping(std::string const& name, std::size_t size)
    {
        return CreateFileMappingA(
            INVALID_HANDLE_VALUE,
            nullptr,
            PAGE_READWRITE,
            static_cast<DWORD>((size >> 32) & 0xFFFFFFFF),
            static_cast<DWORD>(size & 0xFFFFFFFF),
            name.c_str());
    }

    void SharedMemory::create(std::string const& name, std::size_t size)
    {
        fd_ = create_file_mapping(name, size);
        if (fd_ == INVALID_SHM_HANDLE)
        {
            throw_last_error("SharedMemory: CreateFileMappingA(create)");
        }
        map(size);
    }

    bool SharedMemory::try_create(std::string const& name, std::size_t size)
    {
        fd_ = create_file_mapping(name, size);
        if (fd_ == INVALID_SHM_HANDLE)
        {
            throw_last_error("SharedMemory: CreateFileMappingA(try_create)");
        }
        if (GetLastError() == ERROR_ALREADY_EXISTS)
        {
            CloseHandle(fd_);
            fd_ = INVALID_SHM_HANDLE;
            return false;
        }
        map(size);
        return true;
    }

    void SharedMemory::open(std::string const& name)
    {
        if (not try_open(name))
        {
            throw_last_error("SharedMemory: OpenFileMappingA(open)");
        }
    }

    bool SharedMemory::try_open(std::string const& name)
    {
        fd_ = OpenFileMappingA(FILE_MAP_ALL_ACCESS, FALSE, name.c_str());
        if (fd_ == INVALID_SHM_HANDLE)
        {
            if (GetLastError() == ERROR_FILE_NOT_FOUND)
            {
                return false;
            }
            throw_last_error("SharedMemory: OpenFileMappingA(try_open)");
        }

        address_ = MapViewOfFile(fd_, FILE_MAP_ALL_ACCESS, 0, 0, 0);
        if (address_ == nullptr)
        {
            CloseHandle(fd_);
            fd_ = INVALID_SHM_HANDLE;
            throw_last_error("SharedMemory: MapViewOfFile()");
        }

        MEMORY_BASIC_INFORMATION info{};
        if (VirtualQuery(address_, &info, sizeof(info)) == 0)
        {
            UnmapViewOfFile(address_);
            address_ = nullptr;
            CloseHandle(fd_);
            fd_ = INVALID_SHM_HANDLE;
            throw_last_error("SharedMemory: VirtualQuery()");
        }
        size_ = info.RegionSize;
        return true;
    }

    void SharedMemory::close()
    {
        if (address_ != nullptr)
        {
            UnmapViewOfFile(address_);
            address_ = nullptr;
        }
        if (fd_ != INVALID_SHM_HANDLE)
        {
            CloseHandle(fd_);
            fd_ = INVALID_SHM_HANDLE;
        }
        size_ = 0;
    }

    void SharedMemory::unlink(std::string const&)
    {
        // Windows named file mappings are reference-counted by the kernel.
        // Automatically destroyed when all handles are closed.
    }
}
