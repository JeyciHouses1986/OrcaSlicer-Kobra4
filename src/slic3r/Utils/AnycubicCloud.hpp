#pragma once

#include <array>
#include <cstdint>
#include <functional>
#include <istream>
#include <map>
#include <optional>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

namespace Slic3r {

// Client for the Anycubic Cloud "workbench" API, which Anycubic Slicer Next and the Anycubic app use to list the
// printers of an account, read their ACE multi-colour units and start cloud prints.
//
// Anycubic does not document this API. The endpoints, the request signature and the public client constants follow
// the GPL-3.0-or-later anycubic-cloud-api project (https://github.com/Nino6689/anycubic-cloud-api), which in turn
// took them from the published Anycubic web and slicer clients. Nothing here is specific to one user: the account is
// identified only by the token the user pastes into the printer settings, and that token is never stored by this code.
namespace AnycubicCloud {

// Anycubic Slicer Next keeps an access token that must be exchanged for a user token; the Anycubic web site keeps the
// user token itself. Both are JWTs and cannot be told apart, so the token is first checked as a web token and the
// exchange is the fallback.
enum class TokenKind { slicer, web };

struct Printer
{
    std::string id;
    std::string name;
    std::string model;     // e.g. "Anycubic Kobra 4"
    int         device_status = 0; // 1 online, 2 offline
    int         is_printing   = 0; // 1 idle, 2 busy

    bool online() const { return device_status == 1; }
    bool busy() const { return is_printing == 2; }
};

struct AceSlot
{
    int                index = 0;  // 0-based across all ACE units: unit * 4 + slot
    bool               loaded = false;
    std::string        material_type; // "PLA", "PETG", ...
    std::array<int, 3> rgb{0, 0, 0};
};

// An ACE unit's state in /v2/printer/info: its dryer and humidity
struct AceUnit
{
    int id              = 0;
    int temp            = -1; // degrees inside the unit, -1 when not reported
    int humidity        = -1; // percent, -1 when not reported
    int drying          = 0;  // drying_status.status: 1 while drying
    int dry_target_temp = 0;  // degrees
    int dry_duration    = 0;  // minutes
    int dry_remain      = 0;  // minutes
};

// One entry of the start-print order's ams_box_mapping: the project filament `paint_index` is fed from ACE slot `ams_index`.
struct SlotMapping
{
    int                paint_index = 0;
    int                ams_index   = 0;
    std::string        material_type;
    std::array<int, 3> paint_color{0, 0, 0};
    std::array<int, 3> ams_color{0, 0, 0};
};

// A print job as the cloud's project list reports it
struct Job
{
    // Values of print_status
    enum Status { printing = 1, complete = 2, cancelled = 3, downloading = 4, checking = 5, preheating = 6 };

    std::string id; // the cloud's project id, which pause, resume, stop and print settings name
    std::string name;
    int         print_status   = 0;
    bool        paused         = false;
    int         progress       = -1; // percent, -1 when unknown
    int         remain_minutes = -1;
    int         layer          = -1;
    int         total_layers   = -1;
};

// ---- Pure helpers, kept separate from the HTTP client so they can be tested without a network ----

std::string md5_hex(const std::string& input);

// The Xx-* headers every request carries. `user_token` is empty for the token exchange itself.
std::map<std::string, std::string> request_headers(TokenKind kind, const std::string& user_token, int64_t timestamp_ms, const std::string& nonce);

// Every answer is {"code":..,"msg":..,"data":..}; a request succeeded when `data` is present and not null.
bool parse_response(const std::string& body, nlohmann::json& data, std::string& error);

std::vector<Printer> parse_printers(const nlohmann::json& data);

// The loaded and empty slots of every ACE unit in a /v2/printer/info answer.
std::vector<AceSlot> parse_ace_slots(const nlohmann::json& printer_info);

std::vector<AceUnit> parse_ace_units(const nlohmann::json& printer_info);

// "#RRGGBB", "RRGGBB" or "RRGGBBAA" to RGB; anything else is black.
std::array<int, 3> rgb_from_hex(const std::string& colour);
std::string        hex_from_rgb(const std::array<int, 3>& rgb);

// Turns the print dialog's ams_mapping_info (one entry per project filament, with "ams" set to the chosen tray or -1,
// and "sourceColor"/"targetColor"/"filamentType") into the order's slot mapping. Filaments without a tray are left out.
std::vector<SlotMapping> slot_mapping_from_ams_mapping_info(const std::string& ams_mapping_info);

// The body of /work/operation/sendOrder that prints the uploaded cloud file `file_id` on `printer_id`, recording a
// timelapse with the printer's camera when `timelapse` is set.
nlohmann::json start_print_order(const std::string& printer_id, int64_t file_id, const std::vector<SlotMapping>& mapping,
                                 bool timelapse = false);

// Orders on the running job `project_id`: pause (2), resume (3) and stop (4)
enum class JobOrder { pause = 2, resume = 3, stop = 4 };
nlohmann::json job_order(const std::string& printer_id, const std::string& project_id, JobOrder order);

// Starts drying in ACE unit `box_id` at `temp` degrees for `minutes`, or stops it when `minutes` is 0
nlohmann::json ace_drying_order(const std::string& printer_id, int box_id, int temp, int minutes);
// Sets the material and colour of slot `slot` (0-3) of ACE unit `box_id`, as the printer's screen does
nlohmann::json ace_slot_order(const std::string& printer_id, int box_id, int slot, const std::string& material_type,
                              const std::array<int, 3>& rgb);

// Printer-level orders, which work with or without a running job, as Slicer Next sends them.
// Sets the nozzle and/or bed target temperature; a negative value leaves that one alone.
nlohmann::json temperature_order(const std::string& printer_id, int nozzle, int bed);
// Sets one fan, named as the cloud names it: fan_speed_pct (part cooling) or aux_fan_speed_pct
nlohmann::json fan_order(const std::string& printer_id, const std::string& fan, int percent);
// Moves `axis` (1 X, 2 Y, 3 Z, 4 all) by `distance` mm (move_type 0 minus, 1 plus) or homes it (move_type 2)
nlohmann::json axis_order(const std::string& printer_id, int axis, int move_type, int distance);

// Turns light `type` on at full brightness or off. Printers differ in the type of their light (the Kobra S1's is 2).
nlohmann::json light_order(const std::string& printer_id, int type, bool on);

// The printer-level order a G-code line from the device page stands for (M104, M140, M106, M107, G28 or a jog),
// or null when the cloud has none
nlohmann::json order_from_gcode(const std::string& printer_id, const std::string& gcode);

// The body of /work/operation/sendOrder that opens the printer's camera for one viewing session
nlohmann::json camera_open_order(const std::string& printer_id);

// The Agora ("shengwang") credentials of one viewing session in the answer to camera_open_order(), as the camera
// page wants them; null when the answer has none, as when the account's camera goes through another transport.
nlohmann::json camera_session(const nlohmann::json& order_answer);

// The nozzle and bed temperatures in a /v2/printer/info answer, in degrees; -1 when not reported.
void parse_temperatures(const nlohmann::json& printer_info, int& nozzle, int& bed);

// The newest job of `printer_id` in a /work/project/getProjects answer.
std::optional<Job> latest_job(const nlohmann::json& projects, const std::string& printer_id);

// Whether a refused print order only means the cloud has not read the uploaded file yet, which takes some seconds
bool file_not_ready(const std::string& order_error);

// The statistics block Anycubic Slicer Next ends its G-code with, from which the cloud reads the print time and
// model size it shows: built from the stats Orca writes, and empty when the G-code has one already.
std::string statistics_block(std::istream& gcode);

// Picks the printer `wanted` names (its name or id), or the only printer of the account when `wanted` names none.
const Printer* choose_printer(const std::vector<Printer>& printers, const std::string& wanted);

// ---- HTTP client ----

class Client
{
public:
    // `token` as pasted by the user, from Anycubic Slicer Next or the Anycubic web site.
    explicit Client(std::string token);

    // Exchanges or adopts the token. Every other call needs it to have succeeded.
    bool login(std::string& error);
    // Whether the last login failed because the cloud refused the token (expired, or not a token), rather than
    // because the cloud could not be reached. Logging in again on the web site gives a new token.
    bool token_refused() const { return m_token_refused; }

    bool list_printers(std::vector<Printer>& printers, std::string& error);
    bool printer_info(const std::string& printer_id, nlohmann::json& info, std::string& error);

    // Uploads `path` as a temporary cloud file, which the printer deletes after printing, and returns its id.
    bool upload_file(const std::string&                            path,
                     const std::string&                            name,
                     const std::function<void(int percent)>&       progress,
                     const std::function<bool()>&                  cancelled,
                     int64_t&                                      file_id,
                     std::string&                                  error);

    // The account's newest print jobs, newest first
    bool list_projects(nlohmann::json& projects, std::string& error);

    bool send_order(const nlohmann::json& order, std::string& error);
    bool send_order(const nlohmann::json& order, nlohmann::json& answer, std::string& error);

private:
    bool request(const std::string& method, const std::string& endpoint, const nlohmann::json& body, nlohmann::json& data, std::string& error);

    std::string m_pasted_token;
    std::string m_user_token;
    TokenKind   m_kind          = TokenKind::slicer;
    bool        m_answered      = false; // The last request got an answer from the cloud
    bool        m_token_refused = false;
};

} // namespace AnycubicCloud
} // namespace Slic3r
