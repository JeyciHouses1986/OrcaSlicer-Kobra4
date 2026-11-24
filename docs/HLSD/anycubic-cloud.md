# Anycubic Cloud printer agent

The `anycubic` printer agent prints on Anycubic printers through the Anycubic Cloud, the service Anycubic Slicer
Next and the Anycubic app use. It exists for printers such as the Kobra 4, whose firmware offers no open LAN
interface while it is bound to an Anycubic account, and whose ACE multi-colour units are told which slot feeds
each filament only through the cloud's print order. The Kobra 4 system presets select this agent.

## How it is set up

The account is identified by a token kept in the physical printer's API key field. Logging in to Anycubic needs a
captcha or a second factor, so the agent never logs in with a password. Instead, the physical printer dialog offers
"Log in to Anycubic" when the selected preset uses this agent: `AnycubicLoginDialog` opens Anycubic's own login
page in an embedded browser, reads the token the site keeps in its local storage once the user is in, checks it
with the cloud and fills in the API key. The token ends when the user logs out on the web site or when it expires;
the agent then reports the refusal once instead of retrying, and logging in again gives a new token.

A token can also be pasted by hand. Two kinds work:

- the user token the Anycubic web site keeps in its local storage (`XX-Token`), which the agent uses as it is;
- the access token Anycubic Slicer Next keeps in its configuration, which the agent exchanges for a user token.

They cannot be told apart, so the agent first checks the token as the web site does and falls back to the
exchange, so that a web token is never sent to it. The host field names
the printer (its name in the Anycubic app, or its cloud id) when the account has several; with one printer any
placeholder works.

## Design

`AnycubicPrinterAgent` derives from `MoonrakerPrinterAgent` for what does not depend on the transport: the device
record and its discovery announcement, the AMS payload the filament sync and the print dialog read, and the print
flow that extracts the plate's G-code and reports progress. It replaces the transport:

- connecting resolves the printer through the cloud, then polls the cloud every 15 seconds, since the cloud's MQTT
  stream needs certificates and an encrypted login that this agent does not implement. Each poll reads the printer
  list (busy or idle), the printer's record (nozzle and bed temperatures) and the account's newest jobs, whose
  progress, remaining time and layers the device page shows; an idle printer shows how its last job ended;
- filament sync reads the ACE slots from the printer's cloud record (`multi_color_box`), where a slot whose
  `edit_status` is 2 is empty even though it still reports the last spool it held;
- an upload reserves cloud storage, puts the file at the pre-signed address it gets back and registers it as a
  temporary file, which the printer deletes once printed. The uploaded copy ends with the statistics block of Slicer
  Next's G-code, built from Orca's stats, since the cloud reads the print time and model size from it;
- the cloud takes some seconds to read an uploaded file, and refuses a print order for it meanwhile ("No slicer
  found"), so the order is retried until the cloud has read it;
- the print starts with a cloud order whose `ams_box_mapping` pairs each project filament with the ACE slot chosen
  in the print dialog, and which asks the printer to record a timelapse when the dialog's option is set;
- the camera streams through Agora's WebRTC service, which only Agora's web SDK speaks, so the agent's camera mode is
  `web_page`: the device page loads `resources/web/anycubic/camera.html` in its web view, the page asks for a session
  (`request_camera_session`), and the agent sends the cloud's camera order, whose answer carries the channel, token and
  encryption settings of one viewing. Each viewing needs fresh ones, which is why the page asks rather than the
  address carrying them. The page loads the MIT-licensed SDK from a CDN, and gets the few words it shows from the
  agent, already translated.

Device page commands become cloud orders on `/work/operation/sendOrder`: pause (2), resume (3) and stop (4) name the running job's project id; bed and nozzle temperatures and the fans go through the print settings order (6), so they only apply while a job runs. The ACE's dryer (1207) works at any time: each ACE unit is reported to the device page as a filament unit with a dryer and a humidity sensor, which makes Orca offer its drying controls, and the agent turns their command into the drying order (Orca counts hours, the cloud minutes). Commands the cloud has no order for, such as moves and homing, are refused as unsupported rather than ignored.

`AnycubicCloud` holds the HTTP client and the pure helpers (request signature, answer parsing, slot mapping, order
body), so these are tested without a network. The API is undocumented; the endpoints, the request signature and
the public client constants follow the GPL-3.0-or-later
[anycubic-cloud-api](https://github.com/Nino6689/anycubic-cloud-api) project. The firmware also needs the ACE
header lines of the G-code, which `GCode/AnycubicHeader` writes.

## Main implementation locations

- [`AnycubicPrinterAgent`](../../src/slic3r/Utils/AnycubicPrinterAgent.hpp) — the agent
- [`AnycubicCloud`](../../src/slic3r/Utils/AnycubicCloud.hpp) — cloud client and helpers
- [`AnycubicLoginDialog`](../../src/slic3r/GUI/AnycubicLoginDialog.hpp) — login in an embedded browser
- [`camera.html`](../../resources/web/anycubic/camera.html) — the camera player
- [`AnycubicHeader`](../../src/libslic3r/GCode/AnycubicHeader.hpp) — ACE header lines in the G-code
