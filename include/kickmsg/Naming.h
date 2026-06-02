#ifndef KICKMSG_NAMING_H
#define KICKMSG_NAMING_H

#include <cstdint>
#include <string>
#include <string_view>

namespace kickmsg
{
    /// Lowercase 16-char hex representation of a 64-bit value, zero-padded.
    std::string to_hex(uint64_t v);


    /// Sanitize a user-supplied name component (namespace / topic / channel /
    /// owner / tag) into something POSIX shm_open will accept.
    ///
    /// POSIX requires shm names to start with a single '/' and contain no
    /// further '/' characters; Linux additionally constrains the remainder
    /// to a single path component under /dev/shm.  This helper produces the
    /// "after the leading slash" fragment for one component — callers
    /// assemble the final "/<prefix>_<topic>" path themselves.
    ///
    /// Rules (human-readable, no hashing):
    ///   - strip leading '/'  — lets callers pass ROS-style absolute names
    ///     like "/robot/arm" without producing "//…" or embedded slashes
    ///   - interior '/' becomes '.' — preserves hierarchy visually
    ///     ("robot/arm/joint1" -> "robot.arm.joint1")
    ///   - POSIX "portable filename" chars [A-Za-z0-9._-] pass through
    ///   - everything else becomes '_' — deterministic, no collisions
    ///     between benign inputs, still eyeballable in `ls /dev/shm`
    ///
    /// Throws std::invalid_argument if the result would be empty (e.g. input
    /// is "", "/", "///") — a blank component would produce ambiguous names
    /// like "/prefix_" that silently collide across callers.  \p what is
    /// interpolated into the exception message ("namespace", "topic", etc.).
    std::string sanitize_shm_component(std::string_view s, char const* what);

    /// Compose the final shm name from a sanitized namespace and suffix.
    /// macOS: "/" + hex(fnv1a64(ns)) + hex(fnv1a64(suffix)), capped at
    /// PSHMNAMLEN - 1.  Linux / Windows: readable "/" + ns + "_" + suffix,
    /// throws std::system_error(ENAMETOOLONG) past the platform limit
    /// (Linux NAME_MAX, Windows MAX_PATH).
    ///
    /// macOS caveat: PSHMNAMLEN (31) leaves no room for a readable name, so
    /// the result is a hash and the suffix hash is truncated to fit. Two
    /// distinct (namespace, suffix) pairs can therefore collide onto the
    /// same shm object — distinct topics would then share one region.
    /// Linux names are exact and never collide. Collisions are astronomically
    /// unlikely but not impossible; if it matters, keep names short enough to
    /// stay readable (Linux) or verify topology out of band.
    std::string compose_shm_name(std::string_view sanitized_namespace,
                                 std::string_view sanitized_suffix);
}

#endif
