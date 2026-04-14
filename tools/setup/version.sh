#!/bin/bash

# Default version
VERSION="0.0.0"

# Check if we're inside a git repository
if git rev-parse --git-dir > /dev/null 2>&1; then
    # Try to get the exact tag for HEAD
    TAG=$(git describe --tags --exact-match 2>/dev/null || true)

    if [[ -n "$TAG" ]]; then
        # HEAD is exactly at a tag.  Strip a leading "v" so conventional
        # "v0.1.0" tags map to PEP 440-compliant "0.1.0" — PyPI rejects
        # metadata with a leading "v" in [project] version.
        VERSION="${TAG#v}"
    fi
fi

# Export the VERSION variable
export VERSION
echo "VERSION set to: $VERSION"
