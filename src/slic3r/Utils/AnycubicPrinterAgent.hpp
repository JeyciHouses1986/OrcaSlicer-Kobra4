#pragma once

#include "AnycubicCloud.hpp"
#include "IPrinterAgent.hpp"
#include "MoonrakerPrinterAgent.hpp"
#include "bambu_networking.hpp"

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <vector>

namespace Slic3r {

// Anycubic printers reached through the Anycubic Cloud, such as the Kobra 4 with its ACE 2 Pro.
//
// The printer stays bound to the user's Anycubic account, so no LAN mode is needed: the agent talks only to the cloud.
// The printer settings hold the cloud token in the API key field, and optionally the printer's cloud name in the host
// field when the account has several printers. MoonrakerPrinterAgent is the base for its device record, discovery
// announcement, AMS payload and print flow (plate extraction, progress), while the transport is replaced: the
// connection checks the account and polls the printer's state, uploads go to the cloud's storage, and the print is
// started by a cloud order that carries the ACE slot chosen for each filament.
class AnycubicPrinterAgent final : public MoonrakerPrinterAgent
{
public:
    explicit AnycubicPrinterAgent(std::string log_dir);
    ~AnycubicPrinterAgent() override;

    static AgentInfo get_agent_info_static();
    AgentInfo        get_agent_info() override { return get_agent_info_static(); }

    int send_message(std::string dev_id, std::string json_str, int qos, int flag) override;
    int send_message_to_printer(std::string dev_id, std::string json_str, int qos, int flag) override;
    int connect_printer(const PrinterConnectionParams& params) override;
    int disconnect_printer() override;

    int start_print(PrintParams params, OnUpdateStatusFn update_fn, WasCancelledFn cancel_fn, OnWaitFn wait_fn) override;
    int start_local_print_with_record(PrintParams params, OnUpdateStatusFn update_fn, WasCancelledFn cancel_fn, OnWaitFn wait_fn) override;

    bool fetch_filament_info(std::string dev_id, FilamentSyncMode sync_mode = FilamentSyncMode::pull) override;

    // The camera streams through Agora's WebRTC service, which only its web SDK speaks: a local page plays it
    CameraStreamMode get_camera_stream_mode() const override { return CameraStreamMode::web_page; }
    std::string      get_camera_url() const override;
    void             request_camera_session(std::function<void(std::string)> callback) override;

    // What the cloud reports besides the printer list: temperatures and the newest job
    struct CloudState
    {
        int                               nozzle_temp = -1; // degrees, -1 when unknown
        int                               bed_temp    = -1;
        std::optional<AnycubicCloud::Job> job;
        std::vector<AnycubicCloud::AceSlot> ace_slots;
        std::vector<AnycubicCloud::AceUnit> ace_units;
    };

    // The status message the device page reads, built from the cloud's view of the printer
    static nlohmann::json status_payload(const AnycubicCloud::Printer& printer, const CloudState& state);

protected:
    bool upload_gcode(const std::string& local_path,
                      const std::string& filename,
                      const std::string& base_url,
                      const std::string& api_key,
                      OnUpdateStatusFn   update_fn,
                      WasCancelledFn     cancel_fn) override;
    bool print_uploaded_file(const PrintParams& params, const std::string& filename) override;
    std::string ams_unit_name(int ams_id) const override { return "ACE " + std::to_string(ams_id + 1); }
    // The ACE 2 Pro shows as a unit with a dryer and a humidity sensor, so the device page offers drying
    void decorate_ams_unit(int ams_id, nlohmann::json& unit) const override;

private:
    // Finds the printer of the account the settings name; empty on failure, with `error` set
    std::string resolve_printer(AnycubicCloud::Client& client, AnycubicCloud::Printer& printer, std::string& error);
    static CloudState read_cloud_state(AnycubicCloud::Client& client, const std::string& printer_id);
    // The ACE slots as trays of `ace_count` units
    static std::vector<AmsTrayData> ace_trays(const std::vector<AnycubicCloud::AceSlot>& slots, int& ace_count);
    // status_payload() with the ACE, so the device page shows its spools without a filament sync
    nlohmann::json device_status(const AnycubicCloud::Printer& printer, const CloudState& state) const;
    void        run_connection(std::string dev_id, std::string token, uint64_t generation);
    void        report_token_refused(const std::string& dev_id, const std::string& error, const std::function<bool()>& stale);
    void        stop_connection_thread();
    // The cloud order a device page command stands for, or null when the cloud has none
    nlohmann::json order_for_command(const nlohmann::json& print) const;
    void           send_order_async(nlohmann::json order);
    void           send_orders_async(std::vector<nlohmann::json> orders);

    // Orders reach the printer one at a time and in the order the device page gave them, a second apart: the printer
    // drops an order that arrives while it handles the previous one. Sender threads outlive a call, so they share it.
    struct OrderQueue
    {
        std::mutex                            mutex;
        std::condition_variable               turn_changed;
        uint64_t                              next_ticket = 0; // Handed to the next call
        uint64_t                              serving     = 0; // The ticket whose orders go out now
        std::chrono::steady_clock::time_point last_sent{};     // Only the serving thread touches it
    };
    std::shared_ptr<OrderQueue> m_order_queue = std::make_shared<OrderQueue>();

    mutable std::mutex                      m_cloud_mutex; // Guards everything below
    std::shared_ptr<AnycubicCloud::Client>  m_client;
    std::string                             m_printer_id;
    int64_t                                 m_uploaded_file_id = -1;
    std::string                             m_job_id; // The running job's, empty when the printer is idle
    std::vector<AnycubicCloud::AceUnit>     m_ace_units;
    // The light as last switched from the device page: the cloud does not report it, only the printer does, over MQTT
    bool                                    m_light_on = false;
    WasCancelledFn                          m_upload_cancelled; // The print job's, kept for the print order

    std::atomic<uint64_t> m_generation{0};
    std::thread           m_thread;
};

} // namespace Slic3r
