#include "app/controller.h"

#include "domain/comparison_config_service.h"
#include "domain/diff_service.h"
#include "domain/image_library.h"
#include "domain/selection_model.h"
#include "domain/sr_task_service.h"
#include "domain/timeline_model.h"

namespace idiff {

AppController::AppController(ITextureUploader& texture_uploader)
    : library_(std::make_unique<ImageLibrary>(texture_uploader)),
      selection_(std::make_unique<SelectionModel>()),
      timeline_(std::make_unique<TimelineModel>()),
      diff_(std::make_unique<DiffService>(texture_uploader)),
      sr_tasks_(std::make_unique<SrTaskService>()),
      comparison_config_(std::make_unique<ComparisonConfigService>()) {}

AppController::~AppController() = default;

ImageLibrary& AppController::library() noexcept { return *library_; }
SelectionModel& AppController::selection() noexcept { return *selection_; }
TimelineModel& AppController::timeline() noexcept { return *timeline_; }
DiffService& AppController::diff() noexcept { return *diff_; }
SrTaskService& AppController::sr_tasks() noexcept { return *sr_tasks_; }
ComparisonConfigService& AppController::comparison_config() noexcept {
    return *comparison_config_;
}

} // namespace idiff
