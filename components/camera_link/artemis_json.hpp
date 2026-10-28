#pragma once

// Tiny helpers for the small, flat JSON the camera returns. Not a general
// JSON parser — just enough to read known fields. Pure logic, host-testable.

#include <cstdint>
#include <string>

namespace camera::artemis
{

// Reads an integer field from a (flat) JSON object: finds "key" followed by
// ':' and an optionally signed integer. Returns false if the key is absent or
// not followed by an integer. The search starts at `from`, which lets callers
// scope the lookup to one object within a larger document.
bool json_find_int(const std::string &json, const std::string &key,
                   std::int64_t &out, std::size_t from = 0);

}  // namespace camera::artemis
