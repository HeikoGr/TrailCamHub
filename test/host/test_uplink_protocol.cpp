#include <doctest/doctest.h>

#include <string>

#include "uplink_protocol.hpp"

using namespace uplink;

TEST_CASE("check-in body lists every camera with its delta media")
{
    CheckinRequest request;
    request.hub_firmware = "0.5.0";

    CameraReport garden;
    garden.cam_id = "F4C15606ED3B";
    garden.read_at = 1766505900;
    garden.model = "TC100";
    garden.firmware = "2.3.0.3";
    garden.bat_percent = 78;
    garden.sd_free_mb = 28412;
    garden.media = {{100, 36, 0, 1766505692},
                    {100, 35, 2, 1766505600},
                    {100, 34, 7, 1766505500}};

    CameraReport meadow;
    meadow.cam_id = "A1B2C3D4E5F6";
    request.cameras = {garden, meadow};

    CHECK(build_checkin_json(request) ==
          "{\"hubFirmware\":\"0.5.0\",\"cameras\":["
          "{\"camId\":\"F4C15606ED3B\",\"readAt\":1766505900,"
          "\"model\":\"TC100\",\"firmware\":\"2.3.0.3\",\"batPercent\":78,"
          "\"sdFreeMB\":28412,\"media\":[{\"dir\":100,\"num\":36,"
          "\"fileType\":0,\"mediaTime\":1766505692},{\"dir\":100,\"num\":35,"
          "\"fileType\":2,\"mediaTime\":1766505600}]},"
          "{\"camId\":\"A1B2C3D4E5F6\",\"media\":[]}]}");
}

TEST_CASE("check-in body without cameras")
{
    CheckinRequest request;
    request.hub_firmware = "dev";
    request.hub_bat_percent = 91;
    CHECK(build_checkin_json(request) ==
          "{\"hubFirmware\":\"dev\",\"hubBatPercent\":91,\"cameras\":[]}");
}

TEST_CASE("strings are JSON-escaped")
{
    CHECK(json_escape("a\"b\\c\nd") == "a\\\"b\\\\c\\nd");
    CHECK(json_escape(std::string("\x01", 1)) == "\\u0001");
}

TEST_CASE("check-in response is read per camera")
{
    const std::string json =
        "{\"serverTime\":1766506000,\"cameras\":["
        "{\"camId\":\"F4C15606ED3B\",\"accepted\":true,\"resync\":false,"
        "\"thumbUploads\":[{\"dir\":100,\"num\":36,\"url\":\"https://bucket."
        "s3.eu-central-1.amazonaws.com/thumb/F4/100_36.jpg?X-Amz-Signature="
        "ab%2Fcd&x=1\"},{\"dir\":100,\"num\":35,\"url\":\"http://insecure."
        "example/\"}],\"commands\":[{\"id\":\"c-1\",\"type\":\"requestVideo\","
        "\"dir\":100,\"num\":35}]},"
        "{\"camId\":\"A1B2C3D4E5F6\",\"accepted\":true,\"resync\":true,"
        "\"thumbUploads\":[],\"commands\":[]},"
        "{\"camId\":\"OTHER\",\"accepted\":false,\"resync\":false,"
        "\"thumbUploads\":[],\"commands\":[]}],"
        "\"config\":{\"sleepIntervalS\":1800}}";

    CheckinResponse response;
    REQUIRE(parse_checkin_response(json, response));
    CHECK(response.server_time == 1766506000);
    CHECK(response.sleep_interval_s == 1800);
    REQUIRE(response.cameras.size() == 3);

    const CameraCheckin &garden = response.cameras[0];
    CHECK(garden.cam_id == "F4C15606ED3B");
    CHECK(garden.accepted);
    CHECK_FALSE(garden.resync);
    REQUIRE(garden.thumb_uploads.size() == 1);  // http URL rejected
    CHECK(garden.thumb_uploads[0].num == 36);
    CHECK(garden.thumb_uploads[0].url.find("X-Amz-Signature=ab%2Fcd") !=
          std::string::npos);
    REQUIRE(garden.commands.size() == 1);
    CHECK(garden.commands[0].id == "c-1");
    CHECK(garden.commands[0].num == 35);

    CHECK(response.cameras[1].resync);
    CHECK(response.cameras[1].thumb_uploads.empty());
    CHECK_FALSE(response.cameras[2].accepted);
}

TEST_CASE("brackets inside strings and entries without camId")
{
    CheckinResponse response;
    REQUIRE(parse_checkin_response(
        "{\"serverTime\":1,\"cameras\":[{\"accepted\":true},"
        "{\"camId\":\"a]b}\",\"commands\":[{\"id\":\"x]\",\"type\":\"t\"}]}]}",
        response));
    REQUIRE(response.cameras.size() == 1);
    CHECK(response.cameras[0].cam_id == "a]b}");
    REQUIRE(response.cameras[0].commands.size() == 1);
    CHECK(response.cameras[0].commands[0].id == "x]");
}

TEST_CASE("responses without serverTime are rejected")
{
    CheckinResponse response;
    CHECK_FALSE(
        parse_checkin_response("{\"error\":\"Invalid hub key\"}", response));
}

TEST_CASE("ack body is grouped by camera and skips empty cameras")
{
    AckRequest request;
    CameraAck garden;
    garden.cam_id = "F4C15606ED3B";
    garden.uploaded_thumbs = {{100, 36, 0, 0}};
    garden.commands = {{"c-1", true, ""}, {"c-2", false, "not supported"}};
    CameraAck idle;
    idle.cam_id = "A1B2C3D4E5F6";
    request.cameras = {garden, idle};

    CHECK_FALSE(ack_is_empty(request));
    CHECK(build_ack_json(request) ==
          "{\"cameras\":[{\"camId\":\"F4C15606ED3B\",\"uploadedThumbs\":["
          "{\"dir\":100,\"num\":36}],\"commands\":[{\"id\":\"c-1\",\"state\":"
          "\"done\"},{\"id\":\"c-2\",\"state\":\"failed\",\"error\":"
          "\"not supported\"}]}]}");

    AckRequest empty;
    empty.cameras = {idle};
    CHECK(ack_is_empty(empty));
}
