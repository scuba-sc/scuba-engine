#include "hostd/net_sim/process.h"
#include <chrono>
#include <condition_variable>
#include <deque>
#include <mutex>
#include <stdexcept>
#include <thread>
#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#else
#include <cerrno>
#include <csignal>
#include <fcntl.h>
#include <spawn.h>
#include <sys/wait.h>
#include <unistd.h>
extern char** environ;
#endif

namespace scuba::hostd::net_sim {
struct Process::Impl {
#ifdef _WIN32
    HANDLE input = nullptr, output = nullptr, process = nullptr;
    DWORD pid = 0;
#else
    int input = -1, output = -1;
    pid_t pid = -1;
#endif
    std::thread reader;
    std::mutex mutex;
    std::condition_variable changed;
    std::deque<Json> responses;
    bool ended = false;
    bool exited = false;
    bool unusable = false;

    void Read() {
        std::string line;
        char bytes[4096];
        for (;;) {
#ifdef _WIN32
            DWORD count = 0;
            if (!ReadFile(output, bytes, sizeof bytes, &count, nullptr) || count == 0) break;
#else
            const auto count = read(output, bytes, sizeof bytes);
            if (count < 0 && errno == EINTR) continue;
            if (count <= 0) break;
#endif
            bool invalid = false;
            for (size_t i = 0; i < static_cast<size_t>(count); ++i) {
                if (bytes[i] == '\n') {
                    auto response = Json::parse(line);
                    if (!response) { invalid = true; break; }
                    {
                        std::lock_guard lock(mutex);
                        responses.push_back(std::move(*response));
                    }
                    changed.notify_all();
                    line.clear();
                } else {
                    line += bytes[i];
                    if (line.size() > 4u * 1024u * 1024u) { invalid = true; break; }
                }
            }
            if (invalid) break;
        }
        {
            std::lock_guard lock(mutex);
            ended = true;
        }
        changed.notify_all();
    }

    ~Impl() {
#ifdef _WIN32
        if (input) CloseHandle(input);
        if (process) {
            if (WaitForSingleObject(process, 3000) == WAIT_TIMEOUT) {
                TerminateProcess(process, 1);
                WaitForSingleObject(process, INFINITE);
            }
            CloseHandle(process);
        }
#else
        if (input >= 0) close(input);
        if (pid > 0) {
            std::unique_lock lock(mutex);
            if (!changed.wait_for(lock, std::chrono::seconds(3), [this] { return exited; })) kill(pid, SIGKILL);
            lock.unlock();
        }
#endif
        if (reader.joinable()) reader.join();
#ifdef _WIN32
        if (output) CloseHandle(output);
#else
        if (output >= 0) close(output);
#endif
    }
};

#ifdef _WIN32
static std::wstring Quote(const std::wstring& value) {
    std::wstring out = L"\"";
    size_t slashes = 0;
    for (const wchar_t c : value) {
        if (c == L'\\') { ++slashes; continue; }
        out.append(slashes * (c == L'"' ? 2u : 1u), L'\\');
        slashes = 0;
        if (c == L'"') out += L'\\';
        out += c;
    }
    out.append(slashes * 2u, L'\\');
    return out + L'"';
}
#endif

Process::Process(const std::filesystem::path& executable, const std::vector<std::string>& args)
    : impl_(std::make_unique<Impl>()) {
    auto& state = *impl_;
#ifdef _WIN32
    SECURITY_ATTRIBUTES security{sizeof(SECURITY_ATTRIBUTES), nullptr, TRUE};
    HANDLE childInput = nullptr, childOutput = nullptr;
    if (!CreatePipe(&childInput, &state.input, &security, 0)) throw std::runtime_error("peer input pipe failed");
    if (!CreatePipe(&state.output, &childOutput, &security, 0)) {
        CloseHandle(childInput);
        throw std::runtime_error("peer output pipe failed");
    }
    SetHandleInformation(state.input, HANDLE_FLAG_INHERIT, 0);
    SetHandleInformation(state.output, HANDLE_FLAG_INHERIT, 0);
    std::wstring command = Quote(executable.wstring());
    for (const auto& arg : args) command += L" " + Quote(std::filesystem::path(std::u8string(arg.begin(), arg.end())).wstring());
    HANDLE childError = nullptr;
    const HANDLE stderrHandle = GetStdHandle(STD_ERROR_HANDLE);
    const bool hasStderr = stderrHandle && stderrHandle != INVALID_HANDLE_VALUE &&
                           GetFileType(stderrHandle) != FILE_TYPE_UNKNOWN;
    if (!hasStderr) childError = CreateFileW(L"NUL", GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE,
                                            &security, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if ((hasStderr && !DuplicateHandle(GetCurrentProcess(), stderrHandle, GetCurrentProcess(),
                                      &childError, 0, TRUE, DUPLICATE_SAME_ACCESS)) ||
        (!hasStderr && childError == INVALID_HANDLE_VALUE)) {
        CloseHandle(childInput); CloseHandle(childOutput);
        throw std::runtime_error("peer stderr handle failed");
    }
    SIZE_T bytes = 0;
    InitializeProcThreadAttributeList(nullptr, 1, 0, &bytes);
    std::vector<unsigned char> attributes(bytes);
    auto* list = reinterpret_cast<LPPROC_THREAD_ATTRIBUTE_LIST>(attributes.data());
    if (!InitializeProcThreadAttributeList(list, 1, 0, &bytes)) {
        CloseHandle(childInput); CloseHandle(childOutput); CloseHandle(childError);
        throw std::runtime_error("peer handle list failed");
    }
    HANDLE inherited[]{childInput, childOutput, childError};
    if (!UpdateProcThreadAttribute(list, 0, PROC_THREAD_ATTRIBUTE_HANDLE_LIST, inherited, sizeof inherited, nullptr, nullptr)) {
        DeleteProcThreadAttributeList(list);
        CloseHandle(childInput); CloseHandle(childOutput); CloseHandle(childError);
        throw std::runtime_error("peer handle isolation failed");
    }
    STARTUPINFOEXW startup{};
    startup.StartupInfo.cb = sizeof startup;
    startup.StartupInfo.dwFlags = STARTF_USESTDHANDLES;
    startup.StartupInfo.hStdInput = childInput;
    startup.StartupInfo.hStdOutput = childOutput;
    startup.StartupInfo.hStdError = childError;
    startup.lpAttributeList = list;
    PROCESS_INFORMATION info{};
    const bool created = CreateProcessW(executable.c_str(), command.data(), nullptr, nullptr, TRUE,
                                        CREATE_NO_WINDOW | EXTENDED_STARTUPINFO_PRESENT, nullptr, nullptr, &startup.StartupInfo, &info) != 0;
    DeleteProcThreadAttributeList(list);
    CloseHandle(childInput);
    CloseHandle(childOutput);
    CloseHandle(childError);
    if (!created) throw std::runtime_error("peer launch failed");
    state.process = info.hProcess;
    state.pid = info.dwProcessId;
    CloseHandle(info.hThread);
#else
    int inbound[2], outbound[2];
    if (pipe(inbound) != 0) throw std::runtime_error("peer input pipe failed");
    if (pipe(outbound) != 0) {
        close(inbound[0]); close(inbound[1]);
        throw std::runtime_error("peer output pipe failed");
    }
    for (const int fd : {inbound[0], inbound[1], outbound[0], outbound[1]}) fcntl(fd, F_SETFD, FD_CLOEXEC);
    state.input = inbound[1];
    state.output = outbound[0];
    std::vector<std::string> storage{executable.string()};
    storage.insert(storage.end(), args.begin(), args.end());
    std::vector<char*> argv;
    for (auto& arg : storage) argv.push_back(arg.data());
    argv.push_back(nullptr);
    posix_spawn_file_actions_t actions;
    posix_spawn_file_actions_init(&actions);
    posix_spawn_file_actions_adddup2(&actions, inbound[0], STDIN_FILENO);
    posix_spawn_file_actions_adddup2(&actions, outbound[1], STDOUT_FILENO);
    posix_spawnattr_t attributes;
    posix_spawnattr_init(&attributes);
#ifdef POSIX_SPAWN_CLOEXEC_DEFAULT
    posix_spawnattr_setflags(&attributes, POSIX_SPAWN_CLOEXEC_DEFAULT);
#endif
    const int rc = posix_spawn(&state.pid, executable.c_str(), &actions, &attributes, argv.data(), environ);
    posix_spawnattr_destroy(&attributes);
    posix_spawn_file_actions_destroy(&actions);
    close(inbound[0]); close(outbound[1]);
    if (rc != 0) { state.pid = -1; throw std::runtime_error("peer launch failed"); }
#endif
    state.reader = std::thread([&state] {
        state.Read();
#ifndef _WIN32
        int status = 0;
        while (waitpid(state.pid, &status, 0) < 0 && errno == EINTR) {}
        {
            std::lock_guard lock(state.mutex);
            state.exited = true;
        }
        state.changed.notify_all();
#endif
    });
}

Process::~Process() = default;
uint64_t Process::id() const { return static_cast<uint64_t>(impl_->pid); }

Json Process::Call(Json request) {
    auto& state = *impl_;
    {
        std::lock_guard lock(state.mutex);
        if (state.ended || state.unusable) throw std::runtime_error("peer control stream is closed");
    }
    const std::string line = request.dump() + '\n';
    size_t offset = 0;
    while (offset < line.size()) {
#ifdef _WIN32
        DWORD count = 0;
        if (!WriteFile(state.input, line.data() + offset, static_cast<DWORD>(line.size() - offset), &count, nullptr)) {
            state.unusable = true;
            throw std::runtime_error("peer disconnected");
        }
#else
        const auto count = write(state.input, line.data() + offset, line.size() - offset);
        if (count < 0 && errno == EINTR) continue;
        if (count <= 0) { state.unusable = true; throw std::runtime_error("peer disconnected"); }
#endif
        offset += static_cast<size_t>(count);
    }
    std::unique_lock lock(state.mutex);
    if (!state.changed.wait_for(lock, std::chrono::seconds(15), [&state] {
            return !state.responses.empty() || state.ended;
        })) {
        state.unusable = true;
        throw std::runtime_error("peer response timeout");
    }
    if (state.responses.empty()) throw std::runtime_error("peer exited");
    Json result = std::move(state.responses.front());
    state.responses.pop_front();
    if (const auto* error = result.find("error")) throw std::runtime_error(error->as_string());
    return result;
}
} // namespace scuba::hostd::net_sim
