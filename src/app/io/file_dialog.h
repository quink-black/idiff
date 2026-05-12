#ifndef IDIFF_APP_IO_FILE_DIALOG_H
#define IDIFF_APP_IO_FILE_DIALOG_H

// File-dialog interface.
//
// Wraps the three NFD entry points the app uses today.  Tests can
// replace the implementation with a fake that returns prescribed
// paths and records what filters were requested.
//
// Lifetime: the IFileDialog instance is constructed after NFD_Init()
// and destroyed before NFD_Quit(); it does not own NFD's process-wide
// state.

#include <string>
#include <string_view>
#include <vector>

namespace idiff {

struct FileDialogFilter {
    // Human-readable label, e.g. "PNG image".
    std::string_view label;
    // Comma-separated extension list with no dots, e.g. "png,jpg".
    std::string_view extensions;
};

struct FileDialogResult {
    // Empty when the user cancelled.  When `error` is non-empty an OS
    // error happened; in that case `paths` is empty and the caller
    // should surface the error to the user.
    std::vector<std::string> paths;
    std::string error;
};

class IFileDialog {
public:
    virtual ~IFileDialog() = default;

    // Modal "Open one or more files" dialog.
    virtual FileDialogResult open_multiple(
        const std::vector<FileDialogFilter>& filters) = 0;

    // Modal "Open a single file" dialog.  On success `paths` has
    // exactly one element.
    virtual FileDialogResult open_single(
        const std::vector<FileDialogFilter>& filters) = 0;

    // Modal "Save file" dialog.  `default_name` may be empty.
    virtual FileDialogResult save(
        const std::vector<FileDialogFilter>& filters,
        std::string_view default_name) = 0;
};

// NFD-backed default implementation.
class NfdFileDialog : public IFileDialog {
public:
    NfdFileDialog() = default;
    FileDialogResult open_multiple(
        const std::vector<FileDialogFilter>& filters) override;
    FileDialogResult open_single(
        const std::vector<FileDialogFilter>& filters) override;
    FileDialogResult save(
        const std::vector<FileDialogFilter>& filters,
        std::string_view default_name) override;
};

} // namespace idiff

#endif // IDIFF_APP_IO_FILE_DIALOG_H
