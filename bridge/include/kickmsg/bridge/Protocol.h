#ifndef KICKMSG_BRIDGE_PROTOCOL_H
#define KICKMSG_BRIDGE_PROTOCOL_H

/// @file Protocol.h
/// @brief Wire protocol for kickmsg network bridge.
///
/// Messages are split into fragments that fit within a single UDP datagram.
/// Each fragment carries a header identifying the message and its position.
/// The receiver reassembles fragments by msg_id and delivers complete messages.

#include <cstdint>
#include <cstring>
#include <vector>

namespace kickmsg::bridge
{
    constexpr uint32_t PROTOCOL_MAGIC = 0x4B4D5347; // "KMSG"
    constexpr uint16_t DEFAULT_FRAGMENT_SIZE = 1400; // safe for most MTUs

    /// Wire header prepended to every UDP datagram.
    struct FragmentHeader
    {
        uint32_t magic;       ///< PROTOCOL_MAGIC
        uint32_t msg_id;      ///< Monotonic message counter (wraps at UINT32_MAX)
        uint32_t total_size;  ///< Original payload size in bytes
        uint16_t fragment;    ///< Fragment index (0-based)
        uint16_t num_frags;   ///< Total fragment count
    };

    static_assert(sizeof(FragmentHeader) == 16, "FragmentHeader must be 16 bytes");

    /// A single fragment ready to send over UDP.
    struct Fragment
    {
        FragmentHeader header;
        std::vector<uint8_t> payload;
    };

    /// Split a message into fragments.
    /// Each fragment's payload is at most fragment_size bytes.
    /// msg_id is set by the caller (monotonic counter).
    std::vector<Fragment> fragment_message(void const* data, uint32_t size,
                                          uint32_t msg_id,
                                          uint16_t fragment_size = DEFAULT_FRAGMENT_SIZE);

    /// Reassembly buffer for incoming fragments.
    /// Collects fragments by msg_id and delivers complete messages.
    /// If a new msg_id arrives before the current one is complete,
    /// the incomplete message is dropped (lossy semantics).
    class Reassembler
    {
    public:
        explicit Reassembler(std::size_t max_message_size = 16 * 1024 * 1024);

        /// Feed a received datagram (header + payload).
        /// Returns true if a complete message is now available via take().
        bool feed(FragmentHeader const& header,
                  void const* payload, std::size_t payload_len);

        /// Take the reassembled message. Valid only after feed() returns true.
        /// Moves the internal buffer out — caller owns the data.
        std::vector<uint8_t> take();

        uint64_t messages_completed() const { return completed_; }
        uint64_t messages_dropped()   const { return dropped_; }

    private:
        std::size_t max_message_size_;

        uint32_t current_msg_id_{UINT32_MAX};
        uint32_t current_total_size_{0};
        uint16_t current_num_frags_{0};
        uint16_t fragments_received_{0};
        std::vector<bool> received_mask_;
        std::vector<uint8_t> buffer_;

        uint64_t completed_{0};
        uint64_t dropped_{0};

        void reset(uint32_t msg_id, uint32_t total_size, uint16_t num_frags);
    };

    /// Parse a FragmentHeader from raw bytes.
    /// Returns false if the buffer is too small or magic doesn't match.
    bool parse_header(void const* data, std::size_t len, FragmentHeader& out);

    /// Serialize a FragmentHeader + payload into a single buffer for sending.
    std::vector<uint8_t> serialize_fragment(Fragment const& frag);
}

#endif
