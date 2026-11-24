#include "AnycubicCloud.hpp"

#include "Http.hpp"

#include <boost/filesystem/operations.hpp>
#include <boost/filesystem/path.hpp>
#include <boost/log/trivial.hpp>
#include <boost/uuid/uuid.hpp>
#include <boost/uuid/uuid_generators.hpp>
#include <boost/uuid/uuid_io.hpp>
#include <openssl/evp.h>

#include <algorithm>
#include <cctype>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <utility>

namespace Slic3r {
namespace AnycubicCloud {
namespace {

const char* const api_root    = "https://cloud-universe.anycubic.com/p/p/workbench/api";
const char* const auth_origin = "https://uc.makeronline.com";

// Public constants of the Anycubic clients, as published by anycubic-cloud-api (const/const.py).
const char* const app_id             = "f9b3528877c94d5c9c5af32245db46ef";
const char* const app_secret         = "0cf75926606049a3937f56b0373b99fb";
const char* const app_version_slicer = "V3.0.0";
const char* const app_version_web    = "1.0.0";

// A web token is sent as a browser would send it, since the server checks that its client looks like the web site.
const char* const web_user_agent = "Mozilla/5.0 (Macintosh; Intel Mac OS X 10_15_7) AppleWebKit/537.36 (KHTML, like Gecko) "
                                   "Chrome/125.0.0.0 Safari/537.36";

// `edit_status` of an ACE slot that holds no spool; the slot keeps reporting the last material it saw.
constexpr int slot_edit_status_empty = 2;

int64_t now_ms()
{
    return std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::system_clock::now().time_since_epoch()).count();
}

std::string new_nonce() { return boost::uuids::to_string(boost::uuids::random_generator()()); }

std::string json_string(const nlohmann::json& obj, const char* key)
{
    auto it = obj.find(key);
    if (it == obj.end() || it->is_null())
        return {};
    if (it->is_string())
        return it->get<std::string>();
    return it->dump();
}

int json_int(const nlohmann::json& obj, const char* key, int fallback = 0)
{
    auto it = obj.find(key);
    if (it == obj.end())
        return fallback;
    if (it->is_number_integer())
        return it->get<int>();
    if (it->is_string()) {
        const std::string text = it->get<std::string>();
        char*             end  = nullptr;
        const long        v    = std::strtol(text.c_str(), &end, 10);
        if (end != text.c_str())
            return int(v);
    }
    return fallback;
}

// The order needs numeric ids, while ids are kept as text everywhere else.
nlohmann::json id_value(const std::string& id)
{
    if (!id.empty() && std::all_of(id.begin(), id.end(), [](unsigned char c) { return std::isdigit(c); }))
        return std::stoll(id);
    return id;
}

nlohmann::json rgb_json(const std::array<int, 3>& rgb) { return nlohmann::json::array({rgb[0], rgb[1], rgb[2]}); }

} // namespace

std::string md5_hex(const std::string& input)
{
    unsigned char digest[EVP_MAX_MD_SIZE];
    unsigned int  length = 0;
    EVP_MD_CTX*   ctx    = EVP_MD_CTX_new();
    EVP_DigestInit_ex(ctx, EVP_md5(), nullptr);
    EVP_DigestUpdate(ctx, input.data(), input.size());
    EVP_DigestFinal_ex(ctx, digest, &length);
    EVP_MD_CTX_free(ctx);

    std::string hex;
    char        byte[3];
    for (unsigned int i = 0; i < length; ++i) {
        std::snprintf(byte, sizeof(byte), "%02x", digest[i]);
        hex += byte;
    }
    return hex;
}

std::map<std::string, std::string> request_headers(TokenKind kind, const std::string& user_token, int64_t timestamp_ms, const std::string& nonce)
{
    const std::string version   = kind == TokenKind::web ? app_version_web : app_version_slicer;
    const std::string timestamp = std::to_string(timestamp_ms);

    std::map<std::string, std::string> headers{
        {"Xx-Device-Type", kind == TokenKind::web ? "web" : "pcf"},
        {"Xx-Is-Cn", "1"},
        {"Xx-Nonce", nonce},
        {"Xx-Signature", md5_hex(app_id + timestamp + version + app_secret + nonce + app_id)},
        {"Xx-Timestamp", timestamp},
        {"Xx-Version", version},
        {"Content-Type", "application/json"},
        {"XX-LANGUAGE", "US"},
    };
    if (!user_token.empty())
        headers["XX-Token"] = user_token;
    if (kind == TokenKind::web) {
        headers["User-Agent"] = web_user_agent;
        headers["Origin"]     = auth_origin;
    }
    return headers;
}

bool parse_response(const std::string& body, nlohmann::json& data, std::string& error)
{
    const auto json = nlohmann::json::parse(body, nullptr, false);
    if (json.is_discarded() || !json.is_object()) {
        error = "Invalid answer from Anycubic Cloud";
        return false;
    }
    auto it = json.find("data");
    if (it == json.end() || it->is_null()) {
        error = json_string(json, "msg");
        if (error.empty())
            error = "Anycubic Cloud refused the request";
        return false;
    }
    data = *it;
    return true;
}

std::vector<Printer> parse_printers(const nlohmann::json& data)
{
    std::vector<Printer> printers;
    if (!data.is_array())
        return printers;
    for (const auto& item : data) {
        if (!item.is_object())
            continue;
        Printer printer;
        printer.id            = json_string(item, "id");
        printer.name          = json_string(item, "name");
        printer.model         = json_string(item, "model");
        printer.device_status = json_int(item, "device_status");
        printer.is_printing   = json_int(item, "is_printing");
        if (!printer.id.empty())
            printers.push_back(std::move(printer));
    }
    return printers;
}

std::vector<AceSlot> parse_ace_slots(const nlohmann::json& printer_info)
{
    std::vector<AceSlot> slots;
    if (!printer_info.is_object())
        return slots;
    auto boxes_it = printer_info.find("multi_color_box");
    if (boxes_it == printer_info.end() || boxes_it->is_null())
        return slots;
    // One unit comes as an object, several as an array
    const nlohmann::json boxes = boxes_it->is_array() ? *boxes_it : nlohmann::json::array({*boxes_it});

    for (size_t unit = 0; unit < boxes.size(); ++unit) {
        const auto& box = boxes[unit];
        if (!box.is_object() || !box.contains("slots") || !box["slots"].is_array())
            continue;
        for (const auto& item : box["slots"]) {
            if (!item.is_object())
                continue;
            AceSlot slot;
            slot.index         = int(unit) * 4 + json_int(item, "index");
            slot.material_type = json_string(item, "type");
            slot.loaded        = json_int(item, "edit_status", slot_edit_status_empty) != slot_edit_status_empty;
            auto colour        = item.find("color");
            if (colour != item.end() && colour->is_array() && colour->size() >= 3)
                for (size_t i = 0; i < 3; ++i)
                    slot.rgb[i] = (*colour)[i].is_number() ? std::clamp((*colour)[i].get<int>(), 0, 255) : 0;
            slots.push_back(std::move(slot));
        }
    }
    return slots;
}

std::array<int, 3> rgb_from_hex(const std::string& colour)
{
    std::array<int, 3> rgb{0, 0, 0};
    const std::string  hex = !colour.empty() && colour[0] == '#' ? colour.substr(1) : colour;
    if (hex.size() < 6 || !std::all_of(hex.begin(), hex.begin() + 6, [](unsigned char c) { return std::isxdigit(c); }))
        return rgb;
    for (size_t i = 0; i < 3; ++i)
        rgb[i] = int(std::strtol(hex.substr(2 * i, 2).c_str(), nullptr, 16));
    return rgb;
}

std::string hex_from_rgb(const std::array<int, 3>& rgb)
{
    char text[8];
    std::snprintf(text, sizeof(text), "#%02X%02X%02X", rgb[0] & 0xFF, rgb[1] & 0xFF, rgb[2] & 0xFF);
    return text;
}

std::vector<SlotMapping> slot_mapping_from_ams_mapping_info(const std::string& ams_mapping_info)
{
    std::vector<SlotMapping> mapping;
    const auto               json = nlohmann::json::parse(ams_mapping_info, nullptr, false);
    if (!json.is_array())
        return mapping;
    for (size_t filament = 0; filament < json.size(); ++filament) {
        const auto& item = json[filament];
        if (!item.is_object())
            continue;
        const int tray = json_int(item, "ams", -1);
        if (tray < 0)
            continue;
        SlotMapping entry;
        entry.paint_index   = int(filament);
        entry.ams_index     = tray;
        entry.material_type = json_string(item, "filamentType");
        entry.paint_color   = rgb_from_hex(json_string(item, "sourceColor"));
        entry.ams_color     = rgb_from_hex(json_string(item, "targetColor"));
        mapping.push_back(std::move(entry));
    }
    return mapping;
}

nlohmann::json start_print_order(const std::string& printer_id, int64_t file_id, const std::vector<SlotMapping>& mapping)
{
    nlohmann::json data = {
        {"filetype", 0},
        {"file_key", ""},
        {"file_name", ""},
        {"task_settings", {{"ai_detect", 0}, {"camera_timelapse", 0}}},
        {"file_id", file_id},
        {"hollow_param", nullptr},
        // The cloud file is a temporary one, which the printer removes after printing it
        {"is_delete_file", 1},
        {"matrix", ""},
        {"project_type", 1},
        {"punching_param", nullptr},
        {"slice_param", nullptr},
        {"slice_size", nullptr},
        {"template_id", 0},
    };

    nlohmann::json ams_info = nullptr;
    if (!mapping.empty()) {
        nlohmann::json boxes = nlohmann::json::array();
        for (const SlotMapping& entry : mapping)
            boxes.push_back({
                {"ams_color", rgb_json(entry.ams_color)},
                {"ams_index", entry.ams_index},
                {"filament_used", 0.0},
                {"material_type", entry.material_type},
                {"paint_color", rgb_json(entry.paint_color)},
                {"paint_index", entry.paint_index},
            });
        ams_info = {{"ams_box_mapping", boxes}, {"use_ams", true}};
    }

    return {
        {"order_id", 1}, // start print
        {"printer_id", id_value(printer_id)},
        {"project_id", 0},
        {"data", data},
        {"ams_info", ams_info},
        {"settings", nullptr},
    };
}

const Printer* choose_printer(const std::vector<Printer>& printers, const std::string& wanted)
{
    for (const Printer& printer : printers)
        if (!wanted.empty() && (printer.name == wanted || printer.id == wanted))
            return &printer;
    return printers.size() == 1 ? &printers.front() : nullptr;
}

Client::Client(std::string token)
{
    // A token copied from the browser may come with quotes, spaces or line breaks around or inside it
    for (const char c : token)
        if (!std::isspace(static_cast<unsigned char>(c)) && c != '"')
            m_pasted_token.push_back(c);
}

bool Client::request(const std::string& method, const std::string& endpoint, const nlohmann::json& body, nlohmann::json& data, std::string& error)
{
    const std::string url  = std::string(api_root) + endpoint;
    Http              http = method == "POST" ? Http::post(url) : Http::get(url);
    for (const auto& [name, value] : request_headers(m_kind, m_user_token, now_ms(), new_nonce()))
        http.header(name, value);
    if (method == "POST")
        http.set_post_body(body.is_null() ? std::string("{}") : body.dump());

    bool ok    = false;
    m_answered = false;
    http.timeout_connect(10)
        .timeout_max(30)
        .on_complete([&](std::string answer, unsigned /*status*/) {
            m_answered = true;
            ok         = parse_response(answer, data, error);
        })
        .on_error([&](std::string answer, std::string http_error, unsigned status) {
            m_answered = status != 0;
            // A refusal usually still carries the server's message
            const auto json = nlohmann::json::parse(answer, nullptr, false);
            if (json.is_object() && json.contains("msg") && json["msg"].is_string())
                error = json["msg"].get<std::string>();
            else
                error = http_error.empty() ? "HTTP " + std::to_string(status) : http_error;
            BOOST_LOG_TRIVIAL(warning) << "AnycubicCloud: " << endpoint << " failed: " << error;
        })
        .perform_sync();
    return ok;
}

bool Client::login(std::string& error)
{
    if (m_pasted_token.empty()) {
        error = "No Anycubic Cloud token set";
        return false;
    }
    if (!m_user_token.empty())
        return true;
    m_token_refused = false;

    // A user token taken from the web site is used as it is, and checked the way the web site checks it. It is
    // tried first so that a web token is never sent to the exchange, which only takes Slicer Next access tokens.
    m_kind       = TokenKind::web;
    m_user_token = m_pasted_token;
    nlohmann::json data;
    std::string    web_error;
    if (request("GET", "/user/profile/userInfo", nullptr, data, web_error))
        return true;
    m_user_token.clear();
    const bool web_refused = m_answered;

    // Otherwise it is a Slicer Next access token, exchanged for a user token
    m_kind = TokenKind::slicer;
    std::string slicer_error;
    if (request("POST", "/v3/public/loginWithAccessToken", {{"device_type", "pcf"}, {"access_token", m_pasted_token}}, data,
                slicer_error) &&
        data.is_object() && data.contains("token") && data["token"].is_string()) {
        m_user_token = data["token"].get<std::string>();
        return true;
    }

    BOOST_LOG_TRIVIAL(warning) << "AnycubicCloud: the token is refused as a web token (" << web_error
                               << ") and as a Slicer Next token (" << slicer_error << ")";
    error           = web_error;
    m_token_refused = web_refused && m_answered;
    return false;
}

bool Client::list_printers(std::vector<Printer>& printers, std::string& error)
{
    nlohmann::json data;
    if (!login(error))
        return false;
    if (!request("GET", "/work/printer/getPrinters", nullptr, data, error)) {
        // A refusal may mean the session ended, so the next call checks the token again
        if (m_answered)
            m_user_token.clear();
        return false;
    }
    printers = parse_printers(data);
    return true;
}

bool Client::printer_info(const std::string& printer_id, nlohmann::json& info, std::string& error)
{
    return login(error) && request("GET", "/v2/printer/info?id=" + Http::url_encode(printer_id), nullptr, info, error);
}

bool Client::upload_file(const std::string&                      path,
                         const std::string&                      name,
                         const std::function<void(int percent)>& progress,
                         const std::function<bool()>&            cancelled,
                         int64_t&                                file_id,
                         std::string&                            error)
{
    if (!login(error))
        return false;

    boost::system::error_code ec;
    const auto                size = boost::filesystem::file_size(path, ec);
    if (ec || size == 0) {
        error = "G-code file is missing or empty";
        return false;
    }

    // 1. Reserve the space, which answers with a pre-signed storage URL
    nlohmann::json lock;
    if (!request("POST", "/v2/cloud_storage/lockStorageSpace", {{"size", size}, {"name", name}, {"is_temp_file", 1}}, lock, error))
        return false;
    const std::string lock_id    = json_string(lock, "id");
    const std::string upload_url = json_string(lock, "preSignUrl");
    if (lock_id.empty() || upload_url.empty()) {
        error = "Anycubic Cloud gave no upload address";
        return false;
    }

    // 2. Put the file there
    bool uploaded = false;
    Http::put(upload_url)
        .set_put_body(boost::filesystem::path(path))
        .timeout_connect(10)
        .on_progress([&](Http::Progress p, bool& cancel) {
            if (cancelled && cancelled())
                cancel = true;
            if (progress && p.ultotal > 0)
                progress(int(p.ulnow * 100 / p.ultotal));
        })
        .on_complete([&](std::string, unsigned) { uploaded = true; })
        .on_error([&](std::string, std::string http_error, unsigned status) {
            error = http_error.empty() ? "Upload failed, HTTP " + std::to_string(status) : http_error;
        })
        .perform_sync();

    // 3. Register the upload as a cloud file
    nlohmann::json file;
    const bool registered = uploaded && request("POST", "/v2/profile/newUploadFile", {{"user_lock_space_id", id_value(lock_id)}}, file, error) &&
                            file.is_object() && file.contains("id") && file["id"].is_number_integer();
    if (registered)
        file_id = file["id"].get<int64_t>();

    // 4. Release the reservation, dropping the stored data if anything failed
    nlohmann::json  unlocked;
    std::string     unlock_error;
    request("POST", "/v2/cloud_storage/unlockStorageSpace", {{"id", id_value(lock_id)}, {"is_delete_cos", registered ? 0 : 1}}, unlocked, unlock_error);

    if (!registered && error.empty())
        error = "Anycubic Cloud did not accept the upload";
    return registered;
}

bool Client::send_order(const nlohmann::json& order, std::string& error)
{
    nlohmann::json data;
    return login(error) && request("POST", "/work/operation/sendOrder", order, data, error);
}

} // namespace AnycubicCloud
} // namespace Slic3r
