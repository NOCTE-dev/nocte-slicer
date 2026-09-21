// NØCTE Slicer — Copyright (c) 2026 NØCTE Engineering. AGPL-3.0-or-later.
//
// The NØCTE LAN Developer Mode printer agent (ADR-002): the only supported way for NØCTE Slicer
// to drive a Bambu Lab printer. It is 100% NØCTE code with no proprietary dependency — MQTT over
// TLS on 8883 for state and commands, FTPS on 990 for the job file.
//
// Report payloads are handed to the GUI verbatim: MachineObject::parse_json("lan", msg) already
// understands Bambu's JSON, so nothing is translated on the way through.
//
// Printing is gated. Every path that could publish a `project_file` command consults
// m_allow_print, which defaults to false and can only be changed by set_allow_print(). Nothing
// in this build calls it, and the diagnostics probe has no access to it.

#ifndef slic3r_Utils_Nocte_NocteLanPrinterAgent_hpp_
#define slic3r_Utils_Nocte_NocteLanPrinterAgent_hpp_

#include "slic3r/Utils/ICloudServiceAgent.hpp"
#include "slic3r/Utils/IPrinterAgent.hpp"
#include "slic3r/Utils/Nocte/NocteMqttClient.hpp"

#include <atomic>
#include <cstdint>
#include <memory>
#include <mutex>
#include <string>

namespace Slic3r {

class NocteLanPrinterAgent : public IPrinterAgent
{
public:
    explicit NocteLanPrinterAgent(std::string log_dir);
    ~NocteLanPrinterAgent() override;

    void set_cloud_agent(std::shared_ptr<ICloudServiceAgent> cloud) override;

    // Communication
    int send_message(std::string dev_id, std::string json_str, int qos, int flag) override;
    int connect_printer(std::string dev_id, std::string dev_ip, std::string username, std::string password, bool use_ssl) override;
    int disconnect_printer() override;
    int send_message_to_printer(std::string dev_id, std::string json_str, int qos, int flag) override;

    // Certificates
    int  check_cert() override;
    void install_device_cert(std::string dev_id, bool lan_only) override;

    // Discovery
    bool start_discovery(bool start, bool sending) override;

    // Binding
    int ping_bind(std::string ping_code) override;
    int bind_detect(std::string dev_ip, std::string sec_link, detectResult& detect) override;
    int bind(std::string      dev_ip,
             std::string      dev_id,
             std::string      dev_model,
             std::string      sec_link,
             std::string      timezone,
             bool             improved,
             OnUpdateStatusFn update_fn) override;
    int unbind(std::string dev_id) override;
    int request_bind_ticket(std::string* ticket) override;
    int get_hms_snapshot(std::string dev_id, std::string file_name, std::function<void(std::string, int)> callback) override;
    int set_server_callback(OnServerErrFn fn) override;

    // Machine selection
    std::string get_user_selected_machine() override;
    int         set_user_selected_machine(std::string dev_id) override;

    static AgentInfo get_agent_info_static();
    AgentInfo        get_agent_info() override { return get_agent_info_static(); }

    // Print job operations
    int start_print(PrintParams params, OnUpdateStatusFn update_fn, WasCancelledFn cancel_fn, OnWaitFn wait_fn) override;
    int start_local_print_with_record(PrintParams params, OnUpdateStatusFn update_fn, WasCancelledFn cancel_fn, OnWaitFn wait_fn) override;
    int start_send_gcode_to_sdcard(PrintParams params, OnUpdateStatusFn update_fn, WasCancelledFn cancel_fn, OnWaitFn wait_fn) override;
    int start_local_print(PrintParams params, OnUpdateStatusFn update_fn, WasCancelledFn cancel_fn) override;
    int start_sdcard_print(PrintParams params, OnUpdateStatusFn update_fn, WasCancelledFn cancel_fn) override;

    // Callbacks
    int set_on_ssdp_msg_fn(OnMsgArrivedFn fn) override;
    int set_on_printer_connected_fn(OnPrinterConnectedFn fn) override;
    int set_on_subscribe_failure_fn(GetSubscribeFailureFn fn) override;
    int set_on_message_fn(OnMessageFn fn) override;
    int set_on_user_message_fn(OnMessageFn fn) override;
    int set_on_local_connect_fn(OnLocalConnectedFn fn) override;
    int set_on_local_message_fn(OnMessageFn fn) override;
    int set_queue_on_main_fn(QueueOnMainFn fn) override;

    // Reports arrive as MQTT pushes, so nothing has to be fetched.
    FilamentSyncMode get_filament_sync_mode() const override { return FilamentSyncMode::subscription; }

    // The only way to lift the print gate. Nothing in this build calls it; it exists so the
    // gate is a single, greppable switch rather than a condition scattered over three methods.
    void set_allow_print(bool allow);
    bool allow_print() const { return m_allow_print.load(); }

private:
    // Uploads params.filename to the SD card and returns the name it landed under.
    int  upload_job(const PrintParams& params, OnUpdateStatusFn update_fn, WasCancelledFn cancel_fn, std::string& sd_name, int stage);
    // The `project_file` command for a job already on the SD card, per ADR-002.
    std::string build_project_file_command(const PrintParams& params, const std::string& sd_name) const;
    // Publishes a command, refusing `project_file` unless the gate is open.
    int  publish_command(const std::string& dev_id, const std::string& json_str);
    void dispatch_local_connect(int status, const std::string& dev_id, const std::string& msg);
    void dispatch_local_message(const std::string& dev_id, const std::string& payload);
    void dispatch_printer_connected(const std::string& topic);

    std::string log_dir;
    std::string selected_machine;

    mutable std::mutex state_mutex;
    std::shared_ptr<ICloudServiceAgent> m_cloud_agent;

    OnMsgArrivedFn        on_ssdp_msg_fn;
    OnPrinterConnectedFn  on_printer_connected_fn;
    GetSubscribeFailureFn on_subscribe_failure_fn;
    OnMessageFn           on_message_fn;
    OnMessageFn           on_user_message_fn;
    OnLocalConnectedFn    on_local_connect_fn;
    OnMessageFn           on_local_message_fn;
    QueueOnMainFn         queue_on_main_fn;
    OnServerErrFn         on_server_err_fn;

    mutable std::mutex m_connect_mutex;
    std::string        m_dev_id;   // the printer serial; LAN Developer Mode has no other id
    std::string        m_dev_ip;
    std::string        m_username;
    std::string        m_password; // access code; never logged
    // Bumped on every connect_printer(), so callbacks from a session the user already replaced
    // are dropped instead of reporting on the wrong printer.
    std::atomic<uint64_t> m_connect_generation{0};

    Nocte::NocteMqttClient m_mqtt;
    std::atomic<bool>      m_allow_print{false};
};

} // namespace Slic3r

#endif // slic3r_Utils_Nocte_NocteLanPrinterAgent_hpp_
