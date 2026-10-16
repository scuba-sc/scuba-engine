// SPDX-License-Identifier: GPL-3.0-only
#pragma once
// Asynchronous model intake: request by VFS path, read and decode on the job system, hand the
// finished ModelData to whoever uploads it (the renderer) on its own thread.
//
//   Request() ─► Queued ─► [worker] Reading (VFS) ─► Decoding ─► Ready ─┐
//                                     │ error           │ error         ├─► TakeCompleted()
//                                     └──────────► Failed ◄─────────────┘
//   Cancel() moves a not-yet-finished load to Cancelled; a worker that picks it up skips it.
//
// Every method is thread-safe. The loader must outlive nothing it hands out: ModelData is
// shared and immutable. Destroying the loader waits for running decodes to finish.
#include "scuba/asset/model.h"
#include "scuba/core/job_system.h"

#include <condition_variable>
#include <cstdint>
#include <memory>
#include <mutex>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace scuba::asset {

using ModelHandle = uint32_t;   // 0 is never valid

enum class LoadState : uint8_t { Queued, Reading, Decoding, Ready, Failed, Cancelled };
const char* LoadStateName(LoadState state);

struct LoadInfo {
    std::string path;
    LoadState state = LoadState::Failed;
    std::string error;
    uint64_t bytes = 0;
    double readMs = 0, decodeMs = 0;
};

class ModelLoader {
public:
    ModelLoader(const vfs::VirtualFileSystem& vfs, JobSystem& jobs, uint64_t maxFileBytes = uint64_t{512} << 20);
    ~ModelLoader();
    ModelLoader(const ModelLoader&) = delete;
    ModelLoader& operator=(const ModelLoader&) = delete;

    // A path already requested and not failed or cancelled returns the same handle.
    ModelHandle Request(std::string_view path, JobPriority priority = JobPriority::Normal);
    void Cancel(ModelHandle handle);

    LoadInfo Info(ModelHandle handle) const;
    std::shared_ptr<const ModelData> Get(ModelHandle handle) const;   // null unless Ready
    // Handles that became Ready or Failed since the last call, in completion order.
    std::vector<ModelHandle> TakeCompleted();
    size_t inFlight() const;

private:
    struct Slot;
    struct Shared;
    std::shared_ptr<Shared> shared_;
    JobSystem& jobs_;
};

}  // namespace scuba::asset
