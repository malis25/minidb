#include "version.h"

#include <gtest/gtest.h>

TEST(VersionTests, MatchesConfiguredProjectVersion) {
    // This catches accidental drift between CMake's version and the public API.
    EXPECT_EQ(minidb::version(), MINIDB_VERSION);
}
