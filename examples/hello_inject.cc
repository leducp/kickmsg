/// @file hello_inject.cc
/// @brief Back a kickmsg region with caller-provided memory.
///
/// Demonstrates SharedRegion::attach_create / attach_open: the library
/// stamps the region into a buffer the caller already owns.  Use this
/// when the memory comes from somewhere other than POSIX shm — a
/// different shared-memory provider, hugepages, a hardware-mapped
/// region, or (as here) an in-process aligned heap buffer.
///
/// The caller owns the buffer's lifetime; unlink() is a no-op for
/// injected regions.

#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <memory>

#include <kickmsg/Publisher.h>
#include <kickmsg/Subscriber.h>

int main()
{
    kickmsg::channel::Config cfg;
    cfg.max_subscribers   = 2;
    cfg.sub_ring_capacity = 8;
    cfg.pool_size         = 16;
    cfg.max_payload_size  = 128;

    std::size_t const size = kickmsg::SharedRegion::required_size(cfg, "inject_example");

    void* raw = nullptr;
    if (::posix_memalign(&raw, kickmsg::CACHE_LINE, size) != 0)
    {
        std::cerr << "posix_memalign failed\n";
        return 1;
    }
    std::unique_ptr<void, decltype(&::free)> buffer{raw, &::free};

    auto region = kickmsg::SharedRegion::attach_create(
        buffer.get(), size, kickmsg::channel::PubSub, cfg,
        "inject_example", "demo-inject");

    kickmsg::Subscriber sub(region);
    kickmsg::Publisher  pub(region);

    for (uint32_t i = 0; i < 5; ++i)
    {
        if (pub.send(&i, sizeof(i)) < 0)
        {
            std::cerr << "Failed to send message " << i << "\n";
        }
    }

    while (auto sample = sub.try_receive())
    {
        uint32_t value = 0;
        std::memcpy(&value, sample->data(), sizeof(value));
        std::cout << "Received: " << value << "\n";
    }

    // A second attach to the SAME buffer (e.g. another component in the
    // same process that was handed the address by the memory provider)
    // can use attach_open to validate magic/version and read info().
    auto reader = kickmsg::SharedRegion::attach_open(
        buffer.get(), size, "demo-inject-reader");
    auto info = reader.info();
    std::cout << "Reader sees creator='" << info.creator_name
              << "', label='" << info.shm_name
              << "', pool_size=" << info.pool_size << "\n";

    std::cout << "Done.\n";
    return 0;
}
