#include "AnycubicPrinterAgent.hpp"

#include "AnycubicCloud.hpp"
#include "IPrinterAgent.hpp"
#include "MoonrakerPrinterAgent.hpp"
#include "NetworkAgentFactory.hpp"
#include "bambu_networking.hpp"
#include "libslic3r/AppConfig.hpp"
#include "libslic3r/PresetBundle.hpp"
#include "libslic3r/PrintConfig.hpp"
#include "libslic3r/format.hpp"
#include "libslic3r/Utils.hpp"
#include "slic3r/GUI/DeviceCore/DevManager.h"
#include "slic3r/GUI/DeviceCore/DevStorage.h"
#include "slic3r/GUI/DeviceManager.hpp"
#include "slic3r/GUI/GUI.hpp"
#include "slic3r/GUI/GUI_App.hpp"
#include "slic3r/GUI/I18N.hpp"

#include <boost/filesystem/operations.hpp>
#include <boost/filesystem/path.hpp>
#include <boost/nowide/fstream.hpp>

#include <boost/algorithm/string/predicate.hpp>
#include <boost/log/trivial.hpp>
#include <nlohmann/json.hpp>

#include <algorithm>
#include <array>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <utility>
#include <vector>

namespace Slic3r {
namespace {

const std::string AnycubicPrinterAgent_VERSION = "0.1.0";

// The cloud's view of the printer changes slowly, and the account is rate limited
constexpr std::chrono::seconds status_poll_interval{15};
constexpr std::chrono::seconds first_retry_delay{2};
// The cloud reads an uploaded file before it can be printed, which takes a few seconds
constexpr std::chrono::seconds file_read_poll{3};
constexpr int                  file_read_attempts = 40;

// Sleeps in short steps, so a disconnect does not wait out the delay. Returns false once `stale` is true.
template<typename Stale> bool wait_unless(std::chrono::milliseconds delay, Stale stale)
{
    const auto step = std::chrono::milliseconds(100);
    for (auto waited = std::chrono::milliseconds(0); waited < delay; waited += step) {
        if (stale())
            return false;
        std::this_thread::sleep_for(step);
    }
    return !stale();
}

} // namespace

AnycubicPrinterAgent::AnycubicPrinterAgent(std::string log_dir) : MoonrakerPrinterAgent(std::move(log_dir)) {}

AnycubicPrinterAgent::~AnycubicPrinterAgent() { stop_connection_thread(); }

AgentInfo AnycubicPrinterAgent::get_agent_info_static()
{
    return AgentInfo{ANYCUBIC_PRINTER_AGENT_ID, "Anycubic Cloud", AnycubicPrinterAgent_VERSION,
                     "Anycubic printers through the Anycubic Cloud, with ACE slot mapping"};
}

namespace {
const char* gcode_state(const AnycubicCloud::Printer& printer, const std::optional<AnycubicCloud::Job>& job)
{
    using AnycubicCloud::Job;
    if (!job)
        return printer.busy() ? "RUNNING" : "IDLE";
    if (printer.busy()) {
        switch (job->print_status) {
        case Job::downloading:
        case Job::checking:
        case Job::preheating: return "PREPARE";
        default: return job->paused ? "PAUSE" : "RUNNING";
        }
    }
    // An idle printer shows how its last job ended
    switch (job->print_status) {
    case Job::complete: return "FINISH";
    case Job::cancelled: return "FAILED";
    default: return "IDLE";
    }
}
} // namespace

nlohmann::json AnycubicPrinterAgent::status_payload(const AnycubicCloud::Printer& printer, const CloudState& state)
{
    nlohmann::json payload;
    auto&          print      = payload["print"];
    print["command"]            = "push_status";
    print["msg"]                = 0;
    print["support_mqtt_alive"] = true;
    print["gcode_state"]        = gcode_state(printer, state.job);
    print["mc_print_stage"]     = printer.busy() ? 1 : 0;
    if (state.nozzle_temp >= 0)
        print["nozzle_temper"] = state.nozzle_temp;
    if (state.bed_temp >= 0)
        print["bed_temper"] = state.bed_temp;
    if (const auto& job = state.job) {
        print["subtask_name"] = job->name;
        print["gcode_file"]   = job->name;
        if (job->progress >= 0)
            print["mc_percent"] = std::clamp(job->progress, 0, 100);
        if (job->remain_minutes >= 0)
            print["mc_remaining_time"] = job->remain_minutes;
        if (job->layer >= 0)
            print["layer_num"] = job->layer;
        if (job->total_layers >= 0)
            print["total_layer_num"] = job->total_layers;
    }
    print["mc_print_error_code"] = 0;
    print["print_error"]         = 0;
    // The file goes to the cloud, never to the printer's storage, but the send dialog wants storage to be present.
    // Every push sets it twice, from "sdcard" and from home_flag bits 8-9.
    print["sdcard"]             = true;
    print["home_flag"]          = DevStorage::HAS_SDCARD_NORMAL << 8;
    print["support_send_to_sd"] = false;
    // The printer records it with its camera when the print order asks
    print["support_timelapse"]  = true;
    print["nozzle_temp_range"]  = {0, 300};
    print["bed_temp_range"]     = {0, 110};
    payload["t_utc"]            = std::chrono::duration_cast<std::chrono::milliseconds>(
                           std::chrono::system_clock::now().time_since_epoch())
                           .count();
    return payload;
}

int AnycubicPrinterAgent::send_message(std::string dev_id, std::string json_str, int qos, int flag)
{
    const auto json = nlohmann::json::parse(json_str, nullptr, false);
    // The device page switches the light once per light node. The Kobra 4 reports its light as type 3 (the Kobra S1
    // uses 2, older models 1), so the first node sends that order and the second is answered without one.
    if (json.is_object() && json.contains("system") && json["system"].is_object() &&
        json["system"].value("command", std::string()) == "ledctrl") {
        const std::string node = json["system"].value("led_node", std::string());
        const bool        on   = json["system"].value("led_mode", std::string()) == "on";
        std::string       printer_id;
        {
            std::lock_guard<std::mutex> lock(m_cloud_mutex);
            printer_id  = m_printer_id;
            m_light_on  = on;
        }
        if (printer_id.empty() || (node != "chamber_light" && node != "chamber_light2"))
            return ORCA_NETWORK_ERR_CMD_NOT_SUPPORTED;
        if (node == "chamber_light")
            send_order_async(AnycubicCloud::light_order(printer_id, 3, on));
        return BAMBU_NETWORK_SUCCESS;
    }
    // Version and access code are answered from what the agent already holds
    if (json.is_object() && (json.contains("info") || json.contains("system")))
        return MoonrakerPrinterAgent::send_message(dev_id, json_str, qos, flag);
    if (json.is_object() && json.contains("print") && json["print"].is_object()) {
        const bool     slot_setting = json["print"].value("command", std::string()) == "ams_filament_setting";
        nlohmann::json order;
        try {
            order = slot_setting ? slot_setting_order(json["print"]) : order_for_command(json["print"]);
        } catch (const nlohmann::json::exception& e) {
            BOOST_LOG_TRIVIAL(warning) << "AnycubicPrinterAgent: unexpected device page command: " << e.what();
        }
        // A slot that already holds the chosen material and colour needs no order
        if (order.is_null() && slot_setting)
            return BAMBU_NETWORK_SUCCESS;
        // The device page says so rather than doing nothing
        if (order.is_null()) {
            BOOST_LOG_TRIVIAL(info) << "AnycubicPrinterAgent: no cloud order for device page command "
                                    << json["print"].value("command", std::string());
            return ORCA_NETWORK_ERR_CMD_NOT_SUPPORTED;
        }
        send_order_async(std::move(order));
    }
    return BAMBU_NETWORK_SUCCESS;
}

nlohmann::json AnycubicPrinterAgent::order_for_command(const nlohmann::json& print) const
{
    std::string printer_id, job_id;
    {
        std::lock_guard<std::mutex> lock(m_cloud_mutex);
        printer_id = m_printer_id;
        job_id     = m_job_id;
    }
    const std::string command = print.value("command", "");
    if (printer_id.empty())
        return nullptr;

    // The dryer works whether or not the printer prints. Orca gives hours; the cloud takes minutes.
    if (command == "ams_filament_drying") {
        const int ams_id = print.value("ams_id", 0);
        const int mode   = print.value("mode", 0);
        return AnycubicCloud::ace_drying_order(printer_id, ams_id, print.value("temp", 0), mode == 0 ? 0 : print.value("duration", 0) * 60);
    }

    // Temperatures, fans and moves are orders on the printer itself, which work whether or not it prints
    if (command == "set_bed_temp" && print.contains("temp") && print["temp"].is_number())
        return AnycubicCloud::temperature_order(printer_id, -1, print["temp"].get<int>());
    if (command == "set_nozzle_temp" && print.contains("target_temp") && print["target_temp"].is_number())
        return AnycubicCloud::temperature_order(printer_id, print["target_temp"].get<int>(), -1);
    if (command == "gcode_line" && print.contains("param") && print["param"].is_string())
        return AnycubicCloud::order_from_gcode(printer_id, print["param"].get<std::string>());

    // Pause, resume and stop act on the running job
    if (job_id.empty())
        return nullptr;
    using AnycubicCloud::JobOrder;
    if (command == "pause")
        return AnycubicCloud::job_order(printer_id, job_id, JobOrder::pause);
    if (command == "resume")
        return AnycubicCloud::job_order(printer_id, job_id, JobOrder::resume);
    if (command == "stop")
        return AnycubicCloud::job_order(printer_id, job_id, JobOrder::stop);
    return nullptr;
}

nlohmann::json AnycubicPrinterAgent::slot_setting_order(const nlohmann::json& print)
{
    const int  ams_id  = print.value("ams_id", 0);
    const int  slot_id = print.value("slot_id", 0);
    const int  index   = ams_id * 4 + slot_id;
    SlotChoice choice{print.value("tray_type", std::string()), AnycubicCloud::rgb_from_hex(print.value("tray_color", std::string())),
                      print.value("tray_info_idx", std::string())};
    if (choice.material_type.empty() || ams_id < 0 || slot_id < 0 || slot_id > 3)
        return nullptr;

    std::string                           printer_id;
    std::optional<AnycubicCloud::AceSlot> current;
    {
        std::lock_guard<std::mutex> lock(m_cloud_mutex);
        printer_id = m_printer_id;
        for (const AnycubicCloud::AceSlot& slot : m_ace_slots)
            if (slot.index == index)
                current = slot;
        // The printer's own name for a variant is kept, such as "PLA Glow" for a PLA preset
        if (current && current->loaded && boost::istarts_with(current->material_type, choice.material_type))
            choice.material_type = current->material_type;
        m_slot_choices[index] = choice;
    }
    const std::string saved = nlohmann::json{{"type", choice.material_type}, {"color", choice.rgb}, {"filament_id", choice.filament_id}}.dump();
    run_on_main([index, saved]() {
        if (AppConfig* config = GUI::wxGetApp().app_config)
            config->set("anycubic_ace_slots", "slot" + std::to_string(index), saved);
    });

    if (printer_id.empty() || (current && current->loaded && current->material_type == choice.material_type && current->rgb == choice.rgb))
        return nullptr;
    return AnycubicCloud::ace_slot_order(printer_id, ams_id, slot_id, choice.material_type, choice.rgb);
}

void AnycubicPrinterAgent::load_slot_choices()
{
    const AppConfig* config = GUI::wxGetApp().app_config;
    if (!config || !config->has_section("anycubic_ace_slots"))
        return;
    std::map<int, SlotChoice> choices;
    for (const auto& [key, value] : config->get_section("anycubic_ace_slots")) {
        const auto saved = nlohmann::json::parse(value, nullptr, false);
        if (key.rfind("slot", 0) != 0 || !saved.is_object())
            continue;
        SlotChoice choice;
        choice.material_type = saved.value("type", std::string());
        choice.filament_id   = saved.value("filament_id", std::string());
        if (saved.contains("color") && saved["color"].is_array() && saved["color"].size() == 3)
            for (size_t i = 0; i < 3; ++i)
                choice.rgb[i] = saved["color"][i].is_number_integer() ? saved["color"][i].get<int>() : 0;
        choices[std::atoi(key.c_str() + 4)] = std::move(choice);
    }
    std::lock_guard<std::mutex> lock(m_cloud_mutex);
    m_slot_choices = std::move(choices);
}

void AnycubicPrinterAgent::send_order_async(nlohmann::json order)
{
    send_orders_async({std::move(order)});
}

void AnycubicPrinterAgent::send_orders_async(std::vector<nlohmann::json> orders)
{
    std::shared_ptr<AnycubicCloud::Client> client;
    {
        std::lock_guard<std::mutex> lock(m_cloud_mutex);
        client = m_client;
    }
    if (!client)
        return;
    // The device page's command must not wait for the cloud; the next poll shows the result.
    // Orders go one after another: the printer drops an order that arrives while it handles the previous one.
    std::thread([client, orders = std::move(orders)]() {
        for (size_t i = 0; i < orders.size(); ++i) {
            if (i > 0)
                std::this_thread::sleep_for(std::chrono::seconds(1));
            const nlohmann::json& order = orders[i];
            nlohmann::json        answer;
            std::string           error;
            if (client->send_order(order, answer, error))
                BOOST_LOG_TRIVIAL(info) << "AnycubicPrinterAgent: order " << order.dump() << " answered " << answer.dump();
            else
                BOOST_LOG_TRIVIAL(warning) << "AnycubicPrinterAgent: order " << order.dump() << " was refused: " << error;
        }
    }).detach();
}

void AnycubicPrinterAgent::decorate_ams_unit(int ams_id, nlohmann::json& unit) const
{
    AnycubicCloud::AceUnit ace;
    {
        std::lock_guard<std::mutex> lock(m_cloud_mutex);
        if (ams_id < 0 || ams_id >= int(m_ace_units.size()))
            return;
        ace = m_ace_units[ams_id];
    }
    // "info" holds the unit type in bits 0-3 (3, a unit with a dryer), the dryer state in bits 4-7 (2 while drying)
    // and its stage in bits 22-23. The cloud reports no stage, but the drying dialog shows "Idle" without one, so a
    // drying ACE is reported as heating.
    constexpr int with_dryer = 3, drying = 2 << 4, heating = 1 << 22;
    char          info[12];
    std::snprintf(info, sizeof(info), "%X", with_dryer | (ace.drying == 1 ? drying | heating : 0));
    unit["info"] = info;
    if (ace.humidity >= 0)
        unit["humidity_raw"] = std::to_string(ace.humidity);
    if (ace.temp >= 0)
        unit["temp"] = std::to_string(ace.temp);
    unit["dry_time"]    = ace.drying == 1 ? ace.dry_remain : 0;
    unit["dry_setting"] = {{"dry_temperature", ace.dry_target_temp}, {"dry_duration", ace.dry_duration / 60}};
}

int AnycubicPrinterAgent::send_message_to_printer(std::string dev_id, std::string json_str, int qos, int flag)
{
    return send_message(std::move(dev_id), std::move(json_str), qos, flag);
}

int AnycubicPrinterAgent::connect_printer(const PrinterConnectionParams& params)
{
    if (params.dev_id.empty()) {
        BOOST_LOG_TRIVIAL(error) << "AnycubicPrinterAgent: connect_printer without dev_id";
        return BAMBU_NETWORK_ERR_INVALID_HANDLE;
    }

    // The preset holds the current token; the device record may still hold the one it was created with
    std::string token, nozzle_diameter, nozzle_type;
    if (const PresetBundle* bundle = GUI::wxGetApp().preset_bundle) {
        const DynamicPrintConfig& config = bundle->printers.get_edited_preset().config;
        token = config.opt_string("printhost_apikey");
        if (const auto* diameters = config.option<ConfigOptionFloats>("nozzle_diameter"); diameters && !diameters->values.empty()) {
            char text[16];
            std::snprintf(text, sizeof(text), "%.1f", diameters->values.front());
            nozzle_diameter = text;
        }
        if (const auto* types = config.option<ConfigOptionEnumsGenericNullable>("nozzle_type"); types && !types->values.empty())
            if (const auto name = NozzleTypeEumnToStr.find(NozzleType(types->values.front())); name != NozzleTypeEumnToStr.end())
                nozzle_type = name->second;
    }
    if (token.empty())
        token = params.password;

    stop_connection_thread();
    init_device_info(params.dev_id, params.host, params.username, token, false, params.port);
    // Files go to the cloud, never to the printer's storage, so there is always room for one
    has_storage.store(true);
    {
        std::lock_guard<std::mutex> lock(m_cloud_mutex);
        m_client.reset();
        m_printer_id.clear();
        m_nozzle_diameter = nozzle_diameter;
        m_nozzle_type     = nozzle_type;
    }
    load_slot_choices();

    const uint64_t generation = ++m_generation;
    m_thread = std::thread([this, dev_id = params.dev_id, token, generation]() { run_connection(dev_id, token, generation); });
    return BAMBU_NETWORK_SUCCESS;
}

int AnycubicPrinterAgent::disconnect_printer()
{
    stop_connection_thread();
    std::lock_guard<std::mutex> lock(m_cloud_mutex);
    m_client.reset();
    m_printer_id.clear();
    device_info = MoonrakerDeviceInfo{};
    return BAMBU_NETWORK_SUCCESS;
}

void AnycubicPrinterAgent::stop_connection_thread()
{
    ++m_generation;
    if (m_thread.joinable())
        m_thread.join();
}

std::string AnycubicPrinterAgent::resolve_printer(AnycubicCloud::Client& client, AnycubicCloud::Printer& printer, std::string& error)
{
    std::vector<AnycubicCloud::Printer> printers;
    if (!client.list_printers(printers, error))
        return {};
    // The host field may name the printer; anything else there (a placeholder) picks the account's only printer
    if (const AnycubicCloud::Printer* chosen = AnycubicCloud::choose_printer(printers, device_info.dev_ip)) {
        printer = *chosen;
        return printer.id;
    }
    error = printers.empty() ? "No printer is bound to this Anycubic account" :
                               "Several printers are bound to this Anycubic account; enter the name of one as the host";
    return {};
}

void AnycubicPrinterAgent::report_token_refused(const std::string& dev_id, const std::string& error, const std::function<bool()>& stale)
{
    // Retrying cannot help: the user has to log in to Anycubic again for a new token
    if (stale())
        return;
    // TRN %1% is the reason Anycubic Cloud gives, such as "Login information has expired. Please login again."
    dispatch_local_connect(ConnectStatusFailed, dev_id,
                           format(_u8L("Anycubic Cloud refused the login: %1%\nOpen the printer's connection settings and log in to "
                                       "Anycubic again."),
                                  error));
}

AnycubicPrinterAgent::CloudState AnycubicPrinterAgent::read_cloud_state(AnycubicCloud::Client& client, const std::string& printer_id)
{
    // Each part is optional: the page shows what the cloud answered
    CloudState     state;
    nlohmann::json data;
    std::string    error;
    if (client.printer_info(printer_id, data, error)) {
        AnycubicCloud::parse_temperatures(data, state.nozzle_temp, state.bed_temp);
        state.ace_slots = AnycubicCloud::parse_ace_slots(data);
        state.ace_units = AnycubicCloud::parse_ace_units(data);
    }
    if (client.list_projects(data, error))
        state.job = AnycubicCloud::latest_job(data, printer_id);
    return state;
}

std::vector<MoonrakerPrinterAgent::AmsTrayData> AnycubicPrinterAgent::ace_trays(const std::vector<AnycubicCloud::AceSlot>& slots,
                                                                                const std::map<int, SlotChoice>&           choices,
                                                                                int&                                       ace_count)
{
    std::vector<AmsTrayData> trays;
    int                      max_index = 0;
    for (const AnycubicCloud::AceSlot& slot : slots) {
        AmsTrayData tray;
        tray.slot_index   = slot.index;
        tray.has_filament = slot.loaded;
        if (slot.loaded) {
            tray.tray_type     = slot.material_type;
            tray.tray_color    = AnycubicCloud::hex_from_rgb(slot.rgb);
            const auto choice  = choices.find(slot.index);
            const bool chosen  = choice != choices.end() && !choice->second.filament_id.empty() &&
                                choice->second.material_type == slot.material_type && choice->second.rgb == slot.rgb;
            tray.tray_info_idx = chosen ? choice->second.filament_id : map_filament_type_to_generic_id(slot.material_type);
        }
        max_index = std::max(max_index, slot.index);
        trays.push_back(std::move(tray));
    }
    ace_count = max_index / 4 + 1;
    return trays;
}

nlohmann::json AnycubicPrinterAgent::device_status(const AnycubicCloud::Printer& printer, const CloudState& state) const
{
    nlohmann::json            payload = status_payload(printer, state);
    bool                      light_on;
    std::map<int, SlotChoice> choices;
    std::string               nozzle_diameter, nozzle_type;
    {
        std::lock_guard<std::mutex> lock(m_cloud_mutex);
        light_on        = m_light_on;
        choices         = m_slot_choices;
        nozzle_diameter = m_nozzle_diameter;
        nozzle_type     = m_nozzle_type;
    }
    if (!state.ace_slots.empty()) {
        int        ace_count = 0;
        const auto trays     = ace_trays(state.ace_slots, choices, ace_count);
        payload["print"]["ams"] = ams_status_json(ace_count, ace_count * 4 - 1, trays);
    }
    // With a nozzle the device page finds the printer preset, and with it the filament presets for the ACE slots
    if (!nozzle_diameter.empty() && !nozzle_type.empty()) {
        payload["print"]["nozzle_diameter"] = nozzle_diameter;
        payload["print"]["nozzle_type"]     = nozzle_type;
    }
    payload["print"]["lights_report"] = nlohmann::json::array({{{"node", "chamber_light"}, {"mode", light_on ? "on" : "off"}}});
    return payload;
}

void AnycubicPrinterAgent::run_connection(std::string dev_id, std::string token, uint64_t generation)
{
    auto stale  = [this, generation]() { return generation != m_generation.load(); };
    auto client = std::make_shared<AnycubicCloud::Client>(token);

    // Connect, retrying with backoff until superseded: the token may be wrong, or the cloud briefly unreachable
    AnycubicCloud::Printer printer;
    std::string            error;
    for (int attempt = 0; resolve_printer(*client, printer, error).empty(); ++attempt) {
        BOOST_LOG_TRIVIAL(warning) << "AnycubicPrinterAgent: cannot reach the printer through Anycubic Cloud: " << error;
        if (client->token_refused()) {
            report_token_refused(dev_id, error, stale);
            return;
        }
        if (!wait_unless(first_retry_delay * (1 << std::min(attempt, 5)), stale))
            return;
    }

    {
        std::lock_guard<std::mutex> lock(m_cloud_mutex);
        if (stale())
            return;
        m_client     = client;
        m_printer_id = printer.id;
        device_info.dev_name = printer.name;
    }
    BOOST_LOG_TRIVIAL(info) << "AnycubicPrinterAgent: connected to " << printer.model << " through Anycubic Cloud";
    // The device page offers the ACE's dryer only to a printer that says it can dry remotely
    run_on_main([dev_id]() {
        if (auto* manager = GUI::wxGetApp().getDeviceManager())
            if (MachineObject* obj = manager->get_my_machine(dev_id))
                obj->is_support_remote_dry = true;
    });
    // What the device page's commands act on
    const auto remember = [this](const AnycubicCloud::Printer& current, const CloudState& state) {
        std::lock_guard<std::mutex> lock(m_cloud_mutex);
        m_job_id    = current.busy() && state.job ? state.job->id : std::string();
        m_ace_units = state.ace_units;
        m_ace_slots = state.ace_slots;
    };
    CloudState state = read_cloud_state(*client, printer.id);
    remember(printer, state);
    dispatch_message(dev_id, device_status(printer, state).dump());
    dispatch_local_connect(ConnectStatusOk, dev_id, "0");
    dispatch_printer_connected(dev_id);

    // Keep the device page's state current
    while (wait_unless(status_poll_interval, stale)) {
        std::vector<AnycubicCloud::Printer> printers;
        if (!client->list_printers(printers, error)) {
            BOOST_LOG_TRIVIAL(warning) << "AnycubicPrinterAgent: status poll failed: " << error;
            if (client->token_refused()) {
                report_token_refused(dev_id, error, stale);
                return;
            }
            continue;
        }
        const AnycubicCloud::Printer* current = AnycubicCloud::choose_printer(printers, printer.id);
        if (current == nullptr)
            continue;
        state = read_cloud_state(*client, current->id);
        remember(*current, state);
        if (!stale())
            dispatch_message(dev_id, device_status(*current, state).dump());
    }
}

bool AnycubicPrinterAgent::fetch_filament_info(std::string dev_id, FilamentSyncMode /*sync_mode*/)
{
    (void) dev_id;
    std::shared_ptr<AnycubicCloud::Client> client;
    std::string                            printer_id;
    {
        std::lock_guard<std::mutex> lock(m_cloud_mutex);
        client     = m_client;
        printer_id = m_printer_id;
    }
    if (!client || printer_id.empty())
        return false;

    nlohmann::json info;
    std::string    error;
    if (!client->printer_info(printer_id, info, error)) {
        BOOST_LOG_TRIVIAL(warning) << "AnycubicPrinterAgent: reading the ACE failed: " << error;
        return false;
    }
    const std::vector<AnycubicCloud::AceSlot> slots = AnycubicCloud::parse_ace_slots(info);
    if (slots.empty())
        return false;

    int                            ace_count = 0;
    const std::vector<AmsTrayData> trays     = ace_trays(slots, ace_count);
    build_ams_payload(ace_count, ace_count * 4 - 1, trays);
    return true;
}

std::string AnycubicPrinterAgent::get_camera_url() const
{
    {
        std::lock_guard<std::mutex> lock(m_cloud_mutex);
        if (m_printer_id.empty())
            return {};
    }
    // The page asks for its session itself, so the address stays the same from one viewing to the next
    return GUI::file_url_from_path(boost::filesystem::path(resources_dir()) / "web/anycubic/camera.html").ToUTF8().data();
}

void AnycubicPrinterAgent::request_camera_session(std::function<void(std::string)> callback)
{
    std::shared_ptr<AnycubicCloud::Client> client;
    std::string                            printer_id;
    {
        std::lock_guard<std::mutex> lock(m_cloud_mutex);
        client     = m_client;
        printer_id = m_printer_id;
    }
    if (!client || printer_id.empty()) {
        callback({});
        return;
    }
    // The page has no translations of its own, so it gets the words it may show
    nlohmann::json reply = {
        {"messages",
         {{"waiting", _u8L("Waiting for the printer's camera...")},
          {"ended", _u8L("The camera stream ended. Press play to watch it again.")},
          {"no_player", _u8L("The camera player could not be loaded. Check the internet connection.")},
          {"failed", _u8L("Anycubic Cloud did not open the camera.")}}},
    };
    // Each viewing needs fresh credentials, and asking for them opens the camera. The thread holds only what it
    // uses, so it may outlive a disconnect.
    std::thread([client, printer_id, reply = std::move(reply), callback = std::move(callback)]() mutable {
        nlohmann::json answer;
        std::string    error;
        nlohmann::json session;
        if (!client->send_order(AnycubicCloud::camera_open_order(printer_id), answer, error))
            BOOST_LOG_TRIVIAL(warning) << "AnycubicPrinterAgent: opening the camera failed: " << error;
        else if ((session = AnycubicCloud::camera_session(answer)).is_null())
            BOOST_LOG_TRIVIAL(warning) << "AnycubicPrinterAgent: Anycubic Cloud opened the camera without Agora credentials";

        if (session.is_null())
            reply["error"] = true;
        else
            reply["session"] = std::move(session);
        callback(reply.dump());
    }).detach();
}

int AnycubicPrinterAgent::start_print(PrintParams params, OnUpdateStatusFn update_fn, WasCancelledFn cancel_fn, OnWaitFn /*wait_fn*/)
{
    // Every print goes through the cloud, whichever way the dialog sends it
    return start_local_print(std::move(params), std::move(update_fn), std::move(cancel_fn));
}

int AnycubicPrinterAgent::start_local_print_with_record(PrintParams params, OnUpdateStatusFn update_fn, WasCancelledFn cancel_fn, OnWaitFn /*wait_fn*/)
{
    return start_local_print(std::move(params), std::move(update_fn), std::move(cancel_fn));
}

bool AnycubicPrinterAgent::upload_gcode(const std::string& local_path,
                                        const std::string& filename,
                                        const std::string& /*base_url*/,
                                        const std::string& /*api_key*/,
                                        OnUpdateStatusFn   update_fn,
                                        WasCancelledFn     cancel_fn)
{
    std::shared_ptr<AnycubicCloud::Client> client;
    {
        std::lock_guard<std::mutex> lock(m_cloud_mutex);
        client             = m_client;
        m_uploaded_file_id = -1;
    }
    if (!client) {
        BOOST_LOG_TRIVIAL(error) << "AnycubicPrinterAgent: upload before the printer was reached through Anycubic Cloud";
        return false;
    }

    // The cloud reads the print time and model size from Slicer Next's statistics block, which Orca does not write:
    // the upload is a copy with one added
    std::string upload_path = local_path;
    {
        boost::nowide::ifstream in(local_path, std::ios::binary);
        const std::string       block = in ? AnycubicCloud::statistics_block(in) : std::string();
        if (!block.empty()) {
            const auto copy = boost::filesystem::temp_directory_path() / boost::filesystem::unique_path("orca-anycubic-%%%%%%%%.gcode");
            boost::system::error_code ec;
            boost::filesystem::copy_file(local_path, copy, ec);
            boost::nowide::ofstream out(copy.string(), std::ios::binary | std::ios::app);
            if (!ec && out && (out << block)) {
                upload_path = copy.string();
            } else {
                BOOST_LOG_TRIVIAL(warning) << "AnycubicPrinterAgent: could not add the statistics block; uploading the G-code as it is";
                boost::filesystem::remove(copy, ec);
            }
        }
    }

    int64_t     file_id = -1;
    std::string error;
    const bool  ok = client->upload_file(
        upload_path, filename,
        [&update_fn](int percent) {
            if (update_fn)
                update_fn(PrintingStageUpload, percent, _u8L("Uploading..."));
        },
        cancel_fn, file_id, error);
    if (upload_path != local_path) {
        boost::system::error_code ec;
        boost::filesystem::remove(upload_path, ec);
    }
    if (!ok) {
        BOOST_LOG_TRIVIAL(error) << "AnycubicPrinterAgent: upload to Anycubic Cloud failed: " << error;
        return false;
    }

    std::lock_guard<std::mutex> lock(m_cloud_mutex);
    m_uploaded_file_id = file_id;
    m_upload_cancelled = cancel_fn;
    return true;
}

bool AnycubicPrinterAgent::print_uploaded_file(const PrintParams& params, const std::string& /*filename*/)
{
    std::shared_ptr<AnycubicCloud::Client> client;
    std::string                            printer_id;
    int64_t                                file_id = -1;
    {
        std::lock_guard<std::mutex> lock(m_cloud_mutex);
        client     = m_client;
        printer_id = m_printer_id;
        file_id    = m_uploaded_file_id;
    }
    if (!client || printer_id.empty() || file_id < 0)
        return false;

    const auto  order = AnycubicCloud::start_print_order(printer_id, file_id,
                                                         AnycubicCloud::slot_mapping_from_ams_mapping_info(params.ams_mapping_info),
                                                         params.task_record_timelapse);
    WasCancelledFn cancelled;
    {
        std::lock_guard<std::mutex> lock(m_cloud_mutex);
        cancelled = m_upload_cancelled;
    }
    std::string error;
    for (int attempt = 1; !client->send_order(order, error); ++attempt) {
        if (!AnycubicCloud::file_not_ready(error) || attempt == file_read_attempts) {
            BOOST_LOG_TRIVIAL(error) << "AnycubicPrinterAgent: the print order was refused: " << error;
            return false;
        }
        BOOST_LOG_TRIVIAL(info) << "AnycubicPrinterAgent: Anycubic Cloud is still reading the file (" << error << "), retrying";
        if (!wait_unless(file_read_poll, [&cancelled]() { return cancelled && cancelled(); }))
            return false;
    }
    return true;
}

} // namespace Slic3r
