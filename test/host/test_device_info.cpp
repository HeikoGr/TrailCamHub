#include <doctest/doctest.h>

#include <string>

#include "artemis_json.hpp"
#include "device_info.hpp"

using namespace camera::artemis;

TEST_CASE("parse_device_info reads identity and state from a 512 response")
{
    const std::string json =
        "{\"devId\":\"TC100-ED3B\",\"fwVerName\":\"1.4.2\","
        "\"modelName\":\"TC100\",\"subModelName\":\"A\",\"productId\":7,"
        "\"batPercent\":83,\"sdTotalMB\":30436,\"sdFreeMB\":28110,"
        "\"capability\":{\"video\":1},\"result\":0,\"cmdId\":512}";
    DeviceInfo info;
    REQUIRE(parse_device_info(json, info));
    CHECK(info.dev_id == "TC100-ED3B");
    CHECK(info.model_name == "TC100");
    CHECK(info.fw_version == "1.4.2");
    CHECK(info.bat_percent == 83);
    CHECK(info.sd_total_mb == 30436);
    CHECK(info.sd_free_mb == 28110);
}

TEST_CASE(
    "parse_device_info accepts a numeric devId and defaults missing fields")
{
    const std::string json = "{\"devId\":12345,\"result\":0,\"cmdId\":512}";
    DeviceInfo info;
    REQUIRE(parse_device_info(json, info));
    CHECK(info.dev_id == "12345");
    CHECK(info.bat_percent == -1);  // unknown
    CHECK(info.model_name.empty());
}

TEST_CASE("parse_device_info fails without a device identity")
{
    DeviceInfo info;
    CHECK_FALSE(parse_device_info("{\"batPercent\":50}", info));
}

TEST_CASE("json_find_string handles quotes, escapes and scoping")
{
    std::string s;
    REQUIRE(json_find_string("{\"a\":\"hello\",\"b\":\"x\"}", "a", s));
    CHECK(s == "hello");

    REQUIRE(
        json_find_string("{\"path\":\"C:\\\\dir\\/f\\\"q\\\"\"}", "path", s));
    CHECK(s == "C:\\dir/f\"q\"");

    // A later key is found when scoping past the first object.
    std::string json = "{\"name\":\"first\"}{\"name\":\"second\"}";
    std::size_t from = json.find('}');
    REQUIRE(json_find_string(json, "name", s, from));
    CHECK(s == "second");

    CHECK_FALSE(json_find_string("{\"n\":123}", "n", s));      // not a string
    CHECK_FALSE(json_find_string("{\"n\":\"abc\"}", "x", s));  // absent
}
