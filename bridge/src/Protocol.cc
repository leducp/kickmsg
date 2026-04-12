#include "kickmsg/bridge/Protocol.h"

#include <algorithm>

namespace kickmsg::bridge
{
    std::vector<Fragment> fragment_message(void const* data, uint32_t size,
                                          uint32_t msg_id,
                                          uint16_t fragment_size)
    {
        if (fragment_size == 0)
        {
            fragment_size = DEFAULT_FRAGMENT_SIZE;
        }

        uint16_t num_frags = static_cast<uint16_t>((size + fragment_size - 1) / fragment_size);
        if (num_frags == 0)
        {
            num_frags = 1; // empty message still gets one fragment
        }

        // Use evenly-sized chunks so the reassembler can compute offsets
        // from (fragment_index * chunk_size). Last fragment may be shorter.
        uint32_t chunk_size = (num_frags > 1)
            ? (size + num_frags - 1) / num_frags
            : size;

        std::vector<Fragment> frags;
        frags.reserve(num_frags);

        auto const* src = static_cast<uint8_t const*>(data);
        uint32_t offset = 0;

        for (uint16_t i = 0; i < num_frags; ++i)
        {
            uint32_t chunk = std::min(chunk_size, size - offset);

            Fragment f;
            f.header.magic      = PROTOCOL_MAGIC;
            f.header.msg_id     = msg_id;
            f.header.total_size = size;
            f.header.fragment   = i;
            f.header.num_frags  = num_frags;
            f.payload.assign(src + offset, src + offset + chunk);

            frags.push_back(std::move(f));
            offset += chunk;
        }

        return frags;
    }

    Reassembler::Reassembler(std::size_t max_message_size)
        : max_message_size_{max_message_size}
    {
    }

    void Reassembler::reset(uint32_t msg_id, uint32_t total_size, uint16_t num_frags)
    {
        current_msg_id_    = msg_id;
        current_total_size_ = total_size;
        current_num_frags_  = num_frags;
        fragments_received_ = 0;

        received_mask_.assign(num_frags, false);
        buffer_.resize(total_size);
    }

    bool Reassembler::feed(FragmentHeader const& header,
                           void const* payload, std::size_t payload_len)
    {
        if (header.magic != PROTOCOL_MAGIC)
        {
            return false;
        }

        if (header.total_size > max_message_size_)
        {
            return false;
        }

        // New message — drop any incomplete previous one
        if (header.msg_id != current_msg_id_)
        {
            if (current_msg_id_ != UINT32_MAX and fragments_received_ < current_num_frags_)
            {
                ++dropped_;
            }
            reset(header.msg_id, header.total_size, header.num_frags);
        }

        if (header.fragment >= current_num_frags_)
        {
            return false;
        }

        // Skip duplicate fragments
        if (received_mask_[header.fragment])
        {
            return false;
        }

        // Copy payload into the correct position
        uint32_t fragment_size = (current_num_frags_ > 1)
            ? (current_total_size_ + current_num_frags_ - 1) / current_num_frags_
            : current_total_size_;

        // Last fragment may be shorter
        uint32_t offset = static_cast<uint32_t>(header.fragment) * fragment_size;
        uint32_t expected_len = std::min(fragment_size,
                                         current_total_size_ - offset);

        if (payload_len != expected_len)
        {
            return false; // corrupt fragment
        }

        std::memcpy(buffer_.data() + offset, payload, payload_len);
        received_mask_[header.fragment] = true;
        ++fragments_received_;

        return fragments_received_ == current_num_frags_;
    }

    std::vector<uint8_t> Reassembler::take()
    {
        ++completed_;
        current_msg_id_ = UINT32_MAX;
        return std::move(buffer_);
    }

    bool parse_header(void const* data, std::size_t len, FragmentHeader& out)
    {
        if (len < sizeof(FragmentHeader))
        {
            return false;
        }

        std::memcpy(&out, data, sizeof(FragmentHeader));
        return out.magic == PROTOCOL_MAGIC;
    }

    std::vector<uint8_t> serialize_fragment(Fragment const& frag)
    {
        std::vector<uint8_t> buf(sizeof(FragmentHeader) + frag.payload.size());
        std::memcpy(buf.data(), &frag.header, sizeof(FragmentHeader));
        std::memcpy(buf.data() + sizeof(FragmentHeader),
                    frag.payload.data(), frag.payload.size());
        return buf;
    }
}
