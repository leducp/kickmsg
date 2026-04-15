#ifndef KICKMSG_NAMING_H
#define KICKMSG_NAMING_H

#include <string>
#include <string_view>

namespace kickmsg
{
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
}

#endif
