#include "hostd/designer_export.h"

#include <fstream>
#include <stdexcept>
#include <vector>

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#else
#include <cerrno>
#include <spawn.h>
#include <sys/wait.h>
extern char** environ;
#endif

namespace scuba::hostd {
namespace {

std::string RequiredString(const Json& params, const char* name) {
    const Json* value = params.find(name);
    if (!value || !value->is_string())
        throw HandlerError(rpc_error::kInvalidParams, std::string("Expected string: ") + name);
    return value->as_string();
}

bool ValidId(const std::string& id) {
    if (id.empty() || id.size() > 31 || id == "sco" || id == "host" || id == "menu" || id == "game") return false;
    for (const char c : id)
        if (!((c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '_')) return false;
    return true;
}

#ifdef _WIN32
std::wstring QuoteArgument(const std::wstring& text) {
    std::wstring out = L"\"";
    size_t slashes = 0;
    for (const wchar_t c : text) {
        if (c == L'\\') { ++slashes; continue; }
        out.append(c == L'"' ? slashes * 2 + 1 : slashes, L'\\');
        slashes = 0;
        out.push_back(c);
    }
    out.append(slashes * 2, L'\\');
    out.push_back(L'"');
    return out;
}
#endif

// Returns an empty string on success, otherwise why the scaffold tool did not succeed.
std::string RunScaffold(const DesignerExportOptions& options, const std::filesystem::path& folder,
                        const std::string& id, const std::string& language) {
    const std::vector<std::filesystem::path> args = {
        options.python, options.sdkRoot / "sdk/tools/sco-new-plugin.py",
        "--id", id, "--lang", language, "--author", "the maintainer", "--out", folder,
    };
#ifdef _WIN32
    std::wstring command;
    for (const auto& arg : args) {
        if (!command.empty()) command += L' ';
        command += QuoteArgument(arg.wstring());
    }
    STARTUPINFOW startup{};
    startup.cb = static_cast<DWORD>(sizeof(startup));
    startup.hStdInput = GetStdHandle(STD_INPUT_HANDLE);
    startup.hStdOutput = GetStdHandle(STD_OUTPUT_HANDLE);
    startup.hStdError = GetStdHandle(STD_ERROR_HANDLE);
    const bool redirected = startup.hStdInput && startup.hStdInput != INVALID_HANDLE_VALUE &&
        startup.hStdOutput && startup.hStdOutput != INVALID_HANDLE_VALUE &&
        startup.hStdError && startup.hStdError != INVALID_HANDLE_VALUE &&
        GetFileType(startup.hStdInput) != FILE_TYPE_UNKNOWN &&
        GetFileType(startup.hStdOutput) != FILE_TYPE_UNKNOWN &&
        GetFileType(startup.hStdError) != FILE_TYPE_UNKNOWN;
    if (redirected) startup.dwFlags = STARTF_USESTDHANDLES;
    PROCESS_INFORMATION process{};
    if (!CreateProcessW(options.python.c_str(), command.data(), nullptr, nullptr, redirected ? TRUE : FALSE,
                        CREATE_NO_WINDOW, nullptr, nullptr, &startup, &process))
        return "could not start " + options.python.string() + " (Windows error " + std::to_string(GetLastError()) + ")";
    const DWORD waited = WaitForSingleObject(process.hProcess, INFINITE);
    DWORD code = 1;
    const bool gotCode = GetExitCodeProcess(process.hProcess, &code) != FALSE;
    CloseHandle(process.hThread);
    CloseHandle(process.hProcess);
    if (waited != WAIT_OBJECT_0 || !gotCode) return "could not wait for the scaffold process";
    return code == 0 ? std::string() : "exit code " + std::to_string(code);
#else
    std::vector<std::string> storage;
    storage.reserve(args.size());
    for (const auto& arg : args) storage.push_back(arg.string());
    std::vector<char*> argv;
    argv.reserve(storage.size() + 1);
    for (auto& arg : storage) argv.push_back(arg.data());
    argv.push_back(nullptr);
    pid_t pid = 0;
    if (const int rc = posix_spawnp(&pid, argv[0], nullptr, nullptr, argv.data(), environ); rc != 0)
        return "could not start " + storage[0] + " (errno " + std::to_string(rc) + ")";
    int status = 0;
    pid_t waited = 0;
    do { waited = waitpid(pid, &status, 0); } while (waited < 0 && errno == EINTR);
    if (waited != pid || !WIFEXITED(status)) return "the scaffold process did not exit normally";
    return WEXITSTATUS(status) == 0 ? std::string() : "exit code " + std::to_string(WEXITSTATUS(status));
#endif
}

}  // namespace

Json ExportDesignerPlugin(const Json& params, const DesignerExportOptions& options) {
    const std::string id = RequiredString(params, "id");
    const std::string language = RequiredString(params, "language");
    const std::string source = RequiredString(params, "source");
    if (!ValidId(id) || (language != "c" && language != "cpp") || source.empty() ||
        source.size() > 1024 * 1024 || source.find('\0') != std::string::npos)
        throw HandlerError(rpc_error::kInvalidParams, "Expected a safe plugin id, c/cpp language, and nonempty source <=1 MiB");
    if (!std::filesystem::is_regular_file(options.sdkRoot / "sdk/tools/sco-new-plugin.py"))
        throw HandlerError(rpc_error::kUnavailable, "SDK scaffold tool unavailable");
    std::filesystem::create_directories(options.outputRoot);
    const auto folder = options.outputRoot / id;
    if (!std::filesystem::create_directory(folder))
        throw HandlerError(-32010, "Export folder already exists; choose a new plugin id");
    try {
        if (const std::string failure = RunScaffold(options, folder, id, language); !failure.empty())
            throw HandlerError(-32011, "SDK scaffold process failed: " + failure);
        const auto filename = id + (language == "cpp" ? ".cpp" : ".c");
        std::ofstream file(folder / filename, std::ios::binary | std::ios::trunc);
        file.exceptions(std::ios::badbit | std::ios::failbit);
        file.write(source.data(), static_cast<std::streamsize>(source.size()));
        file.close();
        Json out = Json::MakeObject();
        out.set("folder", folder.filename().generic_string());
        Json files = Json::MakeArray();
        files.push_back("plugin.ini"); files.push_back("CMakeLists.txt"); files.push_back(filename);
        out.set("files", std::move(files));
        return out;
    } catch (...) {
        std::error_code ignored;
        std::filesystem::remove_all(folder, ignored);
        throw;
    }
}

void RegisterDesignerHandlers(Registry& registry) {
    const DesignerExportOptions options{
        SCUBA_DESIGNER_SDK_ROOT, std::filesystem::current_path() / "exports", SCUBA_DESIGNER_PYTHON,
    };
    registry.register_handler("designer.export", [options](const Json& params, const CallContext&) {
        return ExportDesignerPlugin(params, options);
    });
}

}  // namespace scuba::hostd
