#include <doctest/doctest.h>

#include <string>

#include "uplink_protocol.hpp"

using namespace uplink;

TEST_CASE("check-in body carries telemetry and photo/video media only")
{
    CheckinRequest request;
    request.hub_firmware = "0.4.0";
    request.camera_known = true;
    request.camera.model = "TC100";
    request.camera.firmware = "2.3.0.3";
    request.camera.bat_percent = 78;
    request.camera.sd_free_mb = 28412;
    request.media = {{100, 36, 0, 1766505692},
                     {100, 35, 2, 1766505600},
                     {100, 34, 7, 1766505500}};

    CHECK(build_checkin_json(request) ==
          "{\"hubFirmware\":\"0.4.0\",\"camera\":{\"model\":\"TC100\","
          "\"firmware\":\"2.3.0.3\",\"batPercent\":78,\"sdFreeMB\":28412},"
          "\"media\":[{\"dir\":100,\"num\":36,\"fileType\":0,"
          "\"mediaTime\":1766505692},{\"dir\":100,\"num\":35,\"fileType\":2,"
          "\"mediaTime\":1766505600}]}");
}

TEST_CASE("check-in body leaves out unknown values")
{
    CheckinRequest request;
    request.hub_firmware = "dev";
    request.hub_bat_percent = 91;
    CHECK(build_checkin_json(request) ==
          "{\"hubFirmware\":\"dev\",\"hubBatPercent\":91,\"media\":[]}");

    request.camera_known = true;
    request.camera.bat_percent = 5;
    CHECK(build_checkin_json(request).find("\"camera\":{\"batPercent\":5}") !=
          std::string::npos);
}

TEST_CASE("strings are JSON-escaped")
{
    CHECK(json_escape("a\"b\\c\nd") == "a\\\"b\\\\c\\nd");
    CHECK(json_escape(std::string("\x01", 1)) == "\\u0001");
}

TEST_CASE("check-in response yields uploads, commands and server time")
{
    const std::string json =
        "{\"serverTime\":1766506000,\"thumbUploads\":["
        "{\"dir\":100,\"num\":36,\"url\":\"https://bucket.s3.eu-central-1."
        "amazonaws.com/thumb/F4/100_36.jpg?X-Amz-Signature=ab%2Fcd&x=1\"},"
        "{\"dir\":100,\"num\":35,\"url\":\"http://insecure.example/\"}],"
        "\"commands\":[{\"id\":\"c-1\",\"type\":\"requestVideo\",\"dir\":100,"
        "\"num\":35}],\"config\":{\"sleepIntervalS\":1800}}";

    CheckinResponse response;
    REQUIRE(parse_checkin_response(json, response));
    CHECK(response.server_time == 1766506000);
    CHECK(response.sleep_interval_s == 1800);
    REQUIRE(response.thumb_uploads.size() == 1);  // http URL rejected
    CHECK(response.thumb_uploads[0].num == 36);
    CHECK(response.thumb_uploads[0].url.find("X-Amz-Signature=ab%2Fcd") !=
          std::string::npos);
    REQUIRE(response.commands.size() == 1);
    CHECK(response.commands[0].id == "c-1");
    CHECK(response.commands[0].type == "requestVideo");
    CHECK(response.commands[0].dir == 100);
    CHECK(response.commands[0].num == 35);
}

TEST_CASE("empty lists and brackets inside strings")
{
    CheckinResponse response;
    REQUIRE(parse_checkin_response(
        "{\"serverTime\":1,\"thumbUploads\":[],\"commands\":[{\"id\":\"a]b}"
        "\",\"type\":\"x\"}],\"config\":{\"sleepIntervalS\":60}}",
        response));
    CHECK(response.thumb_uploads.empty());
    REQUIRE(response.commands.size() == 1);
    CHECK(response.commands[0].id == "a]b}");
}

TEST_CASE("responses without serverTime are rejected")
{
    CheckinResponse response;
    CHECK_FALSE(
        parse_checkin_response("{\"error\":\"Invalid device key\"}", response));
}

TEST_CASE("ack body lists uploads and command results")
{
    AckRequest request;
    request.uploaded_thumbs = {{100, 36, 0, 0}};
    request.commands = {{"c-1", true, ""}, {"c-2", false, "not supported"}};
    CHECK(build_ack_json(request) ==
          "{\"uploadedThumbs\":[{\"dir\":100,\"num\":36}],\"commands\":["
          "{\"id\":\"c-1\",\"state\":\"done\"},{\"id\":\"c-2\",\"state\":"
          "\"failed\",\"error\":\"not supported\"}]}");
}
