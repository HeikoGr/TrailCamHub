#include <doctest/doctest.h>

#include "media_store.hpp"

using camera::store::sanitize_camera_id;

TEST_CASE("sanitize_camera_id keeps safe characters and neutralises the rest")
{
    CHECK(sanitize_camera_id("TC100-ED3B") == "TC100-ED3B");
    CHECK(sanitize_camera_id("F4:C1:56:06:ED:3B") == "F4_C1_56_06_ED_3B");
    CHECK(sanitize_camera_id("a/b\\c..d") == "a_b_c..d");
    CHECK(sanitize_camera_id("") == "unknown");
    CHECK(sanitize_camera_id("..") == "_..");
    CHECK(sanitize_camera_id(".") == "_.");
}

TEST_CASE("sanitize_camera_id caps the length")
{
    std::string long_id(100, 'x');
    CHECK(sanitize_camera_id(long_id).size() == 48);
}
