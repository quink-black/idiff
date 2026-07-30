#ifndef IDIFF_APP_UI_DIALOGS_H
#define IDIFF_APP_UI_DIALOGS_H

#include <string>
#include <vector>

namespace idiff {

// State and behavior of the modal error popup shown via
// IStatusReporter::show_error.  The struct is owned by the UI shell;
// the dialog renderer below does not allocate or persist anything
// beyond what these fields express.  Kept POD so it can be aggregate-
// initialized inside App::State.
struct ErrorDialogState {
    bool visible = false;
    bool needs_open = false;
    std::string title;
    std::string message;
};

// Render the error modal.  No-op when state.visible is false.  Reads
// and writes state.visible / state.needs_open through the user's
// interactions; never allocates.
void render_error_dialog(ErrorDialogState& state);

// State and outputs of the "file changed on disk" reload prompt.
// The UI shell accumulates changed paths from the file watcher and
// opens this dialog; the user either reloads or dismisses.
struct ReloadDialogState {
    bool visible = false;
    bool needs_open = false;
    // Paths that changed since last acknowledged.
    std::vector<std::string> changed_paths;
    // Set to true when the user clicks "Reload"; the caller acts on
    // it next frame and clears it.
    bool reload_requested = false;
};

// Render the file-changed-on-disk modal.  No-op when state.visible
// is false.
void render_reload_dialog(ReloadDialogState& state);

} // namespace idiff

#endif // IDIFF_APP_UI_DIALOGS_H
