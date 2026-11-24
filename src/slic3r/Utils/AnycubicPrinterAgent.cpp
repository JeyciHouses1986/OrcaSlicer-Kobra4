#include "AnycubicPrinterAgent.hpp"

#include "AnycubicCloud.hpp"
#include "IPrinterAgent.hpp"
#include "MoonrakerPrinterAgent.hpp"
#include "NetworkAgentFactory.hpp"
#include "bambu_networking.hpp"
#include "libslic3r/PresetBundle.hpp"
#include "libslic3r/format.hpp"
#include "libslic3r/Utils.hpp"
#include "slic3r/GUI/DeviceCore/DevStorage.h"
#include "slic3r/GUI/GUI.hpp"
#include "slic3r/GUI/GUI_App.hpp"
#include "slic3r/GUI/I18N.hpp"

#include <boost/filesystem/path.hpp>

#include <boost/log/trivial.hpp>
#include <nlohmann/json.hpp>

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <functional>
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
    // Version and access code are answered from what the agent already holds
    if (json.is_object() && (json.contains("info") || json.contains("system")))
        return MoonrakerPrinterAgent::send_message(dev_id, json_str, qos, flag);
    // Printer control through the cloud is not implemented yet; the device page says so rather than doing nothing
    if (json.is_object() && json.contains("print"))
        return ORCA_NETWORK_ERR_CMD_NOT_SUPPORTED;
    return BAMBU_NETWORK_SUCCESS;
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
    std::string token;
    if (const PresetBundle* bundle = GUI::wxGetApp().preset_bundle)
        token = bundle->printers.get_edited_preset().config.opt_string("printhost_apikey");
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
    }

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
    }
    if (client.list_projects(data, error))
        state.job = AnycubicCloud::latest_job(data, printer_id);
    return state;
}

std::vector<MoonrakerPrinterAgent::AmsTrayData> AnycubicPrinterAgent::ace_trays(const std::vector<AnycubicCloud::AceSlot>& slots,
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
            tray.tray_info_idx = map_filament_type_to_generic_id(slot.material_type);
        }
        max_index = std::max(max_index, slot.index);
        trays.push_back(std::move(tray));
    }
    ace_count = max_index / 4 + 1;
    return trays;
}

nlohmann::json AnycubicPrinterAgent::device_status(const AnycubicCloud::Printer& printer, const CloudState& state) const
{
    nlohmann::json payload = status_payload(printer, state);
    if (!state.ace_slots.empty()) {
        int        ace_count = 0;
        const auto trays     = ace_trays(state.ace_slots, ace_count);
        payload["print"]["ams"] = ams_status_json(ace_count, ace_count * 4 - 1, trays);
    }
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
    dispatch_message(dev_id, device_status(printer, read_cloud_state(*client, printer.id)).dump());
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
        const CloudState state = read_cloud_state(*client, current->id);
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

    int64_t     file_id = -1;
    std::string error;
    const bool  ok = client->upload_file(
        local_path, filename,
        [&update_fn](int percent) {
            if (update_fn)
                update_fn(PrintingStageUpload, percent, _u8L("Uploading..."));
        },
        cancel_fn, file_id, error);
    if (!ok) {
        BOOST_LOG_TRIVIAL(error) << "AnycubicPrinterAgent: upload to Anycubic Cloud failed: " << error;
        return false;
    }

    std::lock_guard<std::mutex> lock(m_cloud_mutex);
    m_uploaded_file_id = file_id;
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
    std::string error;
    if (!client->send_order(order, error)) {
        BOOST_LOG_TRIVIAL(error) << "AnycubicPrinterAgent: the print order was refused: " << error;
        return false;
    }
    return true;
}

} // namespace Slic3r
