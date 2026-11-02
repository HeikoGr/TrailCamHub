#pragma once

#include "esp_err.h"

/*
 * Requests camera reads and backend check-ins when the schedule says they are
 * due (see schedule_state.hpp) through the session controller, follows their
 * outcome, and with CONFIG_POWER_DEEP_SLEEP puts the hub into deep sleep until
 * the next one when no one is using it. Without a camera plan and check-in
 * interval (the on-site profile) it does nothing on its own.
 *
 * Call after session_controller_start(); schedule::init() must have run.
 */
esp_err_t scheduler_start(void);
