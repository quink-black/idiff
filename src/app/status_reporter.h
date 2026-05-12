#ifndef IDIFF_APP_STATUS_REPORTER_H
#define IDIFF_APP_STATUS_REPORTER_H

// Status reporter interface.
//
// The non-UI domain services (and the orchestration code in
// AppController) need a way to surface short user-visible messages
// (status bar text, SR badges, error dialogs) without depending on
// App's State struct or any ImGui type.  IStatusReporter is the
// narrow seam: any side that needs to talk back to the user takes a
// reference, and the concrete UI implementation (App owns one)
// writes into the appropriate State fields.
//
// Tests that exercise AppController directly inject a fake
// implementation that simply records the calls.  This keeps the
// controller free of UI dependencies and leaves the production
// formatting / styling decisions in App.

#include <string>

namespace idiff {

class IStatusReporter {
public:
    virtual ~IStatusReporter() = default;

    // Replace the status-bar text with `text`.  Empty strings clear it.
    virtual void set_status(const std::string& text) = 0;

    // Append to the current status-bar text.  Used when one operation
    // wants to add a follow-up note (e.g. "ignored N extra files")
    // without overwriting the message produced by the operation it
    // chained into.
    virtual void append_status(const std::string& text) = 0;

    // Replace the SR/notification status (the secondary line shown for
    // long-running super-resolution jobs).
    virtual void set_sr_status(const std::string& text) = 0;

    // Surface a modal error notification with the given title and
    // body.  Called for unrecoverable failures the user must
    // acknowledge before anything else happens.
    virtual void show_error(const std::string& title,
                            const std::string& message) = 0;
};

} // namespace idiff

#endif // IDIFF_APP_STATUS_REPORTER_H
