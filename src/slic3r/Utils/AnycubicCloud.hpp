#pragma once

#include <array>
#include <cstdint>
#include <functional>
#include <map>
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
// user token itself. Both are JWTs and cannot be told apart, so the exchange is tried first and the web form is the
// fallback.
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

// One entry of the start-print order's ams_box_mapping: the project filament `paint_index` is fed from ACE slot `ams_index`.
struct SlotMapping
{
    int                paint_index = 0;
    int                ams_index   = 0;
    std::string        material_type;
    std::array<int, 3> paint_color{0, 0, 0};
    std::array<int, 3> ams_color{0, 0, 0};
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

// "#RRGGBB", "RRGGBB" or "RRGGBBAA" to RGB; anything else is black.
std::array<int, 3> rgb_from_hex(const std::string& colour);
std::string        hex_from_rgb(const std::array<int, 3>& rgb);

// Turns the print dialog's ams_mapping_info (one entry per project filament, with "ams" set to the chosen tray or -1,
// and "sourceColor"/"targetColor"/"filamentType") into the order's slot mapping. Filaments without a tray are left out.
std::vector<SlotMapping> slot_mapping_from_ams_mapping_info(const std::string& ams_mapping_info);

// The body of /work/operation/sendOrder that prints the uploaded cloud file `file_id` on `printer_id`.
nlohmann::json start_print_order(const std::string& printer_id, int64_t file_id, const std::vector<SlotMapping>& mapping);

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

    bool send_order(const nlohmann::json& order, std::string& error);

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
