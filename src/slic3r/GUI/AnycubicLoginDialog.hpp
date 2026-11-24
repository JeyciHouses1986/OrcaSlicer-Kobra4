#pragma once

#include <string>

#include <wx/dialog.h>
#include <wx/timer.h>

class wxWebView;
class wxWebViewEvent;
class wxWindow;

namespace Slic3r { namespace GUI {

// Logs in to the Anycubic web site in an embedded browser and takes the session token it keeps, for the Anycubic
// Cloud printer agent. Anycubic's login needs a captcha or a second factor, so the user logs in on Anycubic's own
// page; the dialog only reads the token the page stores once logged in, and checks it with the cloud.
class AnycubicLoginDialog : public wxDialog
{
public:
    explicit AnycubicLoginDialog(wxWindow* parent);
    ~AnycubicLoginDialog() override;

    // The checked token, once the dialog ends with wxID_OK
    const std::string& token() const { return m_token; }

private:
    void on_timer(wxTimerEvent& event);
    void on_script_message(wxWebViewEvent& event);

    wxWebView*  m_browser = nullptr;
    wxTimer     m_timer;
    std::string m_token;
    std::string m_refused; // A stale token the page still holds, not checked again
};

}} // namespace Slic3r::GUI
