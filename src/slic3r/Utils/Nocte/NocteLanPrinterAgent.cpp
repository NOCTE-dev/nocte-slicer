// NØCTE Slicer — Copyright (c) 2026 NØCTE Engineering. AGPL-3.0-or-later.

#include "NocteLanPrinterAgent.hpp"
#include "NocteFtps.hpp"

#include "slic3r/GUI/Nocte/NocteUi.hpp" // Slic3r::Nocte::LAN_AGENT_ID

#include <boost/filesystem.hpp>
#include <boost/log/trivial.hpp>

#include <nlohmann/json.hpp>

#include <utility>

namespace Slic3r {

namespace {

const std::string NocteLanPrinterAgent_VERSION = "1.0.0";

// Reports are partial deltas, so the full state has to be asked for again periodically. 300 s is
// a policy choice (ADR-002), not a protocol requirement.
constexpr int NOCTE_PUSHALL_PERIOD_S = 300;

// The printer refuses anything larger, and so does every other agent in this tree.
constexpr std::uintmax_t NOCTE_MAX_JOB_BYTES = 1024ull * 1024ull * 1024ull;

uint64_t next_sequence_id()
{
    static std::atomic<uint64_t> counter{1000};
    return counter.fetch_add(1);
}

// True when this JSON is a request to start printing. A payload we cannot parse but that mentions
// the command is treated as one too: the gate errs towards refusing.
bool is_project_file_command(const std::string& json_str)
{
    const nlohmann::json parsed = nlohmann::json::parse(json_str, nullptr, /*allow_exceptions=*/false);
    if (parsed.is_discarded())
        return json_str.find("project_file") != std::string::npos;
    if (!parsed.is_object())
        return false;
    const auto print_it = parsed.find("print");
    if (print_it == parsed.end() || !print_it->is_object())
        return false;
    const auto command_it = print_it->find("command");
    return command_it != print_it->end() && command_it->is_string() && command_it->get<std::string>() == "project_file";
}

// params.ams_mapping is the `[tray, tray, …]` array SelectMachineDialog builds, one entry per
// filament of the plate, already validated against the machine's own AMS report. It is forwarded
// as it stands; only a payload that is not an array of integers at all is dropped, because
// sending a malformed mapping is what produces a filament mismatch at the printer.
bool parse_ams_mapping(const std::string& text, nlohmann::json& out)
{
    if (text.empty())
        return false;
    const nlohmann::json parsed = nlohmann::json::parse(text, nullptr, /*allow_exceptions=*/false);
    if (parsed.is_discarded() || !parsed.is_array() || parsed.empty())
        return false;
    nlohmann::json mapping = nlohmann::json::array();
    for (const auto& entry : parsed) {
        if (!entry.is_number_integer())
            return false;
        mapping.push_back(entry.get<int>());
    }
    out = mapping;
    return true;
}

} // namespace

NocteLanPrinterAgent::NocteLanPrinterAgent(std::string log_dir) : log_dir(std::move(log_dir)) {}

NocteLanPrinterAgent::~NocteLanPrinterAgent()
{
    m_connect_generation.fetch_add(1);
    m_mqtt.stop();
}

AgentInfo NocteLanPrinterAgent::get_agent_info_static()
{
    return AgentInfo{Nocte::LAN_AGENT_ID, "NØCTE LAN (Developer Mode)", NocteLanPrinterAgent_VERSION,
                     "Bambu Lab LAN Developer Mode over MQTT and FTPS, with no proprietary components"};
}

void NocteLanPrinterAgent::set_cloud_agent(std::shared_ptr<ICloudServiceAgent> cloud)
{
    std::lock_guard<std::mutex> lock(state_mutex);
    m_cloud_agent = std::move(cloud);
}

void NocteLanPrinterAgent::set_allow_print(bool allow)
{
    m_allow_print.store(allow);
    BOOST_LOG_TRIVIAL(warning) << "NocteLan: the print gate is now " << (allow ? "OPEN" : "closed");
}

// ============================================================================
// Communication
// ============================================================================

int NocteLanPrinterAgent::connect_printer(std::string dev_id, std::string dev_ip, std::string username, std::string password, bool use_ssl)
{
    // LAN Developer Mode is TLS-only on both ports, so there is nothing to switch off here.
    (void) use_ssl;
    if (dev_id.empty() || dev_ip.empty()) {
        BOOST_LOG_TRIVIAL(error) << "NocteLan: connect_printer needs both a serial and an address";
        return BAMBU_NETWORK_ERR_INVALID_HANDLE;
    }

    // Tear the previous session down before the generation moves, so that a callback still in
    // flight is dropped against the old generation rather than accepted against the new one.
    m_mqtt.stop();

    Nocte::MqttConfig config;
    const uint64_t    generation = m_connect_generation.fetch_add(1) + 1;
    {
        std::lock_guard<std::mutex> lock(m_connect_mutex);
        m_dev_id   = dev_id;
        m_dev_ip   = dev_ip;
        m_username = username.empty() ? std::string("bblp") : username;
        m_password = password;
        config.host     = m_dev_ip;
        config.username = m_username;
        config.password = m_password;
    }

    const std::string serial        = dev_id;
    const std::string report_topic  = Nocte::NocteMqttClient::report_topic(serial);
    const std::string request_topic = Nocte::NocteMqttClient::request_topic(serial);

    m_mqtt.set_on_connected([this, generation, serial, report_topic, request_topic](int rc) {
        if (generation != m_connect_generation.load())
            return;
        if (rc != Nocte::MqttConnackAccepted) {
            const std::string message = (rc == Nocte::MqttConnackBadCredentials || rc == Nocte::MqttConnackNotAuthorized) ?
                                            std::string("the printer rejected the access code") :
                                            "the printer refused the connection (CONNACK rc=" + std::to_string(rc) + ")";
            dispatch_local_connect(ConnectStatusFailed, serial, message);
            return;
        }
        // Exactly this topic: every wildcard shape is a hard disconnect in about 20 ms, which
        // would otherwise look like a credential failure.
        m_mqtt.subscribe(report_topic);
        m_mqtt.publish_pushall(request_topic, NOCTE_PUSHALL_PERIOD_S);
        dispatch_local_connect(ConnectStatusOk, serial, std::string());
        dispatch_printer_connected(report_topic);
    });

    m_mqtt.set_on_subscribed([this, generation, report_topic](int granted_qos) {
        if (generation != m_connect_generation.load() || granted_qos != 0x80)
            return;
        GetSubscribeFailureFn failure_fn;
        {
            std::lock_guard<std::mutex> lock(state_mutex);
            failure_fn = on_subscribe_failure_fn;
        }
        if (failure_fn)
            failure_fn(report_topic);
    });

    m_mqtt.set_on_message([this, generation, serial](const std::string& topic, const std::string& payload) {
        (void) topic; // only one subscription exists
        if (generation != m_connect_generation.load())
            return;
        dispatch_local_message(serial, payload);
    });

    m_mqtt.set_on_disconnected([this, generation, serial](const std::string& reason) {
        if (generation != m_connect_generation.load())
            return;
        dispatch_local_connect(ConnectStatusLost, serial, reason);
    });

    if (!m_mqtt.start(config)) {
        dispatch_local_connect(ConnectStatusFailed, serial, "could not start the LAN session");
        return BAMBU_NETWORK_ERR_CONNECTION_TO_PRINTER_FAILED;
    }

    BOOST_LOG_TRIVIAL(info) << "NocteLan: connecting to " << Nocte::redact_host(dev_ip) << " serial "
                            << Nocte::redact_serial(dev_id);
    return BAMBU_NETWORK_SUCCESS;
}

int NocteLanPrinterAgent::disconnect_printer()
{
    m_connect_generation.fetch_add(1);
    m_mqtt.stop();
    return BAMBU_NETWORK_SUCCESS;
}

int NocteLanPrinterAgent::publish_command(const std::string& dev_id, const std::string& json_str)
{
    std::string serial;
    {
        std::lock_guard<std::mutex> lock(m_connect_mutex);
        serial = m_dev_id.empty() ? dev_id : m_dev_id;
    }
    if (serial.empty())
        return BAMBU_NETWORK_ERR_INVALID_HANDLE;

    if (!m_allow_print.load() && is_project_file_command(json_str)) {
        // The single place a print can start. See the header: the gate defaults closed and only
        // set_allow_print() opens it.
        BOOST_LOG_TRIVIAL(warning) << "NocteLan: refused a project_file command for "
                                   << Nocte::redact_serial(serial) << "; printing is disabled in this build";
        return BAMBU_NETWORK_ERR_PRINT_LP_PUBLISH_MSG_FAILED;
    }

    if (!m_mqtt.is_connected())
        return BAMBU_NETWORK_ERR_CONNECTION_TO_PRINTER_FAILED;
    if (!m_mqtt.publish(Nocte::NocteMqttClient::request_topic(serial), json_str))
        return BAMBU_NETWORK_ERR_SEND_MSG_FAILED;
    return BAMBU_NETWORK_SUCCESS;
}

int NocteLanPrinterAgent::send_message_to_printer(std::string dev_id, std::string json_str, int qos, int flag)
{
    // QoS 0 always: a QoS 1 publish is delivered but never acknowledged, so an in-flight window
    // of one would stall the client permanently. There is no cloud signing, so the flag is moot.
    (void) qos;
    (void) flag;
    return publish_command(dev_id, json_str);
}

int NocteLanPrinterAgent::send_message(std::string dev_id, std::string json_str, int qos, int flag)
{
    // NØCTE has no cloud relay (ADR-002); the LAN link is the only route a command can take.
    return send_message_to_printer(std::move(dev_id), std::move(json_str), qos, flag);
}

// ============================================================================
// Certificates, discovery and binding - none of which LAN Developer Mode has
// ============================================================================

int NocteLanPrinterAgent::check_cert() { return BAMBU_NETWORK_SUCCESS; }

void NocteLanPrinterAgent::install_device_cert(std::string dev_id, bool lan_only)
{
    // The printer's certificate is never verified (ADR-002), so there is nothing to install.
    (void) dev_id;
    (void) lan_only;
}

bool NocteLanPrinterAgent::start_discovery(bool start, bool sending)
{
    // SSDP discovery is not implemented yet: it occupies UDP 2021 and therefore conflicts with a
    // running Bambu Studio. Printers are entered by address until that lands.
    (void) start;
    (void) sending;
    return true;
}

int NocteLanPrinterAgent::ping_bind(std::string ping_code)
{
    (void) ping_code;
    return BAMBU_NETWORK_SUCCESS;
}

int NocteLanPrinterAgent::bind_detect(std::string dev_ip, std::string sec_link, detectResult& detect)
{
    // Binding is a cloud-account concept; a LAN Developer Mode printer is never bound.
    (void) dev_ip;
    (void) sec_link;
    (void) detect;
    return BAMBU_NETWORK_SUCCESS;
}

int NocteLanPrinterAgent::bind(std::string      dev_ip,
                               std::string      dev_id,
                               std::string      dev_model,
                               std::string      sec_link,
                               std::string      timezone,
                               bool             improved,
                               OnUpdateStatusFn update_fn)
{
    (void) dev_ip;
    (void) dev_id;
    (void) dev_model;
    (void) sec_link;
    (void) timezone;
    (void) improved;
    (void) update_fn;
    return BAMBU_NETWORK_SUCCESS;
}

int NocteLanPrinterAgent::unbind(std::string dev_id)
{
    (void) dev_id;
    return BAMBU_NETWORK_SUCCESS;
}

int NocteLanPrinterAgent::request_bind_ticket(std::string* ticket)
{
    if (ticket != nullptr)
        ticket->clear();
    return BAMBU_NETWORK_SUCCESS;
}

int NocteLanPrinterAgent::get_hms_snapshot(std::string dev_id, std::string file_name, std::function<void(std::string, int)> callback)
{
    // The snapshot lives in Bambu's cloud, which NØCTE never talks to; report failure so the
    // caller falls back to its own error dialog.
    (void) dev_id;
    (void) file_name;
    (void) callback;
    return -1;
}

int NocteLanPrinterAgent::set_server_callback(OnServerErrFn fn)
{
    std::lock_guard<std::mutex> lock(state_mutex);
    on_server_err_fn = std::move(fn);
    return BAMBU_NETWORK_SUCCESS;
}

// ============================================================================
// Machine selection
// ============================================================================

std::string NocteLanPrinterAgent::get_user_selected_machine()
{
    std::lock_guard<std::mutex> lock(state_mutex);
    return selected_machine;
}

int NocteLanPrinterAgent::set_user_selected_machine(std::string dev_id)
{
    std::lock_guard<std::mutex> lock(state_mutex);
    selected_machine = std::move(dev_id);
    return BAMBU_NETWORK_SUCCESS;
}

// ============================================================================
// Print job operations
// ============================================================================

int NocteLanPrinterAgent::upload_job(const PrintParams& params,
                                     OnUpdateStatusFn   update_fn,
                                     WasCancelledFn     cancel_fn,
                                     std::string&       sd_name,
                                     int                stage)
{
    std::string host;
    std::string password;
    std::string username;
    {
        std::lock_guard<std::mutex> lock(m_connect_mutex);
        host     = m_dev_ip.empty() ? params.dev_ip : m_dev_ip;
        password = m_password.empty() ? params.password : m_password;
        username = m_username.empty() ? std::string("bblp") : m_username;
    }
    if (host.empty())
        return BAMBU_NETWORK_ERR_INVALID_HANDLE;

    namespace fs = boost::filesystem;
    const fs::path source(params.filename);
    boost::system::error_code exists_ec;
    if (params.filename.empty() || !fs::exists(source, exists_ec))
        return BAMBU_NETWORK_ERR_FILE_NOT_EXIST;

    boost::system::error_code size_ec;
    const uintmax_t           size = fs::file_size(source, size_ec);
    if (size_ec)
        return BAMBU_NETWORK_ERR_OPEN_FILE_FAILED;
    if (size > NOCTE_MAX_JOB_BYTES)
        return BAMBU_NETWORK_ERR_PRINT_LP_FILE_OVER_SIZE;

    sd_name = Nocte::NocteFtps::sanitize_remote_name(params.filename);
    if (!Nocte::NocteFtps::is_safe_remote_name(sd_name)) {
        BOOST_LOG_TRIVIAL(error) << "NocteLan: '" << sd_name << "' is not a name the printer will accept";
        return BAMBU_NETWORK_ERR_PRINT_LP_UPLOAD_FTP_FAILED;
    }

    Nocte::FtpsConfig ftps_config;
    ftps_config.host     = host;
    ftps_config.username = username;
    ftps_config.password = password;
    Nocte::NocteFtps ftps(ftps_config);

    if (update_fn)
        update_fn(stage, 0, "Uploading the job file...");

    std::string error;
    const bool  uploaded = ftps.upload_file(
        params.filename, sd_name, error,
        [update_fn, stage](double fraction) {
            if (update_fn)
                update_fn(stage, static_cast<int>(fraction * 100.0), "Uploading the job file...");
        },
        [cancel_fn]() { return cancel_fn ? cancel_fn() : false; });

    if (!uploaded) {
        if (cancel_fn && cancel_fn())
            return BAMBU_NETWORK_ERR_CANCELED;
        BOOST_LOG_TRIVIAL(error) << "NocteLan: upload to " << Nocte::redact_host(host) << " failed: " << error;
        return BAMBU_NETWORK_ERR_PRINT_LP_UPLOAD_FTP_FAILED;
    }
    return BAMBU_NETWORK_SUCCESS;
}

std::string NocteLanPrinterAgent::build_project_file_command(const PrintParams& params, const std::string& sd_name) const
{
    // Field for field, ADR-002 and tools/nocte/lan/print_job.py. A `param` naming a plate that is
    // not in the file is accepted by the printer and prints nothing, so the index is clamped.
    const int plate = params.plate_index > 0 ? params.plate_index : 1;

    std::string subtask_name = params.project_name;
    if (subtask_name.empty())
        subtask_name = params.task_name;
    if (subtask_name.empty())
        subtask_name = sd_name;

    nlohmann::json command;
    command["sequence_id"]   = std::to_string(next_sequence_id());
    command["command"]       = "project_file";
    command["param"]         = "Metadata/plate_" + std::to_string(plate) + ".gcode";
    command["url"]           = "file:///sdcard/" + sd_name;
    command["subtask_name"]  = subtask_name;
    command["use_ams"]       = params.task_use_ams;
    command["bed_leveling"]  = params.task_bed_leveling;
    command["timelapse"]     = params.task_record_timelapse;
    command["flow_cali"]     = params.task_flow_cali;
    command["vibration_cali"] = params.task_vibration_cali;
    command["layer_inspect"] = params.task_layer_inspect;
    command["bed_type"]      = "auto";
    // The job does not come from any cloud, so all four identifiers are the string "0".
    command["profile_id"] = "0";
    command["project_id"] = "0";
    command["subtask_id"] = "0";
    command["task_id"]    = "0";

    if (params.task_use_ams) {
        nlohmann::json mapping;
        if (parse_ams_mapping(params.ams_mapping, mapping))
            command["ams_mapping"] = mapping;
        else
            BOOST_LOG_TRIVIAL(warning) << "NocteLan: no usable ams_mapping for this job; sending none";
    }

    nlohmann::json root;
    root["print"] = command;
    return root.dump();
}

int NocteLanPrinterAgent::start_local_print(PrintParams params, OnUpdateStatusFn update_fn, WasCancelledFn cancel_fn)
{
    if (update_fn)
        update_fn(PrintingStageCreate, 0, "Preparing the job...");
    if (cancel_fn && cancel_fn())
        return BAMBU_NETWORK_ERR_CANCELED;

    std::string sd_name;
    const int   upload_result = upload_job(params, update_fn, cancel_fn, sd_name, PrintingStageUpload);
    if (upload_result != BAMBU_NETWORK_SUCCESS)
        return upload_result;
    if (cancel_fn && cancel_fn())
        return BAMBU_NETWORK_ERR_CANCELED;

    if (update_fn)
        update_fn(PrintingStageSending, 0, "Sending the print command...");

    const std::string command = build_project_file_command(params, sd_name);
    if (!m_allow_print.load()) {
        BOOST_LOG_TRIVIAL(warning) << "NocteLan: the job is on the SD card but the print command was not sent; "
                                      "printing is disabled in this build";
        if (update_fn)
            update_fn(PrintingStageFinished, 100, "printing is disabled in this build");
        return BAMBU_NETWORK_ERR_PRINT_LP_PUBLISH_MSG_FAILED;
    }

    const int publish_result = publish_command(params.dev_id, command);
    if (publish_result != BAMBU_NETWORK_SUCCESS)
        return BAMBU_NETWORK_ERR_PRINT_LP_PUBLISH_MSG_FAILED;

    if (update_fn)
        update_fn(PrintingStageFinished, 100, "Print started");
    return BAMBU_NETWORK_SUCCESS;
}

int NocteLanPrinterAgent::start_print(PrintParams params, OnUpdateStatusFn update_fn, WasCancelledFn cancel_fn, OnWaitFn wait_fn)
{
    // There is no cloud print path; the LAN one is the only thing this agent can do.
    (void) wait_fn;
    return start_local_print(std::move(params), std::move(update_fn), std::move(cancel_fn));
}

int NocteLanPrinterAgent::start_local_print_with_record(PrintParams      params,
                                                        OnUpdateStatusFn update_fn,
                                                        WasCancelledFn   cancel_fn,
                                                        OnWaitFn         wait_fn)
{
    // The "record" is a cloud task entry, which NØCTE never creates; the print itself is local.
    (void) wait_fn;
    return start_local_print(std::move(params), std::move(update_fn), std::move(cancel_fn));
}

int NocteLanPrinterAgent::start_send_gcode_to_sdcard(PrintParams      params,
                                                     OnUpdateStatusFn update_fn,
                                                     WasCancelledFn   cancel_fn,
                                                     OnWaitFn         wait_fn)
{
    (void) wait_fn;
    if (update_fn)
        update_fn(PrintingStageCreate, 0, "Preparing the job...");

    std::string sd_name;
    const int   upload_result = upload_job(params, update_fn, cancel_fn, sd_name, PrintingStageUpload);
    if (upload_result != BAMBU_NETWORK_SUCCESS)
        return upload_result == BAMBU_NETWORK_ERR_PRINT_LP_UPLOAD_FTP_FAILED ? BAMBU_NETWORK_ERR_PRINT_SG_UPLOAD_FTP_FAILED
                                                                             : upload_result;

    if (update_fn)
        update_fn(PrintingStageFinished, 100, "File uploaded");
    return BAMBU_NETWORK_SUCCESS;
}

int NocteLanPrinterAgent::start_sdcard_print(PrintParams params, OnUpdateStatusFn update_fn, WasCancelledFn cancel_fn)
{
    // The file is already on the card; only the command is missing.
    (void) cancel_fn;
    std::string sd_name = params.dst_file.empty() ? params.task_name : params.dst_file;
    sd_name             = Nocte::NocteFtps::sanitize_remote_name(sd_name);
    if (!Nocte::NocteFtps::is_safe_remote_name(sd_name))
        return BAMBU_NETWORK_ERR_FILE_NOT_EXIST;

    const std::string command = build_project_file_command(params, sd_name);
    if (!m_allow_print.load()) {
        BOOST_LOG_TRIVIAL(warning) << "NocteLan: refused to start an SD-card print; printing is disabled in this build";
        if (update_fn)
            update_fn(PrintingStageFinished, 100, "printing is disabled in this build");
        return BAMBU_NETWORK_ERR_PRINT_LP_PUBLISH_MSG_FAILED;
    }

    const int publish_result = publish_command(params.dev_id, command);
    if (publish_result != BAMBU_NETWORK_SUCCESS)
        return BAMBU_NETWORK_ERR_PRINT_LP_PUBLISH_MSG_FAILED;

    if (update_fn)
        update_fn(PrintingStageFinished, 100, "Print started");
    return BAMBU_NETWORK_SUCCESS;
}

// ============================================================================
// Callback registration and dispatch
// ============================================================================

int NocteLanPrinterAgent::set_on_ssdp_msg_fn(OnMsgArrivedFn fn)
{
    std::lock_guard<std::mutex> lock(state_mutex);
    on_ssdp_msg_fn = std::move(fn);
    return BAMBU_NETWORK_SUCCESS;
}

int NocteLanPrinterAgent::set_on_printer_connected_fn(OnPrinterConnectedFn fn)
{
    std::lock_guard<std::mutex> lock(state_mutex);
    on_printer_connected_fn = std::move(fn);
    return BAMBU_NETWORK_SUCCESS;
}

int NocteLanPrinterAgent::set_on_subscribe_failure_fn(GetSubscribeFailureFn fn)
{
    std::lock_guard<std::mutex> lock(state_mutex);
    on_subscribe_failure_fn = std::move(fn);
    return BAMBU_NETWORK_SUCCESS;
}

int NocteLanPrinterAgent::set_on_message_fn(OnMessageFn fn)
{
    std::lock_guard<std::mutex> lock(state_mutex);
    on_message_fn = std::move(fn);
    return BAMBU_NETWORK_SUCCESS;
}

int NocteLanPrinterAgent::set_on_user_message_fn(OnMessageFn fn)
{
    std::lock_guard<std::mutex> lock(state_mutex);
    on_user_message_fn = std::move(fn);
    return BAMBU_NETWORK_SUCCESS;
}

int NocteLanPrinterAgent::set_on_local_connect_fn(OnLocalConnectedFn fn)
{
    std::lock_guard<std::mutex> lock(state_mutex);
    on_local_connect_fn = std::move(fn);
    return BAMBU_NETWORK_SUCCESS;
}

int NocteLanPrinterAgent::set_on_local_message_fn(OnMessageFn fn)
{
    std::lock_guard<std::mutex> lock(state_mutex);
    on_local_message_fn = std::move(fn);
    return BAMBU_NETWORK_SUCCESS;
}

int NocteLanPrinterAgent::set_queue_on_main_fn(QueueOnMainFn fn)
{
    std::lock_guard<std::mutex> lock(state_mutex);
    queue_on_main_fn = std::move(fn);
    return BAMBU_NETWORK_SUCCESS;
}

void NocteLanPrinterAgent::dispatch_local_connect(int status, const std::string& dev_id, const std::string& msg)
{
    OnLocalConnectedFn local_fn;
    QueueOnMainFn      queue_fn;
    {
        std::lock_guard<std::mutex> lock(state_mutex);
        local_fn = on_local_connect_fn;
        queue_fn = queue_on_main_fn;
    }
    if (!local_fn)
        return;

    auto dispatch = [status, dev_id, msg, local_fn]() { local_fn(status, dev_id, msg); };
    if (queue_fn)
        queue_fn(dispatch);
    else
        dispatch();
}

void NocteLanPrinterAgent::dispatch_local_message(const std::string& dev_id, const std::string& payload)
{
    OnMessageFn   local_fn;
    QueueOnMainFn queue_fn;
    {
        std::lock_guard<std::mutex> lock(state_mutex);
        local_fn = on_local_message_fn;
        queue_fn = queue_on_main_fn;
    }
    if (!local_fn)
        return;

    // Verbatim: MachineObject::parse_json("lan", msg) already speaks this dialect, and every
    // translation layer is one more place for a field to go missing.
    auto dispatch = [dev_id, payload, local_fn]() { local_fn(dev_id, payload); };
    if (queue_fn)
        queue_fn(dispatch);
    else
        dispatch();
}

void NocteLanPrinterAgent::dispatch_printer_connected(const std::string& topic)
{
    OnPrinterConnectedFn connected_fn;
    QueueOnMainFn        queue_fn;
    {
        std::lock_guard<std::mutex> lock(state_mutex);
        connected_fn = on_printer_connected_fn;
        queue_fn     = queue_on_main_fn;
    }
    if (!connected_fn)
        return;

    auto dispatch = [topic, connected_fn]() { connected_fn(topic); };
    if (queue_fn)
        queue_fn(dispatch);
    else
        dispatch();
}

} // namespace Slic3r
