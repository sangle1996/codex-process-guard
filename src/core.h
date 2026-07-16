#pragma once

#include <cstdint>
#include <functional>
#include <optional>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace guard {

struct Identity {
    std::uint32_t pid{};
    std::uint64_t started{};
    bool operator==(const Identity&) const = default;
};

struct IdentityHash {
    std::size_t operator()(const Identity& value) const noexcept;
};

struct Process {
    Identity identity;
    std::uint32_t parent_pid{};
    std::wstring name;
    std::wstring command_line;
    std::uint64_t working_set{};
};

using Snapshot = std::unordered_map<std::uint32_t, Process>;
using IdentitySet = std::unordered_set<Identity, IdentityHash>;

struct TrackedProcess {
    Identity helper;
    Identity owner;
    int orphan_observations{};
    std::wstring name;
};

enum class CohortDecision { owner_gone, newest_live, older_live };
enum class IdentityStatus { match, different, missing, unknown };

struct Cohort {
    std::uint64_t started{};
    Identity owner;
    std::vector<Identity> members;
    CohortDecision decision{};
};

struct TrackingUpdate {
    int live_attached{};
    int pending{};
    std::vector<Identity> termination_candidates;
};

std::optional<std::wstring> service_name(const Process& process);
bool is_managed_helper(const Process& process);
std::optional<Identity> find_codex_owner(const Process& process, const Snapshot& snapshot);
bool should_terminate(TrackedProcess& process, bool owner_alive);
std::vector<Cohort> build_cohorts(const std::vector<TrackedProcess>& tracked, const Snapshot& snapshot,
                                  const IdentitySet& live_codex);
const Cohort* find_exact_older(const std::vector<Cohort>& current, const Cohort& selected);
TrackingUpdate update_tracking(std::vector<TrackedProcess>& tracked, const Snapshot& snapshot, const IdentitySet& live_codex,
                               bool observe_orphans = true);
int terminate_candidates(std::vector<TrackedProcess>& tracked, const Snapshot& snapshot, const std::vector<Identity>& candidates,
                         const std::function<IdentityStatus(Identity)>& check,
                         const std::function<bool(Identity)>& terminate);

} // namespace guard
