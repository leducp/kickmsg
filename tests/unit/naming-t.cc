#include <limits.h>
#include <stdexcept>
#include <string>

#include <gtest/gtest.h>

#include "kickmsg/Naming.h"

#if defined(__APPLE__) || defined(__DARWIN__)
    #include <sys/posix_shm.h>
#elif defined(_WIN32)
    #ifndef WIN32_LEAN_AND_MEAN
        #define WIN32_LEAN_AND_MEAN
    #endif
    #include <windows.h>   // MAX_PATH
#endif

using kickmsg::sanitize_shm_component;

TEST(SanitizeShmComponent, MapsToThePortableFilenameSet)
{
    struct Case
    {
        char const* input;
        char const* expected;
    };
    Case const cases[] = {
        {"topic",             "topic"},
        {"sensor42",          "sensor42"},
        {"a.b_c-d.42",        "a.b_c-d.42"},
        {"/topic",            "topic"},
        {"///topic",          "topic"},
        {"robot/arm",         "robot.arm"},
        {"robot/arm/joint1",  "robot.arm.joint1"},
        {"/robot/arm/joint1", "robot.arm.joint1"},
        {"hello world",       "hello_world"},
        {"a:b;c",             "a_b_c"},
        {"with\ttab",         "with_tab"},
        {"weird@name!",       "weird_name_"},
        {"caf\xC3\xA9",       "caf__"},
        {"/robot/arm joint1", "robot.arm_joint1"},
    };
    for (auto const& c : cases)
    {
        auto once = sanitize_shm_component(c.input, "topic");
        EXPECT_EQ(once, c.expected) << c.input;
        EXPECT_EQ(sanitize_shm_component(once, "topic"), once) << c.input;
    }
}

// --- Error cases ---------------------------------------------------------

TEST(SanitizeShmComponent, OnlySlashesThrows)
{
    // After stripping leading '/'s there is nothing left — same category
    // of error as an empty input.
    EXPECT_THROW(sanitize_shm_component("/",   "topic"), std::invalid_argument);
    EXPECT_THROW(sanitize_shm_component("///", "topic"), std::invalid_argument);
}

TEST(SanitizeShmComponent, WhatIsIncludedInErrorMessage)
{
    // The `what` label is the only way the caller can tell which of the
    // several calls in make_topic_name / make_mailbox_name failed — this
    // test pins that contract so refactors don't accidentally drop it.
    try
    {
        sanitize_shm_component("", "namespace");
        FAIL() << "expected std::invalid_argument";
    }
    catch (std::invalid_argument const& e)
    {
        std::string msg = e.what();
        EXPECT_NE(msg.find("namespace"), std::string::npos) << msg;
    }
}

// --- compose_shm_name ----------------------------------------------------

using kickmsg::compose_shm_name;

TEST(ComposeShmName, FitsPlatformLimit)
{
    auto name = compose_shm_name("ns", "topic");
    ASSERT_FALSE(name.empty());
    EXPECT_EQ(name[0], '/');
#if defined(__APPLE__) || defined(__DARWIN__)
    EXPECT_LE(name.size(), static_cast<std::size_t>(PSHMNAMLEN) - 1);
#elif defined(_WIN32)
    EXPECT_LE(name.size(), static_cast<std::size_t>(MAX_PATH));
#else
    EXPECT_LE(name.size() - 1, static_cast<std::size_t>(NAME_MAX));
#endif
}

TEST(ComposeShmName, Deterministic)
{
    // Same inputs must always produce the same shm name — peers in
    // different processes need to agree on the name.
    auto a = compose_shm_name("demo", "temperature");
    auto b = compose_shm_name("demo", "temperature");
    EXPECT_EQ(a, b);
}

TEST(ComposeShmName, DistinctInputsProduceDistinctNames)
{
    EXPECT_NE(compose_shm_name("alpha", "temperature"), compose_shm_name("beta", "temperature"));
    EXPECT_NE(compose_shm_name("demo",  "temperature"), compose_shm_name("demo", "pressure"));
}

TEST(ComposeShmName, FitsEvenWhenInputsAreLong)
{
    std::string long_ns(80,  'a');
    std::string long_sx(120, 'b');
#if defined(__APPLE__) || defined(__DARWIN__)
    auto name = compose_shm_name(long_ns, long_sx);
    EXPECT_LE(name.size(), static_cast<std::size_t>(PSHMNAMLEN) - 1);
#else
    EXPECT_NO_THROW(compose_shm_name(long_ns, long_sx));
#endif
}

#if !defined(__APPLE__) && !defined(__DARWIN__)
TEST(ComposeShmName, ThrowsWhenExceedingPlatformLimit)
{
    // Non-macOS (readable names): a pre-composed name longer than the
    // platform limit (Linux NAME_MAX, Windows MAX_PATH) must throw clearly
    // instead of truncating or failing with an opaque OS error. 400+ chars
    // exceeds both. macOS hashes to a fixed length, so it never throws here.
    std::string ns(200, 'n');
    std::string sx(200, 's');
    EXPECT_THROW(compose_shm_name(ns, sx), std::system_error);
}
#endif

#if defined(__APPLE__) || defined(__DARWIN__)
TEST(ComposeShmName, HashFormOnMacOS)
{
    // On macOS the composed name is hash-only (no embedded plaintext).
    // Pin the shape so a future refactor doesn't accidentally regress
    // back to a readable form that overflows PSHMNAMLEN.
    auto name = compose_shm_name("ns", "topic");
    EXPECT_EQ(name[0], '/');
    EXPECT_EQ(name.find("ns"),    std::string::npos);
    EXPECT_EQ(name.find("topic"), std::string::npos);
}
#endif
