#include <doctest/doctest.h>

#include <string>

#include "session_fsm.hpp"

using namespace session;

namespace
{

const Config kConfig = {30000, 5000, 60000};

bool has(const Result &result, Action action)
{
    return (result.actions & action) != 0;
}

// Brings a state machine into the session state.
void reach_session(Fsm &fsm)
{
    fsm.handle(Event::start);
    fsm.handle(Event::credentials_received);
    fsm.handle(Event::wifi_connected);
    REQUIRE(fsm.state() == State::session);
}

}  // namespace

TEST_CASE("normal flow from start to session")
{
    Fsm fsm(kConfig);
    CHECK(fsm.state() == State::idle);

    Result result = fsm.handle(Event::start);
    CHECK(result.next == State::ble_activation);
    CHECK(result.actions == (kResumeBle | kStartTimer));
    CHECK(result.timer_ms == 120000);

    result = fsm.handle(Event::credentials_received);
    CHECK(result.next == State::wifi_join);
    CHECK(has(result, kPauseBle));
    CHECK(has(result, kConnectWifi));
    CHECK(has(result, kStartTimer));
    CHECK(result.timer_ms == 30000);

    result = fsm.handle(Event::wifi_connected);
    CHECK(result.next == State::session);
    CHECK(has(result, kCancelTimer));
    CHECK(has(result, kStartSession));
}

TEST_CASE("Wi-Fi join timeout or failure leads to backoff and reactivation")
{
    for (Event failure : {Event::timeout, Event::wifi_failed}) {
        Fsm fsm(kConfig);
        fsm.handle(Event::start);
        fsm.handle(Event::credentials_received);

        Result result = fsm.handle(failure);
        CHECK(result.next == State::backoff);
        CHECK(has(result, kDisconnectWifi));
        CHECK(has(result, kStartTimer));
        CHECK(result.timer_ms == 5000);

        result = fsm.handle(Event::timeout);
        CHECK(result.next == State::ble_activation);
        CHECK(result.actions == (kResumeBle | kStartTimer));
        CHECK(result.timer_ms == 120000);
    }
}

TEST_CASE("disconnects while joining are left to the Wi-Fi retries")
{
    Fsm fsm(kConfig);
    fsm.handle(Event::start);
    fsm.handle(Event::credentials_received);
    const Result result = fsm.handle(Event::wifi_disconnected);
    CHECK_FALSE(result.handled);
    CHECK(fsm.state() == State::wifi_join);
}

TEST_CASE("Wi-Fi loss during a session stops it before backing off")
{
    Fsm fsm(kConfig);
    reach_session(fsm);

    Result result = fsm.handle(Event::wifi_disconnected);
    CHECK(result.handled);
    CHECK(result.next == State::session);
    CHECK(has(result, kStopSession));
    CHECK(has(result, kDisconnectWifi));

    result = fsm.handle(Event::session_ended);
    CHECK(result.next == State::backoff);
    CHECK(has(result, kStartTimer));
}

TEST_CASE("retry delay doubles up to the maximum and resets after a session")
{
    Fsm fsm(kConfig);
    fsm.handle(Event::start);
    std::uint32_t delays[6] = {};
    for (std::uint32_t &delay : delays) {
        fsm.handle(Event::credentials_received);
        delay = fsm.handle(Event::timeout).timer_ms;
        fsm.handle(Event::timeout);  // backoff -> BLE activation
    }
    CHECK(delays[0] == 5000);
    CHECK(delays[1] == 10000);
    CHECK(delays[2] == 20000);
    CHECK(delays[3] == 40000);
    CHECK(delays[4] == 60000);
    CHECK(delays[5] == 60000);

    fsm.handle(Event::credentials_received);
    fsm.handle(Event::wifi_connected);
    CHECK(fsm.next_retry_delay_ms() == 5000);
    CHECK(fsm.handle(Event::session_ended).timer_ms == 5000);
}

TEST_CASE("unexpected events are ignored")
{
    Fsm fsm(kConfig);
    CHECK_FALSE(fsm.handle(Event::credentials_received).handled);
    CHECK_FALSE(fsm.handle(Event::timeout).handled);
    CHECK(fsm.state() == State::idle);

    reach_session(fsm);
    // New credentials from a BLE reconnect must not disturb the session.
    CHECK_FALSE(fsm.handle(Event::credentials_received).handled);
    CHECK_FALSE(fsm.handle(Event::start).handled);
    CHECK_FALSE(fsm.handle(Event::timeout).handled);
    CHECK(fsm.state() == State::session);
}

TEST_CASE("state and event names")
{
    CHECK(std::string(to_string(State::wifi_join)) == "WIFI_JOIN");
    CHECK(std::string(to_string(Event::session_ended)) == "session ended");
}

TEST_CASE("without auto start the machine waits for the user")
{
    Config config = kConfig;
    config.auto_start = false;
    Fsm fsm(config);

    Result result = fsm.handle(Event::start);
    CHECK(result.next == State::idle);
    CHECK(result.actions == kNone);

    result = fsm.handle(Event::user_start);
    CHECK(result.next == State::ble_activation);
    CHECK(result.actions == (kResumeBle | kStartTimer));
    CHECK(result.timer_ms == 120000);
}

TEST_CASE("user_start skips the backoff delay and resets it")
{
    Fsm fsm(kConfig);
    fsm.handle(Event::start);
    fsm.handle(Event::credentials_received);
    fsm.handle(Event::wifi_failed);
    REQUIRE(fsm.state() == State::backoff);

    Result result = fsm.handle(Event::user_start);
    CHECK(result.next == State::ble_activation);
    CHECK(has(result, kCancelTimer));
    CHECK(has(result, kResumeBle));
    CHECK(fsm.next_retry_delay_ms() == 5000);
}

TEST_CASE("a BLE activation that times out releases BLE and backs off")
{
    Fsm fsm(kConfig);
    fsm.handle(Event::start);

    Result result = fsm.handle(Event::timeout);
    CHECK(result.next == State::backoff);
    CHECK(has(result, kPauseBle));
    CHECK(has(result, kStartTimer));
    CHECK(result.timer_ms == 5000);

    result = fsm.handle(Event::timeout);
    CHECK(result.next == State::ble_activation);
    CHECK(has(result, kResumeBle));
    CHECK(result.timer_ms == 120000);

    // The delay grows like after other failures.
    result = fsm.handle(Event::timeout);
    CHECK(result.next == State::backoff);
    CHECK(result.timer_ms == 10000);
}

TEST_CASE("user_start during the BLE activation starts it over")
{
    Fsm fsm(kConfig);
    fsm.handle(Event::start);
    fsm.handle(Event::timeout);
    fsm.handle(Event::timeout);
    REQUIRE(fsm.state() == State::ble_activation);

    const Result result = fsm.handle(Event::user_start);
    CHECK(result.next == State::ble_activation);
    CHECK(result.actions == (kPauseBle | kResumeBle | kStartTimer));
    CHECK(result.timer_ms == 120000);
    CHECK(fsm.next_retry_delay_ms() == 5000);
}

TEST_CASE("user_stop returns to idle from every active state")
{
    {
        Fsm fsm(kConfig);
        fsm.handle(Event::start);
        Result result = fsm.handle(Event::user_stop);
        CHECK(result.next == State::idle);
        CHECK(has(result, kPauseBle));
        CHECK(has(result, kCancelTimer));
    }
    {
        Fsm fsm(kConfig);
        fsm.handle(Event::start);
        fsm.handle(Event::credentials_received);
        Result result = fsm.handle(Event::user_stop);
        CHECK(result.next == State::idle);
        CHECK(has(result, kCancelTimer));
        CHECK(has(result, kDisconnectWifi));
    }
    {
        Fsm fsm(kConfig);
        fsm.handle(Event::start);
        fsm.handle(Event::credentials_received);
        fsm.handle(Event::wifi_failed);
        Result result = fsm.handle(Event::user_stop);
        CHECK(result.next == State::idle);
        CHECK(has(result, kCancelTimer));
    }
    {
        Fsm fsm(kConfig);
        CHECK_FALSE(fsm.handle(Event::user_stop).handled);
    }
}

TEST_CASE("user_stop in a session ends in idle after the session ended")
{
    Fsm fsm(kConfig);
    reach_session(fsm);

    Result result = fsm.handle(Event::user_stop);
    CHECK(result.next == State::session);
    CHECK(has(result, kStopSession));
    CHECK(has(result, kDisconnectWifi));

    result = fsm.handle(Event::session_ended);
    CHECK(result.next == State::idle);
    CHECK(has(result, kDisconnectWifi));
}

TEST_CASE("a completed session ends in idle without auto start")
{
    Config config = kConfig;
    config.auto_start = false;
    Fsm fsm(config);
    fsm.handle(Event::user_start);
    fsm.handle(Event::credentials_received);
    fsm.handle(Event::wifi_connected);
    REQUIRE(fsm.state() == State::session);

    const Result result = fsm.handle(Event::session_completed);
    CHECK(result.next == State::idle);
    CHECK(has(result, kDisconnectWifi));
    CHECK_FALSE(has(result, kStartTimer));
}

TEST_CASE("a completed session backs off with auto start")
{
    Fsm fsm(kConfig);
    reach_session(fsm);
    CHECK(fsm.handle(Event::session_completed).next == State::backoff);
    CHECK(std::string(to_string(Event::session_completed)) ==
          "session completed");
}

TEST_CASE("a failed session backs off even without auto start")
{
    Config config = kConfig;
    config.auto_start = false;
    Fsm fsm(config);
    fsm.handle(Event::user_start);
    fsm.handle(Event::credentials_received);
    fsm.handle(Event::wifi_connected);
    CHECK(fsm.handle(Event::session_ended).next == State::backoff);
}
