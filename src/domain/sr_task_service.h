#ifndef IDIFF_DOMAIN_SR_TASK_SERVICE_H
#define IDIFF_DOMAIN_SR_TASK_SERVICE_H

// Super-resolution task service.
//
// Owns the lifecycle of every running SR inference job: spawning the
// engine, polling the engine's atomic status / progress, retiring
// completed or failed jobs, and exposing read-only views the UI needs
// (per-entry progress badge, status-bar text, quit-confirm dialog).
//
// The service deliberately does NOT know about App's library or
// selection.  poll() returns lightweight completion / failure events
// that App consumes to do the post-completion bookkeeping (load the
// output image, swap the selection to "input vs output", flag diff
// dirty, surface error dialogs, etc.).  This keeps the service free
// of UI- and view-model dependencies and makes it testable with a
// fake engine.
//
// Threading: the service is single-threaded from the caller's
// perspective.  Engines spawn their own worker threads and expose
// status/progress via atomics, so start / poll / cancel_all are all
// safe to call from the UI thread without any external locking.

#include "app/sr_dialog.h"        // SRTaskParams (POD)
#include "app/sr_infer_engine.h"  // SRInferEngine + SREngineStatus

#include <cstddef>
#include <functional>
#include <memory>
#include <string>
#include <vector>

namespace idiff {

// Snapshot of one task for the UI layer (image-list badge, status bar,
// quit-confirm dialog).  Always reflects the current engine state at
// the moment the caller iterates tasks(); copy the fields out before
// calling poll() if you need a stable snapshot.
struct SrTask {
    std::unique_ptr<SRInferEngine> engine;
    std::string input_path;  // Stable across image-list sorts.
    std::string status_msg;  // Last status string for the status bar.
};

// Returned by start().  When ok == false the caller should surface
// `error.title` / `error.message` to the user; the task was not added
// to the queue.
struct SrStartResult {
    bool ok = false;
    std::string error_title;
    std::string error_message;
};

// Emitted by poll() when an engine finishes successfully.  The output
// path comes straight from the engine; the input path is whatever the
// caller passed in via SRTaskParams.input_path.
struct SrCompletion {
    std::string input_path;
    std::string output_path;
    std::string status_msg;
};

// Emitted by poll() when an engine fails.  description holds the
// engine's last_error().description, formatted into a status-bar
// string in status_msg.
struct SrFailure {
    std::string input_path;
    std::string description;
    std::string status_msg;
};

class SrTaskService {
public:
    SrTaskService();
    ~SrTaskService();

    SrTaskService(const SrTaskService&) = delete;
    SrTaskService& operator=(const SrTaskService&) = delete;

    // Spawn an SR engine for `params` and append a task on success.
    // The engine_factory closure lets tests inject a fake engine
    // without touching SRInferEngineFactory's global registry; the
    // factory must return a fully-configured (but not-yet-started)
    // engine, or nullptr to signal "no engine available".
    using EngineFactory = std::function<std::unique_ptr<SRInferEngine>()>;
    SrStartResult start(const SRTaskParams& params,
                        const EngineFactory& factory);

    // Advance every task one step.  Successful completions and
    // failures are appended to the out vectors and the corresponding
    // task is erased from the queue; running tasks have their
    // status_msg updated in place; tasks whose engine pointer went
    // null are silently dropped.
    //
    // The out vectors are NOT cleared first -- callers can accumulate
    // across multiple poll() calls if they need to.
    void poll(std::vector<SrCompletion>& out_completions,
              std::vector<SrFailure>& out_failures);

    // Cancel every still-running engine.  Safe to call before
    // shutdown; idempotent.
    void cancel_all();

    // Number of tasks whose engine reports SREngineStatus::Running.
    std::size_t running_count() const;
    bool has_running() const { return running_count() > 0; }

    // Read-only iteration for the UI layer.  The returned reference is
    // stable until the next start() / poll() call.
    const std::vector<SrTask>& tasks() const noexcept { return tasks_; }
    std::size_t size() const noexcept { return tasks_.size(); }
    bool empty() const noexcept { return tasks_.empty(); }

private:
    std::vector<SrTask> tasks_;
};

} // namespace idiff

#endif // IDIFF_DOMAIN_SR_TASK_SERVICE_H
