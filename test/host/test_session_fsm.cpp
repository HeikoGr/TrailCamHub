#include <doctest/doctest.h>

#include <string>

#include "session_fsm.hpp"

using namespace session;

namespace
{

// BLE timeout, Wi-Fi join timeout, retry delay, attempts.
const Config kConfig = {90000, 30000, 5000, 3};

bool has(const Result &result, Action action)
{
    return (result.actions & action) != 0;
}

// Brings a state machine into the camera session state.
void reach_session(Fsm &fsm)
{
    fsm.handle(Event::scan_requested);
    fsm.handle(Event::credentials_received);
    fsm.handle(Event::wifi_connected);
    REQUIRE(fsm.state() == State::session);
}

}  // namespace

TEST_CASE("the hub stays idle until a scan is requested")
{
    Fsm fsm(kConfig);
    CHECK(fsm.state() == State::idle);
    CHECK_FALSE(fsm.busy());
    CHECK_FALSE(fsm.handle(Event::timeout).handled);
    CHECK_FALSE(fsm.handle(Event::credentials_received).handled);
    CHECK(fsm.state() == State::idle);
}

TEST_CASE("scan without uplink: camera session, then back to idle")
{
    Fsm fsm(kConfig);

    Result result = fsm.handle(Event::scan_requested);
    CHECK(result.next == State::ble_activation);
    CHECK(has(result, kResumeBle));
    CHECK(has(result, kStartTimer));
    CHECK(result.timer_ms == 90000);
    CHECK(fsm.attempt() == 1);

    result = fsm.handle(Event::credentials_received);
    CHECK(result.next == State::wifi_join);
    CHECK(has(result, kPauseBle));
    CHECK(has(result, kConnectCameraWifi));
    CHECK(result.timer_ms == 30000);

    result = fsm.handle(Event::wifi_connected);
    CHECK(result.next == State::session);
    CHECK(has(result, kCancelTimer));
    CHECK(has(result, kStartSession));

    result = fsm.handle(Event::session_ended);
    CHECK(result.next == State::idle);
    CHECK(has(result, kDisconnectWifi));
    CHECK(result.outcome == Outcome::camera_scanned);
    CHECK(fsm.attempt() == 0);
}

TEST_CASE("scan with uplink: camera session, home Wi-Fi, backend sync")
{
    Fsm fsm(kConfig);
    fsm.set_uplink_enabled(true);
    reach_session(fsm);

    Result result = fsm.handle(Event::session_ended);
    CHECK(result.next == State::uplink_join);
    CHECK(has(result, kDisconnectWifi));
    CHECK(has(result, kConnectHomeWifi));
    CHECK(has(result, kStartTimer));
    CHECK(result.outcome == Outcome::none);

    result = fsm.handle(Event::wifi_connected);
    CHECK(result.next == State::uplink);
    CHECK(has(result, kStartUplink));
    CHECK(has(result, kCancelTimer));

    result = fsm.handle(Event::uplink_done);
    CHECK(result.next == State::idle);
    CHECK(has(result, kDisconnectWifi));
    CHECK(result.outcome == Outcome::uplink_done);
}

TEST_CASE("upload only skips the camera")
{
    Fsm fsm(kConfig);
    CHECK_FALSE(fsm.handle(Event::upload_requested).handled);

    fsm.set_uplink_enabled(true);
    const Result result = fsm.handle(Event::upload_requested);
    CHECK(result.next == State::uplink_join);
    CHECK(has(result, kConnectHomeWifi));
    CHECK_FALSE(has(result, kResumeBle));
}

TEST_CASE("camera failures are retried, then the cycle gives up")
{
    for (Event failure : {Event::timeout, Event::wifi_failed}) {
        Fsm fsm(kConfig);
        fsm.handle(Event::scan_requested);
        for (std::uint32_t attempt = 1; attempt < 3; attempt++) {
            fsm.handle(Event::credentials_received);
            Result result = fsm.handle(failure);
            CHECK(result.next == State::backoff);
            CHECK(has(result, kDisconnectWifi));
            CHECK(result.timer_ms == 5000);

            result = fsm.handle(Event::timeout);
            CHECK(result.next == State::ble_activation);
            CHECK(has(result, kResumeBle));
            CHECK(fsm.attempt() == attempt + 1);
        }
        fsm.handle(Event::credentials_received);
        const Result result = fsm.handle(failure);
        CHECK(result.next == State::idle);
        CHECK(result.outcome == Outcome::camera_unreachable);
    }
}

TEST_CASE("BLE activation timeout counts as a failed attempt")
{
    Fsm fsm({90000, 30000, 5000, 1});
    fsm.handle(Event::scan_requested);
    const Result result = fsm.handle(Event::timeout);
    CHECK(result.next == State::idle);
    CHECK(has(result, kPauseBle));
    CHECK(result.outcome == Outcome::camera_unreachable);
}

TEST_CASE("an unreachable home Wi-Fi ends the cycle")
{
    for (Event failure : {Event::timeout, Event::wifi_failed}) {
        Fsm fsm(kConfig);
        fsm.set_uplink_enabled(true);
        reach_session(fsm);
        fsm.handle(Event::session_ended);
        const Result result = fsm.handle(failure);
        CHECK(result.next == State::idle);
        CHECK(has(result, kDisconnectWifi));
        CHECK(result.outcome == Outcome::uplink_unreachable);
    }
}

TEST_CASE("Wi-Fi loss during a session stops it before moving on")
{
    Fsm fsm(kConfig);
    reach_session(fsm);

    Result result = fsm.handle(Event::wifi_disconnected);
    CHECK(result.handled);
    CHECK(result.next == State::session);
    CHECK(has(result, kStopSession));
    CHECK(has(result, kDisconnectWifi));

    result = fsm.handle(Event::session_ended);
    CHECK(result.next == State::idle);
}

TEST_CASE("requests while busy are ignored")
{
    Fsm fsm(kConfig);
    reach_session(fsm);
    CHECK(fsm.busy());
    CHECK_FALSE(fsm.handle(Event::scan_requested).handled);
    CHECK_FALSE(fsm.handle(Event::upload_requested).handled);
    // New credentials from a BLE reconnect must not disturb the session.
    CHECK_FALSE(fsm.handle(Event::credentials_received).handled);
    CHECK_FALSE(fsm.handle(Event::timeout).handled);
    CHECK(fsm.state() == State::session);
}

TEST_CASE("zero attempts are treated as one")
{
    Fsm fsm({90000, 30000, 5000, 0});
    fsm.handle(Event::scan_requested);
    CHECK(fsm.handle(Event::timeout).next == State::idle);
}

TEST_CASE("state, event and outcome names")
{
    CHECK(std::string(to_string(State::uplink_join)) == "UPLINK_JOIN");
    CHECK(std::string(to_string(Event::scan_requested)) == "scan requested");
    CHECK(std::string(to_string(Outcome::camera_unreachable)) ==
          "camera-unreachable");
    CHECK(std::string(to_string(Event::stop_requested)) == "stop requested");
    CHECK(std::string(to_string(Outcome::stopped)) == "stopped");
}

TEST_CASE("a stop request in idle is ignored")
{
    Fsm fsm(kConfig);
    CHECK_FALSE(fsm.handle(Event::stop_requested).handled);
    CHECK(fsm.state() == State::idle);
}

TEST_CASE("stop while waking the camera returns to idle")
{
    Fsm fsm(kConfig);
    fsm.set_uplink_enabled(true);
    fsm.handle(Event::scan_requested);

    const Result result = fsm.handle(Event::stop_requested);
    CHECK(result.next == State::idle);
    CHECK(has(result, kCancelTimer));
    CHECK(has(result, kPauseBle));
    CHECK(result.outcome == Outcome::stopped);
    CHECK(fsm.attempt() == 0);
}

TEST_CASE("stop while joining the camera Wi-Fi disconnects")
{
    Fsm fsm(kConfig);
    fsm.handle(Event::scan_requested);
    fsm.handle(Event::credentials_received);

    const Result result = fsm.handle(Event::stop_requested);
    CHECK(result.next == State::idle);
    CHECK(has(result, kCancelTimer));
    CHECK(has(result, kDisconnectWifi));
    CHECK(result.outcome == Outcome::stopped);
}

TEST_CASE("stop during the camera session waits for its end, then skips "
          "the uplink")
{
    Fsm fsm(kConfig);
    fsm.set_uplink_enabled(true);
    reach_session(fsm);

    Result result = fsm.handle(Event::stop_requested);
    CHECK(result.handled);
    CHECK(result.next == State::session);
    CHECK(has(result, kStopSession));
    CHECK(has(result, kDisconnectWifi));
    CHECK(result.outcome == Outcome::none);
    CHECK(fsm.stop_pending());

    // A second request does not repeat the actions.
    result = fsm.handle(Event::stop_requested);
    CHECK(result.handled);
    CHECK(result.actions == kNone);

    result = fsm.handle(Event::session_ended);
    CHECK(result.next == State::idle);
    CHECK_FALSE(has(result, kConnectHomeWifi));
    CHECK(result.outcome == Outcome::stopped);
    CHECK_FALSE(fsm.stop_pending());
}

TEST_CASE("stop during the retry delay ends the cycle")
{
    Fsm fsm(kConfig);
    fsm.handle(Event::scan_requested);
    REQUIRE(fsm.handle(Event::timeout).next == State::backoff);

    const Result result = fsm.handle(Event::stop_requested);
    CHECK(result.next == State::idle);
    CHECK(has(result, kCancelTimer));
    CHECK(result.outcome == Outcome::stopped);
}

TEST_CASE("stop while joining the home Wi-Fi ends the cycle")
{
    Fsm fsm(kConfig);
    fsm.set_uplink_enabled(true);
    reach_session(fsm);
    REQUIRE(fsm.handle(Event::session_ended).next == State::uplink_join);

    const Result result = fsm.handle(Event::stop_requested);
    CHECK(result.next == State::idle);
    CHECK(has(result, kCancelTimer));
    CHECK(has(result, kDisconnectWifi));
    CHECK(result.outcome == Outcome::stopped);
}

TEST_CASE("a running backend sync is not interrupted by a stop request")
{
    Fsm fsm(kConfig);
    fsm.set_uplink_enabled(true);
    reach_session(fsm);
    fsm.handle(Event::session_ended);
    REQUIRE(fsm.handle(Event::wifi_connected).next == State::uplink);

    CHECK_FALSE(fsm.handle(Event::stop_requested).handled);
    const Result result = fsm.handle(Event::uplink_done);
    CHECK(result.next == State::idle);
    CHECK(result.outcome == Outcome::uplink_done);
}

TEST_CASE("a new scan after a stop starts fresh")
{
    Fsm fsm(kConfig);
    reach_session(fsm);
    fsm.handle(Event::stop_requested);
    fsm.handle(Event::session_ended);

    reach_session(fsm);
    CHECK_FALSE(fsm.stop_pending());
    CHECK(fsm.handle(Event::session_ended).outcome == Outcome::camera_scanned);
}
