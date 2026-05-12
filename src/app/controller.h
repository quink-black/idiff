#ifndef IDIFF_APP_CONTROLLER_H
#define IDIFF_APP_CONTROLLER_H

#include <memory>

namespace idiff {

class ITextureUploader;
class ImageLibrary;
class SelectionModel;
class TimelineModel;
class DiffService;
class SrTaskService;
class ComparisonConfigService;

// Owns every non-UI service that drives the application.  Constructed
// once per process inside App::init() after the platform IO seam (the
// texture uploader) is wired, and destroyed before that seam goes
// away.  The controller is the seam between the GUI shell (App) and
// the headless domain layer: tests instantiate the controller alone
// to exercise behaviour without bringing up SDL or ImGui.
//
// At this stage the controller is a structural shim.  Business
// orchestration methods (load_paths, switch_to_comparison_group, ...)
// still live on App and reach into the services through the
// accessors below.  A follow-up commit will move that orchestration
// here, leaving App with only frame composition and event dispatch.
class AppController {
public:
    // The texture uploader is borrowed for the controller's lifetime.
    // Caller (App) must keep it alive until ~AppController returns.
    explicit AppController(ITextureUploader& texture_uploader);
    ~AppController();

    AppController(const AppController&) = delete;
    AppController& operator=(const AppController&) = delete;

    ImageLibrary& library() noexcept;
    SelectionModel& selection() noexcept;
    TimelineModel& timeline() noexcept;
    DiffService& diff() noexcept;
    SrTaskService& sr_tasks() noexcept;
    ComparisonConfigService& comparison_config() noexcept;

private:
    std::unique_ptr<ImageLibrary> library_;
    std::unique_ptr<SelectionModel> selection_;
    std::unique_ptr<TimelineModel> timeline_;
    std::unique_ptr<DiffService> diff_;
    std::unique_ptr<SrTaskService> sr_tasks_;
    std::unique_ptr<ComparisonConfigService> comparison_config_;
};

} // namespace idiff

#endif // IDIFF_APP_CONTROLLER_H
