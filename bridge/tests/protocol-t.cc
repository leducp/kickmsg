#include <gtest/gtest.h>

#include "kickmsg/bridge/Protocol.h"

#include <cstring>
#include <numeric>

using namespace kickmsg::bridge;

TEST(Protocol, FragmentSmallMessage)
{
    uint8_t data[] = {1, 2, 3, 4, 5};
    auto frags = fragment_message(data, sizeof(data), 42);

    ASSERT_EQ(frags.size(), 1u);
    EXPECT_EQ(frags[0].header.magic, PROTOCOL_MAGIC);
    EXPECT_EQ(frags[0].header.msg_id, 42u);
    EXPECT_EQ(frags[0].header.total_size, 5u);
    EXPECT_EQ(frags[0].header.fragment, 0u);
    EXPECT_EQ(frags[0].header.num_frags, 1u);
    EXPECT_EQ(frags[0].payload.size(), 5u);
    EXPECT_EQ(std::memcmp(frags[0].payload.data(), data, 5), 0);
}

TEST(Protocol, FragmentLargeMessage)
{
    std::vector<uint8_t> data(5000);
    std::iota(data.begin(), data.end(), 0);

    auto frags = fragment_message(data.data(), static_cast<uint32_t>(data.size()),
                                  1, 1400);

    // 5000 / 1400 = 3.57 → 4 fragments
    ASSERT_EQ(frags.size(), 4u);

    for (uint16_t i = 0; i < 4; ++i)
    {
        EXPECT_EQ(frags[i].header.magic, PROTOCOL_MAGIC);
        EXPECT_EQ(frags[i].header.msg_id, 1u);
        EXPECT_EQ(frags[i].header.total_size, 5000u);
        EXPECT_EQ(frags[i].header.fragment, i);
        EXPECT_EQ(frags[i].header.num_frags, 4u);
    }

    // First 3 fragments: 1250 bytes each (5000/4), last: 1250
    // Actually: ceil(5000/4) = 1250 per fragment
    uint32_t total_payload = 0;
    for (auto const& f : frags)
    {
        total_payload += static_cast<uint32_t>(f.payload.size());
    }
    EXPECT_EQ(total_payload, 5000u);
}

TEST(Protocol, FragmentReassembleRoundtrip)
{
    std::vector<uint8_t> original(10000);
    std::iota(original.begin(), original.end(), 0);

    auto frags = fragment_message(original.data(),
                                  static_cast<uint32_t>(original.size()),
                                  7, 1400);

    Reassembler reasm;

    for (std::size_t i = 0; i < frags.size() - 1; ++i)
    {
        bool complete = reasm.feed(frags[i].header,
                                   frags[i].payload.data(),
                                   frags[i].payload.size());
        EXPECT_FALSE(complete);
    }

    auto const& last = frags.back();
    bool complete = reasm.feed(last.header, last.payload.data(), last.payload.size());
    EXPECT_TRUE(complete);

    auto result = reasm.take();
    ASSERT_EQ(result.size(), original.size());
    EXPECT_EQ(std::memcmp(result.data(), original.data(), original.size()), 0);
    EXPECT_EQ(reasm.messages_completed(), 1u);
    EXPECT_EQ(reasm.messages_dropped(), 0u);
}

TEST(Protocol, ReassembleOutOfOrder)
{
    std::vector<uint8_t> original(4200);
    std::iota(original.begin(), original.end(), 0);

    auto frags = fragment_message(original.data(),
                                  static_cast<uint32_t>(original.size()),
                                  1, 1400);

    ASSERT_EQ(frags.size(), 3u);

    Reassembler reasm;

    // Feed in reverse order
    EXPECT_FALSE(reasm.feed(frags[2].header, frags[2].payload.data(), frags[2].payload.size()));
    EXPECT_FALSE(reasm.feed(frags[0].header, frags[0].payload.data(), frags[0].payload.size()));
    EXPECT_TRUE(reasm.feed(frags[1].header, frags[1].payload.data(), frags[1].payload.size()));

    auto result = reasm.take();
    EXPECT_EQ(result.size(), original.size());
    EXPECT_EQ(std::memcmp(result.data(), original.data(), original.size()), 0);
}

TEST(Protocol, ReassembleDropsIncomplete)
{
    std::vector<uint8_t> data1(2800);
    std::fill(data1.begin(), data1.end(), 0xAA);

    std::vector<uint8_t> data2(100);
    std::fill(data2.begin(), data2.end(), 0xBB);

    auto frags1 = fragment_message(data1.data(), static_cast<uint32_t>(data1.size()), 1, 1400);
    auto frags2 = fragment_message(data2.data(), static_cast<uint32_t>(data2.size()), 2, 1400);

    ASSERT_EQ(frags1.size(), 2u);
    ASSERT_EQ(frags2.size(), 1u);

    Reassembler reasm;

    // Feed only first fragment of msg 1 (incomplete)
    EXPECT_FALSE(reasm.feed(frags1[0].header, frags1[0].payload.data(), frags1[0].payload.size()));

    // Feed msg 2 — should drop incomplete msg 1
    EXPECT_TRUE(reasm.feed(frags2[0].header, frags2[0].payload.data(), frags2[0].payload.size()));

    auto result = reasm.take();
    EXPECT_EQ(result.size(), 100u);
    EXPECT_EQ(result[0], 0xBB);
    EXPECT_EQ(reasm.messages_completed(), 1u);
    EXPECT_EQ(reasm.messages_dropped(), 1u);
}

TEST(Protocol, FragmentEmptyMessage)
{
    auto frags = fragment_message(nullptr, 0, 99);

    ASSERT_EQ(frags.size(), 1u);
    EXPECT_EQ(frags[0].header.total_size, 0u);
    EXPECT_EQ(frags[0].header.num_frags, 1u);
    EXPECT_TRUE(frags[0].payload.empty());
}

TEST(Protocol, SerializeParseRoundtrip)
{
    uint8_t data[] = {10, 20, 30};
    auto frags = fragment_message(data, sizeof(data), 5);

    auto serialized = serialize_fragment(frags[0]);

    FragmentHeader parsed;
    EXPECT_TRUE(parse_header(serialized.data(), serialized.size(), parsed));
    EXPECT_EQ(parsed.magic, PROTOCOL_MAGIC);
    EXPECT_EQ(parsed.msg_id, 5u);
    EXPECT_EQ(parsed.total_size, 3u);

    // Payload follows the header
    EXPECT_EQ(std::memcmp(serialized.data() + sizeof(FragmentHeader), data, 3), 0);
}

TEST(Protocol, ParseRejectsBadMagic)
{
    uint8_t garbage[16] = {};
    FragmentHeader parsed;
    EXPECT_FALSE(parse_header(garbage, sizeof(garbage), parsed));
}

TEST(Protocol, ParseRejectsTooSmall)
{
    uint8_t small[8] = {};
    FragmentHeader parsed;
    EXPECT_FALSE(parse_header(small, sizeof(small), parsed));
}

TEST(Protocol, Fragment16MBMessage)
{
    std::size_t size = 16 * 1024 * 1024;
    std::vector<uint8_t> big(size);
    std::iota(big.begin(), big.end(), 0);

    auto frags = fragment_message(big.data(), static_cast<uint32_t>(size), 100, 1400);

    // 16MB / 1400 = ~11946 fragments
    EXPECT_GT(frags.size(), 11000u);

    Reassembler reasm(size);

    bool complete = false;
    for (auto const& f : frags)
    {
        complete = reasm.feed(f.header, f.payload.data(), f.payload.size());
    }
    EXPECT_TRUE(complete);

    auto result = reasm.take();
    ASSERT_EQ(result.size(), size);
    EXPECT_EQ(std::memcmp(result.data(), big.data(), size), 0);
}
