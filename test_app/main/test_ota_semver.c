#include "unity.h"
#include "ota_manifest.h"

TEST_CASE("equal versions return 0", "[ota_semver]")
{
    TEST_ASSERT_EQUAL_INT(0, ota_semver_compare("1.0.0", "1.0.0"));
    TEST_ASSERT_EQUAL_INT(0, ota_semver_compare("0.0.0", "0.0.0"));
    TEST_ASSERT_EQUAL_INT(0, ota_semver_compare("99.99.99", "99.99.99"));
}

TEST_CASE("newer major version is greater", "[ota_semver]")
{
    TEST_ASSERT_GREATER_THAN_INT(0, ota_semver_compare("2.0.0", "1.0.0"));
    TEST_ASSERT_LESS_THAN_INT   (0, ota_semver_compare("1.0.0", "2.0.0"));
}

TEST_CASE("newer minor version is greater", "[ota_semver]")
{
    TEST_ASSERT_GREATER_THAN_INT(0, ota_semver_compare("1.3.0", "1.2.0"));
    TEST_ASSERT_LESS_THAN_INT   (0, ota_semver_compare("1.2.0", "1.3.0"));
}

TEST_CASE("newer patch version is greater", "[ota_semver]")
{
    TEST_ASSERT_GREATER_THAN_INT(0, ota_semver_compare("1.0.1", "1.0.0"));
    TEST_ASSERT_LESS_THAN_INT   (0, ota_semver_compare("1.0.0", "1.0.1"));
}

TEST_CASE("major dominates minor and patch", "[ota_semver]")
{
    /* 2.0.0 > 1.99.99 */
    TEST_ASSERT_GREATER_THAN_INT(0, ota_semver_compare("2.0.0", "1.99.99"));
}

TEST_CASE("minor dominates patch", "[ota_semver]")
{
    /* 1.2.0 > 1.1.99 */
    TEST_ASSERT_GREATER_THAN_INT(0, ota_semver_compare("1.2.0", "1.1.99"));
}

TEST_CASE("OTA should apply when manifest is newer", "[ota_semver]")
{
    const char *current  = "1.2.3";
    const char *manifest = "1.2.4";
    TEST_ASSERT_GREATER_THAN_INT(0, ota_semver_compare(manifest, current));
}

TEST_CASE("OTA should not apply when manifest is same or older", "[ota_semver]")
{
    TEST_ASSERT_LESS_OR_EQUAL_INT(0, ota_semver_compare("1.2.3", "1.2.3"));
    TEST_ASSERT_LESS_OR_EQUAL_INT(0, ota_semver_compare("1.2.2", "1.2.3"));
}
