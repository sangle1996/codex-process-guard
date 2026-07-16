#include "core.h"

#include <algorithm>
#include <array>
#include <cwctype>
#include <limits>
#include <stdexcept>

namespace guard {

namespace {

std::wstring lower(std::wstring value) {
    std::transform(value.begin(), value.end(), value.begin(), [](wchar_t ch) { return static_cast<wchar_t>(std::towlower(ch)); });
    return value;
}

bool contains(const std::wstring& value, const wchar_t* part) { return value.find(part) != std::wstring::npos; }

std::vector<std::wstring> arguments(const std::wstring& command) {
    std::vector<std::wstring> result;
    std::wstring current;
    bool quoted = false;
    for (const wchar_t character : command) {
        if (character == L'"') quoted = !quoted;
        else if (std::iswspace(character) && !quoted) {
            if (!current.empty()) { result.push_back(std::move(current)); current.clear(); }
        } else current.push_back(character);
    }
    if (!current.empty()) result.push_back(std::move(current));
    return result;
}

std::wstring entrypoint(const std::wstring& name, const std::wstring& command) {
    const auto values = arguments(command);
    if (values.size() < 2) return {};
    std::size_t index = 1;
    if ((name == L"python.exe" || name == L"pythonw.exe") && values[index] == L"-m") ++index;
    if (name == L"uv.exe" && values[index] == L"tool" && values.size() > index + 1 && values[index + 1] == L"run") index += 2;
    return index < values.size() ? lower(values[index]) : std::wstring{};
}

bool same_members(const std::vector<Identity>& left, const std::vector<Identity>& right) {
    if (left.size() != right.size()) return false;
    const IdentitySet left_values(left.begin(), left.end());
    const IdentitySet right_values(right.begin(), right.end());
    return left_values.size() == left.size() && right_values.size() == right.size() && left_values == right_values;
}

} // namespace

std::size_t IdentityHash::operator()(const Identity& value) const noexcept {
    return (static_cast<std::size_t>(value.pid) * 397U) ^ std::hash<std::uint64_t>{}(value.started);
}

std::optional<std::wstring> service_name(const Process& process) {
    static const std::array allowed{L"node.exe", L"node_repl.exe", L"python.exe", L"pythonw.exe", L"uv.exe", L"uvx.exe"};
    const auto name = lower(process.name);
    if (std::find(allowed.begin(), allowed.end(), name) == allowed.end()) return std::nullopt;
    if (name == L"node_repl.exe") return L"Node REPL";

    const auto command = entrypoint(name, process.command_line);
    if (contains(command, L"mongodb-mcp-server")) return L"MongoDB";
    if (contains(command, L"figma-developer-mcp")) return L"Figma";
    if (contains(command, L"@coding-solo\\godot-mcp") || contains(command, L"@coding-solo/godot-mcp") ||
        contains(command, L"godot_visual_mcp_server")) return L"Godot";
    if (contains(command, L"@sentry\\mcp-server")) return L"Sentry";
    if (contains(command, L"observability-mcp")) return L"Observability";
    if (contains(command, L"mcp-atlassian")) return L"Atlassian";
    if (contains(command, L"sonar-mcp")) return L"Sonar";
    if (contains(command, L"mcp-server-postgres")) return L"Postgres";
    if (contains(command, L"mcp-server-memory")) return L"Memory";
    if (contains(command, L"mcp-server-gitlab")) return L"GitLab";
    if (contains(command, L"codex_apps")) return L"Codex Apps";
    if (contains(command, L"@modelcontextprotocol")) return L"Model Context";
    if (contains(command, L"\\mcp\\server.mjs") || contains(command, L"/mcp/server.mjs")) return L"Custom MCP";
    if (contains(command, L"\\.codex\\tools\\")) return L"Codex tool";
    if (contains(command, L"\\.codex\\plugins\\")) return L"Codex plugin";
    return std::nullopt;
}

bool is_managed_helper(const Process& process) { return service_name(process).has_value(); }

std::optional<Identity> find_codex_owner(const Process& process, const Snapshot& snapshot) {
    std::unordered_set<std::uint32_t> seen;
    const Process* current = &process;
    for (int depth = 0; depth < 64 && current && seen.insert(current->identity.pid).second; ++depth) {
        if (lower(current->name) == L"codex.exe") return current->identity;
        const auto parent = snapshot.find(current->parent_pid);
        if (parent == snapshot.end() || parent->second.identity.started > current->identity.started) return std::nullopt;
        current = &parent->second;
    }
    return std::nullopt;
}

bool should_terminate(TrackedProcess& process, bool owner_alive) {
    if (owner_alive) {
        process.orphan_observations = 0;
        return false;
    }
    return ++process.orphan_observations >= 2;
}

std::vector<Cohort> build_cohorts(const std::vector<TrackedProcess>& tracked, const Snapshot& snapshot,
                                  const IdentitySet& live_codex) {
    constexpr std::uint64_t window = 8ULL * 10'000'000ULL;
    std::unordered_map<Identity, std::vector<TrackedProcess>, IdentityHash> owners;
    for (const auto& item : tracked) {
        const auto process = snapshot.find(item.helper.pid);
        if (process != snapshot.end() && process->second.identity == item.helper) owners[item.owner].push_back(item);
    }

    std::vector<Cohort> result;
    for (auto& [owner, members] : owners) {
        std::ranges::sort(members, [](const auto& left, const auto& right) {
            return left.helper.started == right.helper.started ? left.helper.pid < right.helper.pid : left.helper.started < right.helper.started;
        });
        std::vector<std::vector<TrackedProcess>> groups;
        for (const auto& item : members) {
            if (groups.empty() || item.helper.started - groups.back().front().helper.started > window) groups.emplace_back();
            groups.back().push_back(item);
        }
        const bool owner_alive = live_codex.contains(owner);
        for (std::size_t index = 0; index < groups.size(); ++index) {
            Cohort cohort{groups[index].front().helper.started, owner, {}, CohortDecision::owner_gone};
            for (const auto& item : groups[index]) cohort.members.push_back(item.helper);
            if (owner_alive) cohort.decision = index + 1 == groups.size() ? CohortDecision::newest_live : CohortDecision::older_live;
            result.push_back(std::move(cohort));
        }
    }
    std::ranges::sort(result, [](const auto& left, const auto& right) { return left.started > right.started; });
    return result;
}

const Cohort* find_exact_older(const std::vector<Cohort>& current, const Cohort& selected) {
    if (selected.decision != CohortDecision::older_live) return nullptr;
    const auto found = std::find_if(current.begin(), current.end(), [&](const Cohort& item) {
        return item.decision == CohortDecision::older_live && item.owner == selected.owner && same_members(item.members, selected.members);
    });
    return found == current.end() ? nullptr : &*found;
}

TrackingUpdate update_tracking(std::vector<TrackedProcess>& tracked, const Snapshot& snapshot, const IdentitySet& live_codex,
                               bool observe_orphans) {
    std::erase_if(tracked, [&](const TrackedProcess& item) {
        const auto current = snapshot.find(item.helper.pid);
        return current == snapshot.end() || current->second.identity != item.helper;
    });

    // ponytail: linear lookup is cheaper than another index at the expected tens-of-helpers scale.
    for (const auto& [pid, process] : snapshot) {
        static_cast<void>(pid);
        if (!is_managed_helper(process)) continue;
        const auto owner = find_codex_owner(process, snapshot);
        if (!owner) continue;
        const auto existing = std::find_if(tracked.begin(), tracked.end(), [&](const TrackedProcess& item) {
            return item.helper == process.identity;
        });
        if (existing == tracked.end()) tracked.push_back({process.identity, *owner, 0, process.name});
        else if (existing->owner != *owner) *existing = {process.identity, *owner, 0, process.name};
    }

    TrackingUpdate result;
    for (auto& item : tracked) {
        const bool owner_alive = live_codex.contains(item.owner);
        if (owner_alive) ++result.live_attached;
        else ++result.pending;
        if (owner_alive) should_terminate(item, true);
        else if (observe_orphans && should_terminate(item, false)) result.termination_candidates.push_back(item.helper);
    }
    return result;
}

TerminationSummary terminate_candidates(std::vector<TrackedProcess>& tracked, const Snapshot& snapshot,
                                        const std::vector<Identity>& candidates,
                                        const std::function<IdentityStatus(Identity)>& check,
                                        const std::function<bool(Identity)>& terminate) {
    TerminationSummary result;
    for (const auto candidate : candidates) {
        const auto tracked_item = std::find_if(tracked.begin(), tracked.end(), [&](const TrackedProcess& item) { return item.helper == candidate; });
        const auto current = snapshot.find(candidate.pid);
        if (tracked_item == tracked.end() || current == snapshot.end() || current->second.identity != candidate || !is_managed_helper(current->second)) continue;
        const auto owner_status = check(tracked_item->owner);
        if (owner_status == IdentityStatus::match) { tracked_item->orphan_observations = 0; continue; }
        if (owner_status == IdentityStatus::unknown || !terminate(candidate)) continue;
        ++result.confirmed;
        if (current->second.working_set_known) result.estimated_working_set = saturating_add(result.estimated_working_set, current->second.working_set);
        else ++result.unknown_working_sets;
        result.events.push_back({candidate, tracked_item->owner, current->second.working_set, current->second.working_set_known});
        tracked.erase(tracked_item);
    }
    return result;
}

EvidenceSnapshot build_evidence(const std::vector<TrackedProcess>& tracked, const Snapshot& snapshot,
                                const IdentitySet& live_codex) {
    EvidenceSnapshot result;
    for (const auto& item : tracked) {
        const auto process = snapshot.find(item.helper.pid);
        if (process == snapshot.end() || process->second.identity != item.helper) continue;
        ++result.managed;
        if (process->second.working_set_known) result.managed_working_set = saturating_add(result.managed_working_set, process->second.working_set);
        else ++result.unknown_working_sets;
        if (live_codex.contains(item.owner)) {
            ++result.protected_live;
            if (process->second.working_set_known) result.protected_working_set = saturating_add(result.protected_working_set, process->second.working_set);
        } else {
            ++result.pending;
            if (process->second.working_set_known) result.pending_working_set = saturating_add(result.pending_working_set, process->second.working_set);
            if (item.orphan_observations < 2) {
                ++result.waiting_confirmation;
                if (process->second.working_set_known) result.waiting_working_set = saturating_add(result.waiting_working_set, process->second.working_set);
            } else {
                ++result.eligible_preserved;
                if (process->second.working_set_known) result.eligible_working_set = saturating_add(result.eligible_working_set, process->second.working_set);
            }
        }
    }
    for (const auto& [pid, process] : snapshot) {
        static_cast<void>(pid);
        if (lower(process.name) == L"codex.exe" || is_managed_helper(process)) continue;
        const auto owner = find_codex_owner(process, snapshot);
        if (!owner || !live_codex.contains(*owner)) continue;
        ++result.out_of_scope_codex;
        if (process.working_set_known) result.out_of_scope_working_set = saturating_add(result.out_of_scope_working_set, process.working_set);
        else ++result.out_of_scope_unknown_working_sets;
    }
    return result;
}

std::uint64_t saturating_add(std::uint64_t left, std::uint64_t right) {
    return right > std::numeric_limits<std::uint64_t>::max() - left ? std::numeric_limits<std::uint64_t>::max() : left + right;
}

} // namespace guard
