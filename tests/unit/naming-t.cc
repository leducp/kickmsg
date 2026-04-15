#include <stdexcept>
#include <string>

#include <gtest/gtest.h>

#include "kickmsg/Naming.h"

using kickmsg::sanitize_shm_component;

// --- Pass-through for already-valid inputs -------------------------------

TEST(SanitizeShmComponent, PlainAlnumIsUnchanged)
{
    EXPECT_EQ(sanitize_shm_component("topic",     "topic"), "topic");
    EXPECT_EQ(sanitize_shm_component("sensor42",  "topic"), "sensor42");
}

TEST(SanitizeShmComponent, PortableFilenameCharsSurvive)
{
    // POSIX portable filename set [A-Za-z0-9._-] must pass through verbatim
    // — this is what allows `ls /dev/shm` output to stay human-readable.
    EXPECT_EQ(sanitize_shm_component("a.b_c-d.42", "topic"), "a.b_c-d.42");
}

// --- Leading slash handling (ROS-style absolute names) -------------------

TEST(SanitizeShmComponent, StripsSingleLeadingSlash)
{
    EXPECT_EQ(sanitize_shm_component("/topic", "topic"), "topic");
}

TEST(SanitizeShmComponent, StripsRepeatedLeadingSlashes)
{
    // "///a" -> "a" (not ".a" or "..a") — otherwise a user who typed "//x"
    // by accident would silently get a different region than "/x".
    EXPECT_EQ(sanitize_shm_component("///topic", "topic"), "topic");
}

// --- Interior slash becomes '.' ------------------------------------------

TEST(SanitizeShmComponent, InteriorSlashBecomesDot)
{
    EXPECT_EQ(sanitize_shm_component("robot/arm",        "topic"),
              "robot.arm");
    EXPECT_EQ(sanitize_shm_component("robot/arm/joint1", "topic"),
              "robot.arm.joint1");
}

TEST(SanitizeShmComponent, LeadingAndInteriorSlashesCombine)
{
    // The leading-slash strip must not fire for interior slashes after
    // real characters have been seen.
    EXPECT_EQ(sanitize_shm_component("/robot/arm/joint1", "topic"),
              "robot.arm.joint1");
}

// --- Fallback mapping for everything else --------------------------------

TEST(SanitizeShmComponent, InvalidCharsBecomeUnderscore)
{
    EXPECT_EQ(sanitize_shm_component("hello world",  "topic"), "hello_world");
    EXPECT_EQ(sanitize_shm_component("a:b;c",        "topic"), "a_b_c");
    EXPECT_EQ(sanitize_shm_component("with\ttab",    "topic"), "with_tab");
    EXPECT_EQ(sanitize_shm_component("weird@name!",  "topic"), "weird_name_");
}

TEST(SanitizeShmComponent, UnicodeBytesBecomeUnderscores)
{
    // Multi-byte UTF-8 sequences: each byte fails isalnum and maps to '_'.
    // We don't care about exact width — only that the output stays within
    // the portable set. "é" is 0xC3 0xA9 -> "__".
    auto out = sanitize_shm_component("caf\xC3\xA9", "topic");
    EXPECT_EQ(out, "caf__");
}

// --- Idempotency ---------------------------------------------------------

TEST(SanitizeShmComponent, Idempotent)
{
    // Sanitize twice -> same as sanitize once.  Matters because Node
    // sanitizes the prefix in the ctor, and make_topic_name could
    // conceivably be called with the prefix as an input somewhere.
    auto once  = sanitize_shm_component("/robot/arm joint1", "topic");
    auto twice = sanitize_shm_component(once, "topic");
    EXPECT_EQ(once,  "robot.arm_joint1");
    EXPECT_EQ(twice, once);
}

// --- Error cases ---------------------------------------------------------

TEST(SanitizeShmComponent, EmptyInputThrows)
{
    EXPECT_THROW(sanitize_shm_component("", "topic"), std::invalid_argument);
}

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
