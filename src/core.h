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
    bool working_set_known{};
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

struct TerminationEvidence {
    Identity helper;
    Identity owner;
    std::uint64_t working_set{};
    bool working_set_known{};
};

struct TerminationSummary {
    int confirmed{};
    std::uint64_t estimated_working_set{};
    int unknown_working_sets{};
    std::vector<TerminationEvidence> events;
};

struct EvidenceSnapshot {
    int managed{};
    int protected_live{};
    int pending{};
    int waiting_confirmation{};
    int eligible_preserved{};
    int unknown_working_sets{};
    int out_of_scope_codex{};
    int out_of_scope_unknown_working_sets{};
    std::uint64_t managed_working_set{};
    std::uint64_t protected_working_set{};
    std::uint64_t pending_working_set{};
    std::uint64_t waiting_working_set{};
    std::uint64_t eligible_working_set{};
    std::uint64_t out_of_scope_working_set{};
};

struct CleanupRecord {
    std::uint64_t timestamp{};
    std::uint64_t working_set{};
    bool working_set_known{};
    bool automatic{};
};

struct CleanupSummary {
    int confirmed{};
    int automatic{};
    int unknown_working_sets{};
    std::uint64_t observed_working_set{};
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
TerminationSummary terminate_candidates(std::vector<TrackedProcess>& tracked, const Snapshot& snapshot,
                                        const std::vector<Identity>& candidates,
                                        const std::function<IdentityStatus(Identity)>& check,
                                        const std::function<bool(Identity)>& terminate);
EvidenceSnapshot build_evidence(const std::vector<TrackedProcess>& tracked, const Snapshot& snapshot,
                                const IdentitySet& live_codex);
CleanupSummary summarize_cleanup(const std::vector<CleanupRecord>& records, std::uint64_t now,
                                 std::uint64_t window_seconds);
std::uint64_t saturating_add(std::uint64_t left, std::uint64_t right);

} // namespace guard
