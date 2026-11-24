#include "AnycubicLoginDialog.hpp"

#include "GUI.hpp"
#include "I18N.hpp"
#include "Widgets/WebView.hpp"
#include "slic3r/Utils/AnycubicCloud.hpp"

#include <boost/log/trivial.hpp>
#include <nlohmann/json.hpp>
#include <wx/sizer.h>
#include <wx/utils.h>
#include <wx/webview.h>

namespace Slic3r { namespace GUI {

namespace {
// The Anycubic web site's file page, which sends to the login page when there is no session
const char* const login_url = "https://cloud-universe.anycubic.com/file";

// Hands the token the site keeps in its local storage to the dialog, once there is one
const char* const token_script = "(function() {"
                                 "  var token = window.localStorage && window.localStorage.getItem('XX-Token');"
                                 "  if (token && window.wx)"
                                 "    window.wx.postMessage(JSON.stringify({command: 'anycubic_token', token: token}));"
                                 "})();";
} // namespace

AnycubicLoginDialog::AnycubicLoginDialog(wxWindow* parent)
    : wxDialog(parent, wxID_ANY, _L("Log in to Anycubic"), wxDefaultPosition, wxDefaultSize, wxDEFAULT_DIALOG_STYLE | wxRESIZE_BORDER)
    , m_timer(this)
{
    m_browser = WebView::CreateWebView(this, login_url);
    if (m_browser == nullptr) {
        BOOST_LOG_TRIVIAL(error) << "AnycubicLoginDialog: cannot create the web view";
        return;
    }

    auto* sizer = new wxBoxSizer(wxVERTICAL);
    sizer->Add(m_browser, 1, wxEXPAND);
    SetSizer(sizer);
    SetSize(FromDIP(wxSize(900, 760)));
    CenterOnParent();

    Bind(wxEVT_WEBVIEW_SCRIPT_MESSAGE_RECEIVED, &AnycubicLoginDialog::on_script_message, this, m_browser->GetId());
    Bind(wxEVT_TIMER, &AnycubicLoginDialog::on_timer, this, m_timer.GetId());
    m_timer.Start(1000);
}

AnycubicLoginDialog::~AnycubicLoginDialog() { m_timer.Stop(); }

void AnycubicLoginDialog::on_timer(wxTimerEvent& /*event*/)
{
    if (m_browser != nullptr && !m_browser->IsBusy())
        WebView::RunScript(m_browser, token_script);
}

void AnycubicLoginDialog::on_script_message(wxWebViewEvent& event)
{
    const auto message = nlohmann::json::parse(into_u8(event.GetString()), nullptr, false);
    if (!message.is_object() || message.value("command", "") != "anycubic_token" || !message.contains("token") ||
        !message["token"].is_string())
        return;
    const std::string token = message["token"].get<std::string>();
    if (token.empty() || token == m_refused || !m_timer.IsRunning())
        return;

    // Check the token before taking it: the page may still hold one from an ended session until the user logs in
    m_timer.Stop();
    std::string error;
    bool        valid;
    {
        wxBusyCursor          wait;
        AnycubicCloud::Client client(token);
        valid = client.login(error);
        if (!valid && !client.token_refused()) {
            // The cloud could not be reached: try the same token again later
            BOOST_LOG_TRIVIAL(warning) << "AnycubicLoginDialog: cannot check the token: " << error;
            m_timer.Start(5000);
            return;
        }
    }
    if (!valid) {
        m_refused = token;
        m_timer.Start(1000);
        return;
    }

    m_token = token;
    EndModal(wxID_OK);
}

}} // namespace Slic3r::GUI
