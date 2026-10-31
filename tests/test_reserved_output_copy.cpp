#include "hostd/reserved_mock_host.h"
#include "reserved_copy_probe.h"

#include <chrono>
#include <condition_variable>
#include <cstdlib>
#include <future>
#include <iostream>
#include <mutex>
#include <string>
#include <thread>

namespace sco::reserved::curated_testing {
bool ReaderDrainObserved() noexcept;
}

namespace {
using namespace std::chrono_literals;
using namespace scuba::hostd;
using namespace sco::reserved;
constexpr auto kWait = 10s;

void Require(bool condition, const char* description) {
    if (!condition) { std::cerr << "FAIL: " << description << '\n'; std::exit(1); }
}

// Each gate has an independent arrival and release; failure is bounded even if
// an expected hook is never reached. Only the handler thread waits at a gate.
class Gate {
public:
    void Stop() noexcept {
        std::unique_lock<std::mutex> lock(mutex_);
        arrived_ = true;
        cv_.notify_all();
        if (!cv_.wait_for(lock, kWait, [&] { return released_; })) {
            std::cerr << "FAIL: timed out waiting for gate release\n";
            std::exit(1);
        }
    }
    void WaitForArrival(const char* description) {
        std::unique_lock<std::mutex> lock(mutex_);
        Require(cv_.wait_for(lock, kWait, [&] { return arrived_; }), description);
    }
    void Release() {
        std::lock_guard<std::mutex> lock(mutex_);
        released_ = true;
        cv_.notify_all();
    }
private:
    std::mutex mutex_;
    std::condition_variable cv_;
    bool arrived_ = false;
    bool released_ = false;
};

Gate copyGate;
Gate unwindGate;
bool early = false;
bool shouldThrow = false;

void WaitForReaderDrain() {
    const auto deadline = std::chrono::steady_clock::now() + kWait;
    while (!curated_testing::ReaderDrainObserved() && std::chrono::steady_clock::now() < deadline)
        std::this_thread::yield();
    Require(curated_testing::ReaderDrainObserved(), "retirement observed a reader while closing");
}

template<class T> void RequireReady(std::future<T>& result, const char* description) {
    Require(result.wait_for(kWait) == std::future_status::ready, description);
}

void Run() {
    auto& host = ReservedMockHost::Instance();
    Require(host.Start(), "start host");
    const auto& registry = host.registry();
    const auto put = registry.dispatch_text(R"({"id":1,"method":"mock.entity_put","params":{"id":"42","zone":"1","position":[1.25,2,3],"flags":9,"name":"Retained Diver"}})", {});
    auto putResponse = Json::parse(put);
    Require(putResponse && putResponse->find("result"), "insert entity");

    std::promise<std::string> responsePromise;
    auto response = responsePromise.get_future();
    std::thread caller([&] {
        try {
            responsePromise.set_value(registry.dispatch_text(R"({"id":2,"method":"mock.entity_get","params":{"id":"42"}})", {}));
        } catch (...) { responsePromise.set_exception(std::current_exception()); }
    });
    copyGate.WaitForArrival("handler reached name copy");
    Require(host.Close() == sco::Result::Ok, "close with copy in progress");

    std::promise<RetireResult> retirePromise;
    auto retirement = retirePromise.get_future();
    std::thread retirer([&] {
        retirePromise.set_value(host.RetireUntil(std::chrono::steady_clock::now() + 30s));
    });

    if (early) {
        // This is the negative control: the actual handler pin was reset before
        // EntityJson. The detector must observe retirement completing at the gate.
        RequireReady(retirement, "negative control retires while name copy is blocked");
        Require(retirement.get() == RetireResult::QuiescedRetained, "negative control quiesced while copy blocked");
        Require(!curated_testing::ReaderDrainObserved(), "negative control did not observe a reader");
    } else {
        WaitForReaderDrain();
        Require(retirement.wait_for(0s) == std::future_status::timeout,
                "retirement remains pending while name copy is blocked");
    }

    copyGate.Release();
    if (shouldThrow) {
        unwindGate.WaitForArrival("handler entered unwind destructor");
        if (!early) {
            Require(retirement.wait_for(0s) == std::future_status::timeout,
                    "retirement remains pending during exception unwind");
        }
        unwindGate.Release();
    }
    RequireReady(response, "handler response completes");
    const auto responseText = response.get();
    if (!early) {
        RequireReady(retirement, "retirement completes after handler pin leaves");
        Require(retirement.get() == RetireResult::QuiescedRetained, "retirement quiesced and retained");
    }
    caller.join();
    retirer.join();
    Require(host.phase() == Phase::QuiescedRetained, "terminal retained phase");
    Require(!host.Start(), "retained host refuses restart");
    auto refused = Json::parse(registry.dispatch_text(R"({"id":3,"method":"mock.scalar_snapshot"})", {}));
    Require(refused && refused->find("error"), "cached registry refuses after retirement");

    // Parse and inspect the actual RPC response only after retirement completes.
    auto parsed = Json::parse(responseText);
    Require(parsed.has_value(), "response owns parseable data after retirement");
    if (shouldThrow) {
        const auto* error = parsed->find("error");
        Require(error && error->find("message") && error->find("message")->as_string() == "copy failed: Retained Diver",
                "exception message owns copied name after retirement");
        const auto* data = error->find("data");
        Require(data && data->find("name") && data->find("name")->as_string() == "Retained Diver",
                "exception data owns copied entity after retirement");
    } else {
        const auto* result = parsed->find("result");
        Require(result && result->find("id") && result->find("id")->as_string() == "42" &&
                result->find("name") && result->find("name")->as_string() == "Retained Diver" &&
                result->find("position") && result->find("position")->at(0).as_number() == 1.25,
                "returned entity owns copied data after retirement");
    }
}
} // namespace

namespace scuba::hostd::reserved_copy_probe {
bool ReleasePinEarly() noexcept { return early; }
void BeforeNameCopy() { copyGate.Stop(); }
bool ThrowAfterCopy() noexcept { return shouldThrow; }
void DuringUnwind() noexcept { if (shouldThrow) unwindGate.Stop(); }
}

int main(int argc, char** argv) {
    if (argc != 2) return 2;
    const std::string mode = argv[1];
    if (mode == "return") {}
    else if (mode == "unwind") shouldThrow = true;
    else if (mode == "early_return") early = true;
    else if (mode == "early_unwind") { early = true; shouldThrow = true; }
    else return 2;
    Run();
}
