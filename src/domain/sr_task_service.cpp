#include "domain/sr_task_service.h"

#include "util/logger.h"

#include <cstdio>
#include <utility>

namespace idiff {

SrTaskService::SrTaskService() = default;

SrTaskService::~SrTaskService() {
    // Best-effort: cancel anything still running so engines tear down
    // their worker threads cleanly before destructors run.  Engines
    // own their own joins, so we don't need to wait here.
    cancel_all();
}

SrStartResult SrTaskService::start(const SRTaskParams& params,
                                   const EngineFactory& factory) {
    SrStartResult result;
    result.ok = false;

    auto engine = factory ? factory() : nullptr;
    if (!engine) {
        result.error_title = "Super Resolution Error";
        result.error_message =
            "SR engine not available. "
            "Make sure the seedvr2-upscaler directory exists next to "
            "the executable or set the SEEDVR2_UPSCALER_PATH environment "
            "variable.";
        LOG_WARN("SrTaskService::start: engine factory returned null");
        return result;
    }
    if (!engine->start_inference(
            params.input_path, params.output_path,
            params.scale, params.tile_size, params.tile_overlap,
            params.model, params.color_correction)) {
        const auto err = engine->last_error();
        result.error_title = "Super Resolution Error";
        result.error_message = err.description;
        LOG_WARN("SrTaskService::start: %s", err.description.c_str());
        return result;
    }

    SrTask task;
    task.engine = std::move(engine);
    task.input_path = params.input_path.string();
    task.status_msg = "Super Resolution: processing " +
                      params.input_path.filename().string() + "...";
    LOG_INFO("SrTaskService::start: %s", task.status_msg.c_str());
    tasks_.push_back(std::move(task));

    result.ok = true;
    return result;
}

void SrTaskService::poll(std::vector<SrCompletion>& out_completions,
                         std::vector<SrFailure>& out_failures) {
    for (auto it = tasks_.begin(); it != tasks_.end(); ) {
        auto& task = *it;
        if (!task.engine) {
            it = tasks_.erase(it);
            continue;
        }

        const auto status = task.engine->get_status();

        if (status == SREngineStatus::Running) {
            const float progress = task.engine->get_progress();
            if (progress >= 0) {
                task.status_msg = "Super Resolution: processing " +
                    std::to_string(static_cast<int>(progress * 100)) + "%";
            }
            ++it;
            continue;
        }

        if (status == SREngineStatus::Completed) {
            const auto output_path = task.engine->get_output_path();
            const auto output_path_str = output_path.string();

            SrCompletion done;
            done.input_path = task.input_path;
            done.output_path = output_path_str;
            done.status_msg = "Super Resolution: completed " +
                              output_path.filename().string();
            LOG_INFO("SrTaskService::poll: %s", done.status_msg.c_str());
            out_completions.push_back(std::move(done));

            it = tasks_.erase(it);
            continue;
        }

        if (status == SREngineStatus::Failed) {
            const auto err = task.engine->last_error();

            SrFailure failed;
            failed.input_path = task.input_path;
            failed.description = err.description;
            failed.status_msg = "Super Resolution failed: " + err.description;
            LOG_WARN("SrTaskService::poll: %s", failed.status_msg.c_str());
            out_failures.push_back(std::move(failed));

            it = tasks_.erase(it);
            continue;
        }

        // Idle / Cancelled / unknown: leave in place; the engine will
        // either transition into Running or be torn down with the
        // service.
        ++it;
    }
}

void SrTaskService::cancel_all() {
    for (auto& task : tasks_) {
        if (task.engine) task.engine->cancel();
    }
}

std::size_t SrTaskService::running_count() const {
    std::size_t n = 0;
    for (const auto& task : tasks_) {
        if (task.engine &&
            task.engine->get_status() == SREngineStatus::Running) {
            ++n;
        }
    }
    return n;
}

} // namespace idiff
