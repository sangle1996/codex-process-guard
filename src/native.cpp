#include "native.h"

#include <WinSock2.h>
#include <WS2tcpip.h>
#include <Windows.h>
#include <Iphlpapi.h>
#include <Psapi.h>
#include <TlHelp32.h>

#include <limits>
#include <vector>

namespace guard {

namespace {

struct Handle {
    HANDLE value{};
    explicit Handle(HANDLE handle) : value(handle) {}
    ~Handle() { if (value && value != INVALID_HANDLE_VALUE) CloseHandle(value); }
    Handle(const Handle&) = delete;
    Handle& operator=(const Handle&) = delete;
    explicit operator bool() const { return value && value != INVALID_HANDLE_VALUE; }
};

struct UnicodeString {
    USHORT length;
    USHORT maximum_length;
    PWSTR buffer;
};

using NtQueryInformationProcessFn = LONG(NTAPI*)(HANDLE, int, void*, ULONG, ULONG*);

std::uint64_t ticks(FILETIME time) {
    ULARGE_INTEGER value{};
    value.LowPart = time.dwLowDateTime;
    value.HighPart = time.dwHighDateTime;
    return value.QuadPart;
}

bool read_start_time(HANDLE process, std::uint64_t& started, bool* exited = nullptr) {
    FILETIME creation{}, exit{}, kernel{}, user{};
    if (!GetProcessTimes(process, &creation, &exit, &kernel, &user)) return false;
    started = ticks(creation);
    if (exited) *exited = exit.dwLowDateTime != 0 || exit.dwHighDateTime != 0;
    return true;
}

std::wstring read_command_line(HANDLE process) {
    static const auto query = reinterpret_cast<NtQueryInformationProcessFn>(
        GetProcAddress(GetModuleHandleW(L"ntdll.dll"), "NtQueryInformationProcess"));
    if (!query) return {};

    ULONG size = 0;
    query(process, 60, nullptr, 0, &size); // ProcessCommandLineInformation
    if (size < sizeof(UnicodeString) || size > 1024 * 1024) return {};
    std::vector<unsigned char> bytes(size + sizeof(wchar_t));
    if (query(process, 60, bytes.data(), size, &size) < 0) return {};

    const auto text = reinterpret_cast<const UnicodeString*>(bytes.data());
    const auto begin = reinterpret_cast<std::uintptr_t>(bytes.data());
    const auto end = begin + bytes.size();
    const auto pointer = reinterpret_cast<std::uintptr_t>(text->buffer);
    if (!text->buffer || text->length % sizeof(wchar_t) != 0 || pointer < begin || pointer > end || text->length > end - pointer) return {};
    return {text->buffer, text->length / sizeof(wchar_t)};
}

void append_tcp_owners(ULONG family, std::unordered_set<std::uint32_t>& owners) {
    ULONG size = 0;
    const DWORD first = GetExtendedTcpTable(nullptr, &size, FALSE, family, TCP_TABLE_OWNER_PID_ALL, 0);
    if (first != ERROR_INSUFFICIENT_BUFFER || size == 0) return;
    std::vector<unsigned char> bytes(size);
    if (GetExtendedTcpTable(bytes.data(), &size, FALSE, family, TCP_TABLE_OWNER_PID_ALL, 0) != NO_ERROR) return;
    if (family == AF_INET) {
        const auto table = reinterpret_cast<const MIB_TCPTABLE_OWNER_PID*>(bytes.data());
        for (DWORD index = 0; index < table->dwNumEntries; ++index) owners.insert(table->table[index].dwOwningPid);
    } else {
        const auto table = reinterpret_cast<const MIB_TCP6TABLE_OWNER_PID*>(bytes.data());
        for (DWORD index = 0; index < table->dwNumEntries; ++index) owners.insert(table->table[index].dwOwningPid);
    }
}

} // namespace

Snapshot read_process_snapshot() {
    Snapshot result;
    Handle snapshot(CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0));
    if (!snapshot) return result;

    PROCESSENTRY32W entry{sizeof(entry)};
    if (!Process32FirstW(snapshot.value, &entry)) return result;
    do {
        Handle process(OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, entry.th32ProcessID));
        std::uint64_t started = 0;
        if (!process || !read_start_time(process.value, started)) continue;
        PROCESS_MEMORY_COUNTERS_EX memory{sizeof(memory)};
        const bool working_set_known = GetProcessMemoryInfo(process.value, reinterpret_cast<PROCESS_MEMORY_COUNTERS*>(&memory), sizeof(memory)) != FALSE;
        const auto working_set = working_set_known ? static_cast<std::uint64_t>(memory.WorkingSetSize) : 0;
        result.emplace(entry.th32ProcessID, Process{{entry.th32ProcessID, started}, entry.th32ParentProcessID,
            entry.szExeFile, read_command_line(process.value), working_set, working_set_known});
    } while (Process32NextW(snapshot.value, &entry));
    return result;
}

IdentityStatus check_identity(Identity expected) {
    Handle process(OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, expected.pid));
    if (!process) return GetLastError() == ERROR_INVALID_PARAMETER ? IdentityStatus::missing : IdentityStatus::unknown;
    std::uint64_t started = 0;
    bool exited = false;
    if (!read_start_time(process.value, started, &exited)) return IdentityStatus::unknown;
    if (exited) return IdentityStatus::missing;
    return started == expected.started ? IdentityStatus::match : IdentityStatus::different;
}

bool terminate_exact(Identity expected) {
    Handle process(OpenProcess(PROCESS_TERMINATE | PROCESS_QUERY_LIMITED_INFORMATION | SYNCHRONIZE, FALSE, expected.pid));
    if (!process) return false;
    std::uint64_t started = 0;
    bool exited = false;
    if (!read_start_time(process.value, started, &exited) || exited || started != expected.started) return false;
    return TerminateProcess(process.value, 1) && WaitForSingleObject(process.value, 2000) == WAIT_OBJECT_0;
}

std::unordered_set<std::uint32_t> read_tcp_owner_pids() {
    std::unordered_set<std::uint32_t> owners;
    append_tcp_owners(AF_INET, owners);
    append_tcp_owners(AF_INET6, owners);
    owners.erase(0);
    return owners;
}

} // namespace guard
