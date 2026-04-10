#ifndef KICKMSG_OS_SHARED_MEMORY_H
#define KICKMSG_OS_SHARED_MEMORY_H

#include <cstddef>
#include <string>

#ifdef _WIN32
    #define WIN32_LEAN_AND_MEAN
    #include <windows.h>
#endif

namespace kickmsg
{
#ifdef _WIN32
    using os_shm_handle = HANDLE;
    constexpr os_shm_handle INVALID_SHM_HANDLE = nullptr;
#else
    using os_shm_handle = int;
    constexpr os_shm_handle INVALID_SHM_HANDLE = -1;
#endif

    /// RAII wrapper around a named shared-memory region.
    /// Platform-specific: POSIX shm_open on Linux, named file mapping on Windows.
    class SharedMemory
    {
    public:
        SharedMemory() = default;
        SharedMemory(SharedMemory const&) = delete;
        SharedMemory& operator=(SharedMemory const&) = delete;
        SharedMemory(SharedMemory&& other) noexcept;
        SharedMemory& operator=(SharedMemory&& other) noexcept;
        ~SharedMemory();

        /// Create a new shared-memory region. Truncates to \p size bytes.
        void create(std::string const& name, std::size_t size);

        /// Attempt to create a new shared-memory region.
        /// Returns true if created, false if it already exists.
        bool try_create(std::string const& name, std::size_t size);

        /// Open an existing shared-memory region (read/write).
        void open(std::string const& name);

        /// Unmap and close the handle.
        void close();

        /// Remove the shared-memory object from the filesystem.
        /// No-op on Windows (kernel reference-counted).
        static void unlink(std::string const& name);

        void*       address() const { return address_; }
        std::size_t size()    const { return size_; }
        bool        is_open() const { return address_ != nullptr; }

    private:
        void map(std::size_t size);

        std::size_t    size_{0};
        void*          address_{nullptr};
        os_shm_handle  fd_{INVALID_SHM_HANDLE};
    };
}

#endif
