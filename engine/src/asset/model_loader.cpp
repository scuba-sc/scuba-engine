// SPDX-License-Identifier: GPL-3.0-only
#include "scuba/asset/model_loader.h"
#include "scuba/core/log.h"
#include "scuba/vfs/virtual_file_system.h"

#include <chrono>

namespace scuba::asset {

struct ModelLoader::Slot {
    LoadInfo info;
    std::shared_ptr<const ModelData> model;
};

// Workers hold this, not the loader, so a job that outlives a Cancel stays valid; the destructor
// waits until no job is running before the VFS reference can go stale.
struct ModelLoader::Shared {
    const vfs::VirtualFileSystem& vfs;
    uint64_t maxFileBytes;
    mutable std::mutex mutex;
    std::condition_variable drained;
    std::unordered_map<ModelHandle, Slot> slots;
    std::unordered_map<std::string, ModelHandle> byPath;
    std::vector<ModelHandle> completed;
    ModelHandle next = 1;
    size_t running = 0;   // jobs submitted and not yet returned
    bool closing = false;

    Shared(const vfs::VirtualFileSystem& v, uint64_t limit) : vfs(v), maxFileBytes(limit) {}

    // Moves h to state unless it was cancelled meanwhile. False: stop working on it.
    bool Advance(ModelHandle h, LoadState state) {
        std::lock_guard<std::mutex> hold(mutex);
        auto it = slots.find(h);
        if (closing || it == slots.end() || it->second.info.state == LoadState::Cancelled) return false;
        it->second.info.state = state;
        return true;
    }

    void Finish(ModelHandle h, Status st, std::shared_ptr<const ModelData> model, uint64_t bytes, double readMs, double decodeMs) {
        std::lock_guard<std::mutex> hold(mutex);
        auto it = slots.find(h);
        if (it == slots.end() || it->second.info.state == LoadState::Cancelled) return;
        auto& info = it->second.info;
        info.bytes = bytes;
        info.readMs = readMs;
        info.decodeMs = decodeMs;
        if (st) {
            info.state = LoadState::Ready;
            it->second.model = std::move(model);
        } else {
            info.state = LoadState::Failed;
            info.error = st.error;
            byPath.erase(info.path);   // a later Request retries
        }
        completed.push_back(h);
    }

    void Work(ModelHandle h, std::string path) {
        using Clock = std::chrono::steady_clock;
        auto ms = [](Clock::time_point a, Clock::time_point b) { return std::chrono::duration<double, std::milli>(b - a).count(); };
        if (!Advance(h, LoadState::Reading)) return;
        const auto t0 = Clock::now();
        vfs::Bytes bytes;
        Status st = vfs.ReadAll(path, bytes, maxFileBytes);
        const auto t1 = Clock::now();
        if (!st) {
            Finish(h, st, nullptr, 0, ms(t0, t1), 0);
            return;
        }
        if (!Advance(h, LoadState::Decoding)) return;
        auto model = std::make_shared<ModelData>();
        st = DecodeModel(path, bytes, &vfs, *model);
        const auto t2 = Clock::now();
        if (st)
            log::Info("assets", path + ": " + std::to_string(model->meshes.size()) + " mesh(es), " +
                                    std::to_string(model->triangleCount()) + " triangles");
        else
            log::Warn("assets", st.error);
        Finish(h, st, st ? std::move(model) : nullptr, bytes.size(), ms(t0, t1), ms(t1, t2));
    }
};

const char* LoadStateName(LoadState state) {
    switch (state) {
    case LoadState::Queued: return "queued";
    case LoadState::Reading: return "reading";
    case LoadState::Decoding: return "decoding";
    case LoadState::Ready: return "ready";
    case LoadState::Failed: return "failed";
    case LoadState::Cancelled: return "cancelled";
    }
    return "?";
}

ModelLoader::ModelLoader(const vfs::VirtualFileSystem& vfs, JobSystem& jobs, uint64_t maxFileBytes)
    : shared_(std::make_shared<Shared>(vfs, maxFileBytes)), jobs_(jobs) {}

// Destroy the loader before the JobSystem: queued jobs must still run to release it.
ModelLoader::~ModelLoader() {
    std::unique_lock<std::mutex> hold(shared_->mutex);
    shared_->closing = true;
    shared_->drained.wait(hold, [this] { return shared_->running == 0; });
}

ModelHandle ModelLoader::Request(std::string_view path, JobPriority priority) {
    const std::string norm = vfs::NormalizePath(path);
    ModelHandle h = 0;
    {
        std::lock_guard<std::mutex> hold(shared_->mutex);
        if (shared_->closing) return 0;
        if (!norm.empty())
            if (auto it = shared_->byPath.find(norm); it != shared_->byPath.end()) return it->second;
        h = shared_->next++;
        Slot& slot = shared_->slots[h];
        slot.info.path = norm.empty() ? std::string(path) : norm;
        if (norm.empty()) {
            slot.info.state = LoadState::Failed;
            slot.info.error = "invalid path";
            shared_->completed.push_back(h);
            return h;
        }
        slot.info.state = LoadState::Queued;
        shared_->byPath[norm] = h;
        ++shared_->running;
    }
    jobs_.Submit([shared = shared_, h, norm] {
        shared->Work(h, norm);
        std::lock_guard<std::mutex> hold(shared->mutex);
        if (--shared->running == 0) shared->drained.notify_all();
    }, priority);
    return h;
}

void ModelLoader::Cancel(ModelHandle handle) {
    std::lock_guard<std::mutex> hold(shared_->mutex);
    auto it = shared_->slots.find(handle);
    if (it == shared_->slots.end()) return;
    auto& info = it->second.info;
    if (info.state == LoadState::Ready || info.state == LoadState::Failed || info.state == LoadState::Cancelled) return;
    info.state = LoadState::Cancelled;
    shared_->byPath.erase(info.path);
}

LoadInfo ModelLoader::Info(ModelHandle handle) const {
    std::lock_guard<std::mutex> hold(shared_->mutex);
    auto it = shared_->slots.find(handle);
    if (it == shared_->slots.end()) {
        LoadInfo none;
        none.error = "unknown handle";
        return none;
    }
    return it->second.info;
}

std::shared_ptr<const ModelData> ModelLoader::Get(ModelHandle handle) const {
    std::lock_guard<std::mutex> hold(shared_->mutex);
    auto it = shared_->slots.find(handle);
    return it == shared_->slots.end() ? nullptr : it->second.model;
}

std::vector<ModelHandle> ModelLoader::TakeCompleted() {
    std::lock_guard<std::mutex> hold(shared_->mutex);
    std::vector<ModelHandle> out;
    out.swap(shared_->completed);
    return out;
}

size_t ModelLoader::inFlight() const {
    std::lock_guard<std::mutex> hold(shared_->mutex);
    return shared_->running;
}

}  // namespace scuba::asset
