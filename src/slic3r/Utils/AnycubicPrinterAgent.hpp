#pragma once

#include "AnycubicCloud.hpp"
#include "IPrinterAgent.hpp"
#include "MoonrakerPrinterAgent.hpp"
#include "bambu_networking.hpp"

#include <atomic>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
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

    // The status message the device page reads, built from the cloud's view of the printer
    static nlohmann::json status_payload(const AnycubicCloud::Printer& printer);

protected:
    bool upload_gcode(const std::string& local_path,
                      const std::string& filename,
                      const std::string& base_url,
                      const std::string& api_key,
                      OnUpdateStatusFn   update_fn,
                      WasCancelledFn     cancel_fn) override;
    bool print_uploaded_file(const PrintParams& params, const std::string& filename) override;
    std::string ams_unit_name(int ams_id) const override { return "ACE " + std::to_string(ams_id + 1); }

private:
    // Finds the printer of the account the settings name; empty on failure, with `error` set
    std::string resolve_printer(AnycubicCloud::Client& client, AnycubicCloud::Printer& printer, std::string& error);
    void        run_connection(std::string dev_id, std::string token, uint64_t generation);
    void        report_token_refused(const std::string& dev_id, const std::string& error, const std::function<bool()>& stale);
    void        stop_connection_thread();

    std::mutex                              m_cloud_mutex; // Guards everything below
    std::shared_ptr<AnycubicCloud::Client>  m_client;
    std::string                             m_printer_id;
    int64_t                                 m_uploaded_file_id = -1;

    std::atomic<uint64_t> m_generation{0};
    std::thread           m_thread;
};

} // namespace Slic3r
