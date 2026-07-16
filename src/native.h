#pragma once

#include "core.h"

namespace guard {

Snapshot read_process_snapshot();
IdentityStatus check_identity(Identity expected);
bool terminate_exact(Identity expected);
std::unordered_set<std::uint32_t> read_tcp_owner_pids();

} // namespace guard
