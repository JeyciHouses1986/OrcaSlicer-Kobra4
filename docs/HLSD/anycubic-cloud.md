# Anycubic Cloud printer agent

The `anycubic` printer agent prints on Anycubic printers through the Anycubic Cloud, the service Anycubic Slicer
Next and the Anycubic app use. It exists for printers such as the Kobra 4, whose firmware offers no open LAN
interface while it is bound to an Anycubic account, and whose ACE multi-colour units are told which slot feeds
each filament only through the cloud's print order. The Kobra 4 system presets select this agent.

## How it is set up

The account is identified by a token the user pastes into the printer settings' API key field. Logging in to
Anycubic needs a captcha or a second factor, so the agent does not log in itself. Two tokens work:

- the access token Anycubic Slicer Next keeps in its configuration, which the agent exchanges for a user token;
- the user token the Anycubic web site keeps in its local storage, which the agent uses as it is.

They cannot be told apart, so the exchange is tried first and the web form is the fallback. The host field names
the printer (its name in the Anycubic app, or its cloud id) when the account has several; with one printer any
placeholder works.

## Design

`AnycubicPrinterAgent` derives from `MoonrakerPrinterAgent` for what does not depend on the transport: the device
record and its discovery announcement, the AMS payload the filament sync and the print dialog read, and the print
flow that extracts the plate's G-code and reports progress. It replaces the transport:

- connecting resolves the printer through the cloud, then polls the cloud every 15 seconds for its state, since
  the cloud's MQTT stream needs certificates and an encrypted login that this agent does not implement;
- filament sync reads the ACE slots from the printer's cloud record (`multi_color_box`), where a slot whose
  `edit_status` is 2 is empty even though it still reports the last spool it held;
- an upload reserves cloud storage, puts the file at the pre-signed address it gets back and registers it as a
  temporary file, which the printer deletes once printed;
- the print starts with a cloud order whose `ams_box_mapping` pairs each project filament with the ACE slot chosen
  in the print dialog.

Printer control from the device page (pause, temperatures, moves) is refused as unsupported rather than ignored.

`AnycubicCloud` holds the HTTP client and the pure helpers (request signature, answer parsing, slot mapping, order
body), so these are tested without a network. The API is undocumented; the endpoints, the request signature and
the public client constants follow the GPL-3.0-or-later
[anycubic-cloud-api](https://github.com/Nino6689/anycubic-cloud-api) project. The firmware also needs the ACE
header lines of the G-code, which `GCode/AnycubicHeader` writes.

## Main implementation locations

- [`AnycubicPrinterAgent`](../../src/slic3r/Utils/AnycubicPrinterAgent.hpp) — the agent
- [`AnycubicCloud`](../../src/slic3r/Utils/AnycubicCloud.hpp) — cloud client and helpers
- [`AnycubicHeader`](../../src/libslic3r/GCode/AnycubicHeader.hpp) — ACE header lines in the G-code
