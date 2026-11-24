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
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <istream>
#include <limits>
#include <sstream>
#include <string>
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
    if (it->is_number())
        return int(std::lround(it->get<double>()));
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

std::vector<AceUnit> parse_ace_units(const nlohmann::json& printer_info)
{
    std::vector<AceUnit> units;
    if (!printer_info.is_object())
        return units;
    auto boxes_it = printer_info.find("multi_color_box");
    if (boxes_it == printer_info.end() || boxes_it->is_null())
        return units;
    const nlohmann::json boxes = boxes_it->is_array() ? *boxes_it : nlohmann::json::array({*boxes_it});
    for (size_t i = 0; i < boxes.size(); ++i) {
        const auto& box = boxes[i];
        if (!box.is_object())
            continue;
        AceUnit unit;
        unit.id       = json_int(box, "id", int(i));
        unit.temp     = json_int(box, "temp", -1);
        unit.humidity = json_int(box, "humidity", -1);
        auto drying   = box.find("drying_status");
        if (drying != box.end() && drying->is_object()) {
            unit.drying          = json_int(*drying, "status");
            unit.dry_target_temp = json_int(*drying, "target_temp");
            unit.dry_duration    = json_int(*drying, "duration");
            unit.dry_remain      = json_int(*drying, "remain_time");
        }
        units.push_back(unit);
    }
    return units;
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

nlohmann::json start_print_order(const std::string& printer_id, int64_t file_id, const std::vector<SlotMapping>& mapping, bool timelapse)
{
    nlohmann::json data = {
        {"filetype", 0},
        {"file_key", ""},
        {"file_name", ""},
        {"task_settings", {{"ai_detect", 0}, {"camera_timelapse", timelapse ? 1 : 0}}},
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

bool file_not_ready(const std::string& order_error)
{
    // The cloud's own wording: there is no code that tells these apart from other refusals
    return order_error == "No slicer found" || order_error == "No file found";
}

std::string statistics_block(std::istream& gcode)
{
    const auto value_after = [](const std::string& line, const char* prefix, std::string& value) {
        const size_t n = std::strlen(prefix);
        if (line.compare(0, n, prefix) != 0)
            return false;
        value = line.substr(n);
        while (!value.empty() && (value.back() == '\r' || value.back() == ' '))
            value.pop_back();
        return true;
    };
    // The axis value of a G0/G1 word such as X12.5, or NaN
    const auto axis = [](const std::string& line, char letter) {
        for (size_t i = 3; i < line.size(); ++i) {
            if (line[i] == ';')
                break;
            if (line[i] == letter && line[i - 1] == ' ')
                return std::strtod(line.c_str() + i + 1, nullptr);
        }
        return std::numeric_limits<double>::quiet_NaN();
    };

    std::string print_time, layers, filament, max_z, value;
    double      min_x = 1e9, max_x = -1e9, min_y = 1e9, max_y = -1e9;
    double      x = 0, y = 0;
    bool        absolute_e = false;
    std::string line;
    while (std::getline(gcode, line)) {
        if (line.empty())
            continue;
        if (line[0] == ';') {
            if (line.compare(0, 20, "; statistics = begin") == 0)
                return {};
            if (value_after(line, "; estimated printing time (normal mode) = ", value))
                print_time = value;
            else if (value_after(line, "; total layers count = ", value))
                layers = value;
            else if (value_after(line, "; total filament used [g] = ", value))
                filament = value;
            else if (value_after(line, "; max_z_height: ", value))
                max_z = value;
            continue;
        }
        if (line.compare(0, 3, "M82") == 0)
            absolute_e = true;
        else if (line.compare(0, 3, "M83") == 0)
            absolute_e = false;
        if (line.compare(0, 3, "G1 ") != 0 && line.compare(0, 3, "G0 ") != 0)
            continue;
        const double nx = axis(line, 'X'), ny = axis(line, 'Y'), e = axis(line, 'E');
        if (!std::isnan(nx))
            x = nx;
        if (!std::isnan(ny))
            y = ny;
        // The footprint is where it extrudes, a purge line included; travel does not count
        if (!absolute_e && !std::isnan(e) && e > 0 && (!std::isnan(nx) || !std::isnan(ny))) {
            min_x = std::min(min_x, x), max_x = std::max(max_x, x);
            min_y = std::min(min_y, y), max_y = std::max(max_y, y);
        }
    }
    if (print_time.empty())
        return {};

    std::ostringstream out;
    out.setf(std::ios::fixed);
    out.precision(2);
    out << "\n; statistics = begin\n";
    if (!filament.empty())
        out << "; used_filament = " << filament << "\n";
    out << "; print_time = " << print_time << "\n";
    if (max_x >= min_x)
        out << "; model_size = " << max_x - min_x << "," << max_y - min_y << "," << (max_z.empty() ? "0" : max_z) << "\n";
    if (!layers.empty())
        out << "; total_layers = " << layers << "\n";
    out << "; statistics = end\n";
    return out.str();
}

nlohmann::json job_order(const std::string& printer_id, const std::string& project_id, JobOrder order)
{
    return {
        {"order_id", int(order)},
        {"printer_id", id_value(printer_id)},
        {"project_id", id_value(project_id)},
        {"data", nullptr},
        {"ams_info", nullptr},
        {"settings", nullptr},
    };
}

nlohmann::json print_settings_order(const std::string& printer_id, const std::string& project_id, const nlohmann::json& settings)
{
    return {
        {"order_id", 6},
        {"printer_id", id_value(printer_id)},
        {"project_id", id_value(project_id)},
        {"data", {{"settings", settings}}},
    };
}

nlohmann::json ace_drying_order(const std::string& printer_id, int box_id, int temp, int minutes)
{
    const bool on = minutes > 0;
    return {
        {"order_id", 1207},
        {"printer_id", id_value(printer_id)},
        {"project_id", 0},
        {"data",
         {{"multi_color_box",
           {{{"id", box_id},
             {"drying_status",
              {{"duration", on ? minutes : 0}, {"remain_time", nullptr}, {"status", on ? 1 : 0}, {"target_temp", on ? temp : 0}}}}}}}},
    };
}

nlohmann::json print_settings_from_gcode(const std::string& gcode)
{
    // One command per line, such as "M140 S70" or "M106 P2 S128"
    std::istringstream in(gcode);
    std::string        command;
    in >> command;
    double s = -1;
    int    p = 1;
    for (std::string word; in >> word;) {
        if (word[0] == ';')
            break;
        if (word.size() > 1 && (word[0] == 'S' || word[0] == 's'))
            s = std::strtod(word.c_str() + 1, nullptr);
        else if (word.size() > 1 && (word[0] == 'P' || word[0] == 'p'))
            p = std::atoi(word.c_str() + 1);
    }
    if (command == "M107")
        s = 0;
    if (s < 0)
        return nullptr;
    if (command == "M104" || command == "M109")
        return {{"target_nozzle_temp", int(std::lround(s))}};
    if (command == "M140" || command == "M190")
        return {{"target_hotbed_temp", int(std::lround(s))}};
    if (command == "M106" || command == "M107") {
        const int percent = int(std::lround(std::clamp(s, 0.0, 255.0) * 100 / 255));
        // P1 is the part cooling fan and P2 the auxiliary fan; the device page names no other here
        if (p == 1 || p == 0)
            return {{"fan_speed_pct", percent}};
        if (p == 2)
            return {{"aux_fan_speed_pct", percent}};
    }
    return nullptr;
}

nlohmann::json camera_open_order(const std::string& printer_id)
{
    return {
        {"order_id", 1001}, // open the camera
        {"printer_id", id_value(printer_id)},
        // Without it the cloud answers with the older credentials the printer no longer streams to
        {"shengwang_rtc_support", true},
    };
}

nlohmann::json camera_session(const nlohmann::json& order_answer)
{
    if (!order_answer.is_object() || !order_answer.contains("shengwang") || !order_answer["shengwang"].is_object())
        return nullptr;
    const nlohmann::json& agora = order_answer["shengwang"];
    nlohmann::json        session;
    for (const char* key : {"appid", "channel", "rtc_token", "client_uid", "encryption_mode", "encryption_key", "encryption_kdf_salt"}) {
        if (!agora.contains(key) || agora[key].is_null())
            return nullptr;
        session[key] = agora[key];
    }
    return session;
}

void parse_temperatures(const nlohmann::json& printer_info, int& nozzle, int& bed)
{
    nozzle = bed = -1;
    if (!printer_info.is_object())
        return;
    auto parameter = printer_info.find("parameter");
    if (parameter == printer_info.end() || !parameter->is_object())
        return;
    nozzle = json_int(*parameter, "curr_nozzle_temp", -1);
    bed    = json_int(*parameter, "curr_hotbed_temp", -1);
}

std::optional<Job> latest_job(const nlohmann::json& projects, const std::string& printer_id)
{
    if (!projects.is_array())
        return std::nullopt;
    // Newest first, as the cloud lists them
    for (const auto& project : projects) {
        if (!project.is_object() || json_string(project, "printer_id") != printer_id)
            continue;
        Job job;
        job.id             = json_string(project, "id");
        job.name           = json_string(project, "gcode_name");
        job.print_status   = json_int(project, "print_status");
        job.paused         = json_int(project, "pause") != 0;
        job.progress       = json_int(project, "progress", -1);
        job.remain_minutes = json_int(project, "remain_time", -1);
        // The layers are in the "settings" object, which the cloud sends as JSON text
        auto settings = project.find("settings");
        if (settings != project.end()) {
            const nlohmann::json parsed = settings->is_string() ? nlohmann::json::parse(settings->get<std::string>(), nullptr, false) : *settings;
            if (parsed.is_object()) {
                job.layer        = json_int(parsed, "curr_layer", -1);
                job.total_layers = json_int(parsed, "total_layers", -1);
            }
        }
        return job;
    }
    return std::nullopt;
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

bool Client::list_projects(nlohmann::json& projects, std::string& error)
{
    return login(error) && request("GET", "/work/project/getProjects?page=1&limit=10", nullptr, projects, error);
}

bool Client::send_order(const nlohmann::json& order, std::string& error)
{
    nlohmann::json answer;
    return send_order(order, answer, error);
}

bool Client::send_order(const nlohmann::json& order, nlohmann::json& answer, std::string& error)
{
    return login(error) && request("POST", "/work/operation/sendOrder", order, answer, error);
}

} // namespace AnycubicCloud
} // namespace Slic3r
