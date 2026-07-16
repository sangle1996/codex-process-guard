#include "core.h"
#include "native.h"

#include <WinSock2.h>
#include <Windows.h>

#include <algorithm>
#include <functional>
#include <iostream>
#include <stdexcept>
#include <string>

namespace {

#define CHECK(expression) do { if (!(expression)) throw std::runtime_error(#expression); } while (false)

int failures = 0;

void run(const char* name, const std::function<void()>& test) {
    try {
        test();
        std::cout << "PASS " << name << '\n';
    } catch (const std::exception& error) {
        ++failures;
        std::cout << "FAIL " << name << ": " << error.what() << '\n';
    }
}

guard::Process process(std::uint32_t pid, std::uint32_t parent, std::wstring name,
                       std::wstring command, std::uint64_t started, std::uint64_t working_set = 0) {
    return {{pid, started}, parent, std::move(name), std::move(command), working_set, true};
}

void classifier_tests() {
    CHECK(guard::service_name(process(2, 1, L"node.exe", L"node C:\\cache\\@modelcontextprotocol\\server-postgres\\index.js", 20)) == L"Model Context");
    CHECK(guard::service_name(process(3, 1, L"node_repl.exe", L"", 30)) == L"Node REPL");
    CHECK(guard::is_managed_helper(process(4, 1, L"node.exe", L"node mongodb-mcp-server", 40)));
    CHECK(!guard::is_managed_helper(process(5, 1, L"node.exe", L"node next dev", 50)));
    CHECK(!guard::is_managed_helper(process(6, 1, L"chrome.exe", L"mongodb-mcp-server", 60)));
    CHECK(!guard::is_managed_helper(process(7, 1, L"codex.exe", L"codex app-server", 70)));
    CHECK(!guard::is_managed_helper(process(8, 1, L"node.exe", L"node server.js mongodb-mcp-server", 80)));
    CHECK(!guard::is_managed_helper(process(9, 1, L"node.exe", L"\"C:\\mongodb-mcp-server\\node.exe\" next dev", 90)));
    CHECK(guard::is_managed_helper(process(10, 1, L"node.exe", L"\"C:\\Program Files\\node.exe\" ./mcp/server.mjs", 100)));
}

void lineage_tests() {
    auto codex = process(10, 1, L"codex.exe", L"codex app-server", 100);
    auto wrapper = process(11, 10, L"cmd.exe", L"npx mcp-server", 110);
    auto helper = process(12, 11, L"node.exe", L"node mcp-server", 120);
    guard::Snapshot snapshot{{10, codex}, {11, wrapper}, {12, helper}};
    CHECK(guard::find_codex_owner(helper, snapshot) == codex.identity);

    snapshot[10] = process(10, 1, L"codex.exe", L"codex app-server", 999);
    CHECK(!guard::find_codex_owner(helper, snapshot));
}

void orphan_policy_tests() {
    guard::TrackedProcess tracked{{12, 120}, {10, 100}, 1, L"node.exe"};
    CHECK(!guard::should_terminate(tracked, true));
    CHECK(tracked.orphan_observations == 0);
    CHECK(!guard::should_terminate(tracked, false));
    CHECK(tracked.orphan_observations == 1);
    CHECK(guard::should_terminate(tracked, false));
    CHECK(tracked.orphan_observations == 2);
}

void cohort_tests() {
    constexpr std::uint64_t second = 10'000'000;
    const guard::Identity owner{20, 90};
    auto first = process(21, 20, L"node.exe", L"mongodb-mcp-server", 100, 100);
    auto second_member = process(22, 20, L"cmd.exe", L"mongodb-mcp-server", 100 + 2 * second, 10);
    auto newest = process(23, 20, L"node.exe", L"mongodb-mcp-server", 100 + 12 * second, 120);
    guard::Snapshot snapshot{{21, first}, {22, second_member}, {23, newest}};
    std::vector<guard::TrackedProcess> tracked{
        {first.identity, owner, 0, first.name},
        {second_member.identity, owner, 0, second_member.name},
        {newest.identity, owner, 0, newest.name}
    };
    guard::IdentitySet live{owner};

    auto cohorts = guard::build_cohorts(tracked, snapshot, live);
    CHECK(cohorts.size() == 2);
    CHECK(cohorts[0].decision == guard::CohortDecision::newest_live);
    CHECK(cohorts[1].decision == guard::CohortDecision::older_live);
    CHECK(cohorts[1].members.size() == 2);
    CHECK(guard::find_exact_older(cohorts, cohorts[1]) == &cohorts[1]);
    CHECK(guard::find_exact_older(cohorts, cohorts[0]) == nullptr);

    auto reordered = cohorts[1];
    std::reverse(reordered.members.begin(), reordered.members.end());
    CHECK(guard::find_exact_older(cohorts, reordered) == &cohorts[1]);
    reordered.members[0] = {99, 99};
    CHECK(guard::find_exact_older(cohorts, reordered) == nullptr);
    auto duplicated = cohorts[1];
    duplicated.members[1] = duplicated.members[0];
    CHECK(guard::find_exact_older(cohorts, duplicated) == nullptr);
    auto wrong_owner = cohorts[1];
    wrong_owner.owner = {98, 98};
    CHECK(guard::find_exact_older(cohorts, wrong_owner) == nullptr);
}

void native_snapshot_and_identity_tests() {
    const auto pid = GetCurrentProcessId();
    const auto snapshot = guard::read_process_snapshot();
    const auto self = snapshot.find(pid);
    CHECK(self != snapshot.end());
    CHECK(self->second.identity.started != 0);
    CHECK(self->second.working_set != 0);
    if (self->second.name != L"guard_tests.exe") std::wcerr << L"snapshot self name: " << self->second.name << L'\n';
    CHECK(self->second.name == L"guard_tests.exe");
    CHECK(self->second.command_line.find(L"guard_tests") != std::wstring::npos);
    CHECK(guard::check_identity(self->second.identity) == guard::IdentityStatus::match);
    CHECK(guard::check_identity({pid, self->second.identity.started + 1}) == guard::IdentityStatus::different);
}

void tcp_owner_test() {
    WSADATA data{};
    CHECK(WSAStartup(MAKEWORD(2, 2), &data) == 0);
    const SOCKET listener = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    CHECK(listener != INVALID_SOCKET);
    sockaddr_in address{};
    address.sin_family = AF_INET;
    address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    CHECK(bind(listener, reinterpret_cast<const sockaddr*>(&address), sizeof(address)) == 0);
    CHECK(listen(listener, 1) == 0);
    const auto owners = guard::read_tcp_owner_pids();
    CHECK(owners.contains(GetCurrentProcessId()));
    closesocket(listener);
    WSACleanup();
}

void exact_child_termination_test() {
    wchar_t command[] = L"C:\\Windows\\System32\\ping.exe -n 30 127.0.0.1";
    STARTUPINFOW startup{sizeof(startup)};
    PROCESS_INFORMATION child{};
    CHECK(CreateProcessW(nullptr, command, nullptr, nullptr, FALSE, CREATE_NO_WINDOW, nullptr, nullptr, &startup, &child));
    CloseHandle(child.hThread);
    try {
        guard::Identity identity{};
        for (int attempt = 0; attempt < 20 && identity.pid == 0; ++attempt) {
            const auto snapshot = guard::read_process_snapshot();
            const auto found = snapshot.find(child.dwProcessId);
            if (found != snapshot.end()) identity = found->second.identity;
            else Sleep(25);
        }
        CHECK(identity.pid == child.dwProcessId);
        CHECK(!guard::terminate_exact({identity.pid, identity.started + 1}));
        CHECK(WaitForSingleObject(child.hProcess, 0) == WAIT_TIMEOUT);
        CHECK(guard::terminate_exact(identity));
        CHECK(WaitForSingleObject(child.hProcess, 0) == WAIT_OBJECT_0);
        CHECK(guard::check_identity(identity) == guard::IdentityStatus::missing);
    } catch (...) {
        TerminateProcess(child.hProcess, 99);
        CloseHandle(child.hProcess);
        throw;
    }
    CloseHandle(child.hProcess);
}

void tracking_state_machine_test() {
    auto owner = process(30, 1, L"codex.exe", L"codex app-server", 100);
    auto helper = process(31, 30, L"node.exe", L"node mongodb-mcp-server", 110);
    auto project_server = process(32, 30, L"node.exe", L"node next dev", 120);
    guard::Snapshot live_snapshot{{30, owner}, {31, helper}, {32, project_server}};
    guard::IdentitySet live_owners{owner.identity};
    std::vector<guard::TrackedProcess> tracked;

    auto live = guard::update_tracking(tracked, live_snapshot, live_owners);
    CHECK(tracked.size() == 1);
    CHECK(tracked[0].helper == helper.identity);
    CHECK(live.live_attached == 1);
    CHECK(live.pending == 0);
    CHECK(live.termination_candidates.empty());

    guard::Snapshot orphan_snapshot{{31, helper}, {32, project_server}};
    auto first_missing = guard::update_tracking(tracked, orphan_snapshot, {});
    CHECK(first_missing.pending == 1);
    CHECK(first_missing.termination_candidates.empty());
    CHECK(tracked[0].orphan_observations == 1);

    auto refresh_only = guard::update_tracking(tracked, orphan_snapshot, {}, false);
    CHECK(refresh_only.pending == 1);
    CHECK(refresh_only.termination_candidates.empty());
    CHECK(tracked[0].orphan_observations == 1);

    auto second_missing = guard::update_tracking(tracked, orphan_snapshot, {});
    CHECK(second_missing.termination_candidates == std::vector<guard::Identity>{helper.identity});

    auto live_again = guard::update_tracking(tracked, live_snapshot, live_owners);
    CHECK(live_again.termination_candidates.empty());
    CHECK(tracked[0].orphan_observations == 0);
}

void destructive_coordinator_test() {
    const auto owner = process(40, 1, L"codex.exe", L"codex app-server", 400);
    const auto helper = process(41, 40, L"node.exe", L"node mongodb-mcp-server", 410, 64 * 1024 * 1024);
    guard::Snapshot live_snapshot{{40, owner}, {41, helper}};
    guard::Snapshot orphan_snapshot{{41, helper}};
    std::vector<guard::TrackedProcess> tracked;
    const guard::IdentitySet live_owners{owner.identity};
    CHECK(guard::update_tracking(tracked, live_snapshot, live_owners).termination_candidates.empty());
    CHECK(guard::update_tracking(tracked, orphan_snapshot, {}).termination_candidates.empty());
    const auto candidates = guard::update_tracking(tracked, orphan_snapshot, {}).termination_candidates;
    bool terminated = false;
    CHECK(guard::terminate_candidates(tracked, orphan_snapshot, candidates,
        [](guard::Identity) { return guard::IdentityStatus::match; }, [&](guard::Identity) { terminated = true; return true; }).confirmed == 0);
    CHECK(!terminated && tracked[0].orphan_observations == 0);

    tracked[0].orphan_observations = 2;
    CHECK(guard::terminate_candidates(tracked, orphan_snapshot, candidates,
        [](guard::Identity) { return guard::IdentityStatus::unknown; }, [&](guard::Identity) { terminated = true; return true; }).confirmed == 0);
    CHECK(!terminated && tracked.size() == 1);
    const auto result = guard::terminate_candidates(tracked, orphan_snapshot, candidates,
        [](guard::Identity) { return guard::IdentityStatus::missing; }, [&](guard::Identity identity) { terminated = identity == helper.identity; return true; });
    CHECK(result.confirmed == 1);
    CHECK(result.estimated_working_set == helper.working_set);
    CHECK(result.events.size() == 1);
    CHECK(result.events[0].helper == helper.identity && result.events[0].owner == owner.identity);
    CHECK(result.events[0].working_set == helper.working_set);
    CHECK(terminated && tracked.empty());
}

void evidence_snapshot_test() {
    const guard::Identity live_owner{50, 500};
    const guard::Identity missing_owner{51, 510};
    const auto live_helper = process(52, 50, L"node.exe", L"node mongodb-mcp-server", 520, 100 * 1024 * 1024);
    const auto pending_helper = process(53, 51, L"node.exe", L"node mongodb-mcp-server", 530, 40 * 1024 * 1024);
    auto unknown_helper = process(54, 50, L"node.exe", L"node mongodb-mcp-server", 540);
    unknown_helper.working_set_known = false;
    const auto retry_helper = process(55, 51, L"node.exe", L"node mongodb-mcp-server", 550, 20 * 1024 * 1024);
    const auto ignored_child = process(56, 50, L"node.exe", L"node next dev", 560, 30 * 1024 * 1024);
    const guard::Snapshot snapshot{{50, process(50, 1, L"codex.exe", L"codex app-server", 500)},
                                   {52, live_helper}, {53, pending_helper}, {54, unknown_helper},
                                   {55, retry_helper}, {56, ignored_child}};
    const std::vector<guard::TrackedProcess> tracked{
        {live_helper.identity, live_owner, 0, live_helper.name},
        {pending_helper.identity, missing_owner, 1, pending_helper.name},
        {unknown_helper.identity, live_owner, 0, unknown_helper.name},
        {retry_helper.identity, missing_owner, 2, retry_helper.name}
    };
    const auto evidence = guard::build_evidence(tracked, snapshot, guard::IdentitySet{live_owner});
    CHECK(evidence.managed == 4 && evidence.managed_working_set == 160 * 1024 * 1024);
    CHECK(evidence.unknown_working_sets == 1);
    CHECK(evidence.protected_live == 2 && evidence.protected_working_set == 100 * 1024 * 1024);
    CHECK(evidence.waiting_confirmation == 1 && evidence.waiting_working_set == 40 * 1024 * 1024);
    CHECK(evidence.eligible_preserved == 1 && evidence.eligible_working_set == 20 * 1024 * 1024);
    CHECK(evidence.out_of_scope_codex == 1 && evidence.out_of_scope_working_set == 30 * 1024 * 1024);
    CHECK(guard::saturating_add(UINT64_MAX, 1) == UINT64_MAX);
}

} // namespace

int main() {
    run("classifier allows only known Codex helpers", classifier_tests);
    run("lineage rejects reused parent PID", lineage_tests);
    run("orphan policy requires two missing-owner scans", orphan_policy_tests);
    run("cohort matching is exact and order independent", cohort_tests);
    run("native snapshot reads self command line and exact identity", native_snapshot_and_identity_tests);
    run("native TCP table reports this process listener", tcp_owner_test);
    run("native termination rejects wrong identity and kills owned child", exact_child_termination_test);
    run("tracking never kills live owners and requires two orphan scans", tracking_state_machine_test);
    run("destructive coordinator fails closed and kills only exact candidates", destructive_coordinator_test);
    run("evidence reports only exact currently observed helpers", evidence_snapshot_test);
    return failures == 0 ? 0 : 1;
}
