// Parts of SharedMemory that are identical on Linux and macOS.
// Platform-specific create() lives in src/os/{linux,darwin}/SharedMemory.cc.
#include "kickmsg/os/SharedMemory.h"

#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <fcntl.h>
#include <string>
#include <system_error>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>

namespace kickmsg
{
    mode_t kickmsg_shm_mode()
    {
        static mode_t const mode = []() -> mode_t
        {
            constexpr mode_t DEFAULT_MODE = 0600;
            char const* env = std::getenv("KICKMSG_SHM_MODE");
            if (env == nullptr or *env == '\0')
            {
                return DEFAULT_MODE;
            }
            errno = 0;
            char*         end = nullptr;
            unsigned long v   = std::strtoul(env, &end, 8);
            if (errno != 0 or end == env or *end != '\0' or v > 07777)
            {
                std::fprintf(stderr,
                    "kickmsg: invalid KICKMSG_SHM_MODE '%s' "
                    "(expected octal permission bits, e.g. \"0666\"); using 0600\n",
                    env);
                return DEFAULT_MODE;
            }
            return static_cast<mode_t>(v);
        }();
        return mode;
    }

    namespace
    {
        [[noreturn]] void throw_system_error(char const* context,
                                             std::string const& name = "")
        {
            // Append the shm name so the failure points at the offending
            // region rather than just naming the syscall.
            std::string msg = context;
            if (not name.empty())
            {
                msg += " '" + name + "'";
            }
            throw std::system_error(errno, std::system_category(), msg);
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
        // directly -- there's no reason to close here and re-enter create(),
        // and the old round-trip pattern caused a subtle race on Darwin.
        fd_ = ::shm_open(name.c_str(), O_RDWR | O_CREAT | O_EXCL, kickmsg_shm_mode());
        if (fd_ < 0)
        {
            if (errno == EEXIST)
            {
                fd_ = INVALID_SHM_HANDLE;
                return false;
            }
            throw_system_error("SharedMemory: shm_open(try_create)", name);
        }
        try
        {
            map(size);
        }
        catch (...)
        {
            // O_EXCL made the name ours; leaving a zero-size object would
            // wedge it (EEXIST on create, not-ready on open, forever).
            ::shm_unlink(name.c_str());
            throw;
        }
        return true;
    }

    void SharedMemory::open(std::string const& name)
    {
        if (not try_open(name))
        {
            throw_system_error("SharedMemory: shm_open(open)", name);
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
            throw_system_error("SharedMemory: shm_open(try_open)", name);
        }

        struct stat st{};
        if (::fstat(fd_, &st) < 0)
        {
            ::close(fd_);
            fd_ = INVALID_SHM_HANDLE;
            throw_system_error("SharedMemory: fstat()");
        }

        size_ = static_cast<std::size_t>(st.st_size);
        if (size_ == 0)
        {
            // Creator reached shm_open(O_CREAT) but not ftruncate() yet.
            // mmap(., 0, .) would fail EINVAL; report not-ready so the
            // create_or_open / spin_open retry loops keep spinning.
            ::close(fd_);
            fd_ = INVALID_SHM_HANDLE;
            return false;
        }
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
