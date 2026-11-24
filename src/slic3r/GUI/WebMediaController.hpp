#pragma once

#include <slic3r/GUI/IMediaController.hpp>
#include <slic3r/Utils/IPrinterAgent.hpp>

#include <string>
#include <wx/uri.h>

class wxWebView;

namespace Slic3r { namespace GUI {

class WebMediaController : public IMediaController
{
public:
    explicit WebMediaController(wxWebView* webview);

    void Load(wxURI url) override;

    void set_mode(CameraStreamMode mode);

    void Play() override;

    void Stop() override;

private:
    // A web_page camera asks for its session through window.wx.postMessage
    void on_script_message(const wxString& message);

    wxWebView* m_webview = nullptr;
    std::string m_url;
    CameraStreamMode m_stream_mode = CameraStreamMode::http;
};

}} // namespace Slic3r::GUI
