#include "app/controller.h"

#include "app/sr_dialog.h"
#include "app/sr_infer_engine.h"
#include "app/sr_infer_engine_factory.h"
#include "app/status_reporter.h"
#include "domain/comparison_config_service.h"
#include "domain/diff_service.h"
#include "domain/image_library.h"
#include "domain/selection_model.h"
#include "domain/sr_task_service.h"
#include "domain/timeline_model.h"

#include <string>
#include <string_view>
#include <unordered_map>
#include <utility>

namespace idiff {

AppController::AppController(ITextureUploader& texture_uploader,
                             IStatusReporter& status_reporter)
    : library_(std::make_unique<ImageLibrary>(texture_uploader)),
      selection_(std::make_unique<SelectionModel>()),
      timeline_(std::make_unique<TimelineModel>()),
      diff_(std::make_unique<DiffService>(texture_uploader)),
      sr_tasks_(std::make_unique<SrTaskService>()),
      comparison_config_(std::make_unique<ComparisonConfigService>()),
      status_reporter_(&status_reporter) {}

AppController::~AppController() = default;

ImageLibrary& AppController::library() noexcept { return *library_; }
SelectionModel& AppController::selection() noexcept { return *selection_; }
TimelineModel& AppController::timeline() noexcept { return *timeline_; }
DiffService& AppController::diff() noexcept { return *diff_; }
SrTaskService& AppController::sr_tasks() noexcept { return *sr_tasks_; }
ComparisonConfigService& AppController::comparison_config() noexcept {
    return *comparison_config_;
}

namespace {

// Split a filename into (stem, extension) at the last '.' that is not
// the leading character.  A name with no extension or a single-char
// name returns the whole string as the stem and an empty extension.
std::pair<std::string_view, std::string_view>
split_stem_ext(std::string_view name) {
    if (name.size() <= 1) return {name, {}};
    auto dot = name.find_last_of('.');
    if (dot == std::string_view::npos || dot == 0) return {name, {}};
    return {name.substr(0, dot), name.substr(dot + 1)};
}

// Compare filenames by (stem, extension) so that a "base" name like
// "kid.jpg" sorts before its derived siblings ("kid-pisa.jpg",
// "kid-pisa-v0.jpg").  The default std::string operator< puts '-'
// (0x2D) before '.' (0x2E) which is the wrong answer here: the user
// expects the shorter root name to come first.
bool filename_less(const std::string& a, const std::string& b) {
    auto [a_stem, a_ext] = split_stem_ext(a);
    auto [b_stem, b_ext] = split_stem_ext(b);
    if (a_stem != b_stem) return a_stem < b_stem;
    return a_ext < b_ext;
}

} // namespace

void AppController::get_ab_indices(int& a_idx, int& b_idx) const {
    selection_->get_ab_indices(a_idx, b_idx);
}

int AppController::timeline_length() const {
    return TimelineModel::length(library_->all());
}

void AppController::compute_display_labels() {
    auto& entries = library_->all();
    if (entries.empty()) return;

    std::unordered_map<std::string, int> name_counts;
    for (const auto& entry : entries) {
        name_counts[entry.filename]++;
    }

    for (auto& entry : entries) {
        if (name_counts[entry.filename] > 1) {
            auto sep = entry.path.find_last_of("/\\");
            if (sep != std::string::npos) {
                auto parent = entry.path.substr(0, sep);
                auto sep2 = parent.find_last_of("/\\");
                std::string dir_name = (sep2 != std::string::npos)
                    ? parent.substr(sep2 + 1) : parent;
                entry.display_label = dir_name + "/" + entry.filename;
            } else {
                entry.display_label = entry.filename;
            }
        } else {
            entry.display_label = entry.filename;
        }
    }
}

void AppController::sort_entries_by_name() {
    auto remap = library_->sort_with(&filename_less);
    selection_->apply_remap(remap);
}

void AppController::move_entry(int from, int to) {
    if (from == to) return;
    const int n = static_cast<int>(library_->all().size());
    if (from < 0 || from >= n) return;
    if (to < 0 || to >= n) return;

    auto remap = library_->move(static_cast<std::size_t>(from),
                                static_cast<std::size_t>(to));
    selection_->apply_remap(remap);
}

void AppController::remove_entry(int index) {
    const int n = static_cast<int>(library_->all().size());
    if (index < 0 || index >= n) return;

    // The library destroys the entry's texture via ITextureUploader
    // and returns a remap so we can fix up selection indices.
    // apply_remap tells us whether membership actually changed; if so,
    // the A/B swap toggle no longer refers to a meaningful pair and
    // must reset.
    auto remap = library_->remove(static_cast<std::size_t>(index));
    if (selection_->apply_remap(remap)) {
        selection_->set_swap_ab(false);
    }

    compute_display_labels();
    diff_->mark_dirty();
}

bool AppController::has_running_sr_tasks() const {
    return sr_tasks_->has_running();
}

void AppController::sync_entries_to_timeline() {
    std::string status_buf;
    if (timeline_->sync_to(library_->all(), status_buf)) {
        diff_->mark_dirty();
    }
    // sync_to() only writes to out_status on a per-frame read failure.
    // Use append so the message decorates whatever prior text the
    // status bar was showing (matching the previous in-place behaviour
    // when state_->status_text was passed directly).
    if (!status_buf.empty()) {
        status_reporter_->append_status(status_buf);
    }
}

void AppController::start_sr_task(const SRTaskParams& params) {
    auto factory = []() -> std::unique_ptr<SRInferEngine> {
        return SRInferEngineFactory::instance().create_engine("seedvr2");
    };
    const auto result = sr_tasks_->start(params, factory);
    if (!result.ok) {
        status_reporter_->show_error(result.error_title,
                                     result.error_message);
    }
}

} // namespace idiff
