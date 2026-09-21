// NØCTE Slicer — Copyright (c) 2026 NØCTE Engineering. AGPL-3.0-or-later.

#include "NocteLanProbe.hpp"
#include "NocteFtps.hpp"
#include "NocteMqttClient.hpp"

#include "libslic3r/miniz_extension.hpp"

#include <boost/nowide/cstdlib.hpp> // boost::nowide::getenv
#include <boost/nowide/fstream.hpp>
#include <boost/nowide/iostream.hpp>

#include <nlohmann/json.hpp>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstring>
#include <mutex>
#include <regex>
#include <string>
#include <thread>
#include <vector>

namespace Slic3r {
namespace Nocte {

namespace {

const char* const PROBE_REPORT_FILE = "nocte_lan_probe.txt";
const char* const PROBE_SD_NAME     = "nocte_lan_probe.gcode.3mf";

constexpr int PROBE_LISTEN_SECONDS = 20;
constexpr int PROBE_CONNACK_WAIT_S = 15;

struct Credentials
{
    std::string ip;
    std::string serial;
    std::string access_code;

    bool complete() const { return !ip.empty() && !serial.empty() && !access_code.empty(); }
};

std::string env_or_empty(const char* name)
{
    const char* value = boost::nowide::getenv(name);
    return value != nullptr ? std::string(value) : std::string();
}

// The same two-stage scrub tools/nocte/lan/credentials.py applies: the exact values we know
// first (longest first, so a short one cannot shadow a longer match), then the generic shapes
// that arrive from the printer itself — reports name serials, addresses and MACs of their own
// accord, and a 227 reply spells the address as h1,h2,h3,h4,p1,p2.
class Redactor
{
public:
    explicit Redactor(const Credentials& credentials) : m_credentials(credentials)
    {
        m_pasv_form = credentials.ip;
        std::replace(m_pasv_form.begin(), m_pasv_form.end(), '.', ',');
    }

    std::string operator()(const std::string& text) const
    {
        std::string out = text;
        // Longest first.
        std::vector<std::pair<std::string, std::string>> exact;
        if (!m_credentials.access_code.empty())
            exact.emplace_back(m_credentials.access_code, "<access-code>");
        if (!m_credentials.serial.empty())
            exact.emplace_back(m_credentials.serial, "<serial>");
        if (!m_credentials.ip.empty())
            exact.emplace_back(m_credentials.ip, "<ip>");
        std::sort(exact.begin(), exact.end(),
                  [](const std::pair<std::string, std::string>& a, const std::pair<std::string, std::string>& b) {
                      return a.first.size() > b.first.size();
                  });
        for (const auto& pair : exact)
            replace_all(out, pair.first, pair.second);
        if (m_pasv_form.size() > 3)
            replace_all(out, m_pasv_form, "<pasv-host>");

        static const std::regex mac_re(R"(\b(?:[0-9A-Fa-f]{2}[:-]){5}[0-9A-Fa-f]{2}\b)");
        static const std::regex pasv_re(R"(\b(?:\d{1,3},){3}\d{1,3}(?:,\d{1,3},\d{1,3})?\b)");
        static const std::regex ipv4_re(R"(\b(?:\d{1,3}\.){3}\d{1,3}\b)");
        // Bambu firmware versions are dotted quads too ("01.08.01.00"). They are exempted only in
        // their canonical form - four two-digit groups with a leading zero - which no canonically
        // written address has.
        static const std::regex firmware_re(R"(^(?=.*\b0\d\b)\d{2}\.\d{2}\.\d{2}\.\d{2}$)");
        static const std::regex serial_re(R"(\b(?=[0-9A-Z]*[0-9])(?=[0-9A-Z]*[A-Z])[0-9A-Z]{12,20}\b)");
        static const std::regex code_re(R"(\b\d{8}\b)");

        out = std::regex_replace(out, mac_re, "<mac>");
        out = std::regex_replace(out, pasv_re, "<pasv-addr>");
        out = mask_addresses(out, ipv4_re, firmware_re);
        out = std::regex_replace(out, serial_re, "<serial>");
        out = std::regex_replace(out, code_re, "<access-code>");
        return out;
    }

private:
    static void replace_all(std::string& text, const std::string& from, const std::string& to)
    {
        if (from.empty())
            return;
        size_t position = text.find(from);
        while (position != std::string::npos) {
            text.replace(position, from.size(), to);
            position = text.find(from, position + to.size());
        }
    }

    static std::string mask_addresses(const std::string& text, const std::regex& ipv4, const std::regex& firmware)
    {
        std::string out;
        auto        begin = std::sregex_iterator(text.begin(), text.end(), ipv4);
        auto        end   = std::sregex_iterator();
        size_t      last  = 0;
        for (auto it = begin; it != end; ++it) {
            const std::smatch& match   = *it;
            const std::string  matched = match.str();
            out.append(text, last, static_cast<size_t>(match.position()) - last);
            out.append(std::regex_match(matched, firmware) ? matched : std::string("x.x.x.x"));
            last = static_cast<size_t>(match.position()) + matched.size();
        }
        out.append(text, last, std::string::npos);
        return out;
    }

    Credentials m_credentials;
    std::string m_pasv_form;
};

// Writes to stdout and, always, to a file next to the working directory: nocte-slicer.exe is a
// GUI-subsystem binary and its stdout may go nowhere at all.
class Checklist
{
public:
    Checklist(const Credentials& credentials, const std::string& path) : m_redact(credentials), m_file(path.c_str())
    {}

    void say(const std::string& line)
    {
        const std::string safe = m_redact(line);
        boost::nowide::cout << safe << std::endl;
        if (m_file.is_open())
            m_file << safe << "\n";
    }

    void check(const std::string& label, bool passed, const std::string& detail = std::string())
    {
        if (!passed)
            m_failures += 1;
        say(std::string(passed ? "  PASS  " : "  FAIL  ") + label + (detail.empty() ? std::string() : " - " + detail));
    }

    int failures() const { return m_failures; }

    void flush()
    {
        boost::nowide::cout.flush();
        if (m_file.is_open())
            m_file.flush();
    }

private:
    Redactor              m_redact;
    boost::nowide::ofstream m_file;
    int                   m_failures = 0;
};

// A minimal but structurally valid .gcode.3mf: one Metadata/plate_1.gcode entry, built in memory
// so the probe never touches the user's disk. It is uploaded and deleted again; it is never
// named in a print command, and the plate it declares is not printable.
bool build_probe_archive(std::string& out)
{
    out.clear();
    mz_zip_archive archive;
    std::memset(&archive, 0, sizeof(archive));
    if (mz_zip_writer_init_heap(&archive, 0, 64 * 1024) == MZ_FALSE)
        return false;

    const std::string gcode = "; NOCTE LAN probe artefact - not a printable job\n; generated in memory\nM400\n";
    bool              ok    = mz_zip_writer_add_mem(&archive, "Metadata/plate_1.gcode", gcode.data(), gcode.size(),
                                                    static_cast<mz_uint>(MZ_DEFAULT_LEVEL)) != MZ_FALSE;

    void*  buffer = nullptr;
    size_t size   = 0;
    ok            = ok && mz_zip_writer_finalize_heap_archive(&archive, &buffer, &size) != MZ_FALSE;
    if (ok && buffer != nullptr && size > 0)
        out.assign(static_cast<const char*>(buffer), size);
    // finalize_heap_archive() hands the buffer over and clears m_pMem, so mz_zip_writer_end()
    // does not free it: the caller owns it from here (miniz.c:7621-7624, and the same
    // mz_free() the thumbnail code uses).
    if (buffer != nullptr)
        mz_free(buffer);
    mz_zip_writer_end(&archive);
    return ok && !out.empty();
}

struct CapturedReport
{
    double      at_seconds = 0.0;
    std::string payload;
};

std::string join_keys(const nlohmann::json& object, size_t limit)
{
    std::string out;
    size_t      count = 0;
    for (auto it = object.begin(); it != object.end(); ++it) {
        if (count >= limit) {
            out += ", …";
            break;
        }
        if (count > 0)
            out += ", ";
        out += it.key();
        ++count;
    }
    return out;
}

} // namespace

int run_lan_probe()
{
    Credentials credentials;
    credentials.ip          = env_or_empty("NOCTE_LAN_IP");
    credentials.serial      = env_or_empty("NOCTE_LAN_SERIAL");
    credentials.access_code = env_or_empty("NOCTE_LAN_ACCESS_CODE");

    Checklist checklist(credentials, PROBE_REPORT_FILE);
    checklist.say("NOCTE LAN Developer Mode probe");
    checklist.say("  printer " + redact_host(credentials.ip) + " serial " + redact_serial(credentials.serial) +
                  " (values come from NOCTE_LAN_* and are never printed)");
    checklist.say("  no print command is sent by this probe, ever");
    checklist.say("");

    checklist.check("credentials present (NOCTE_LAN_IP / _SERIAL / _ACCESS_CODE)", credentials.complete());
    if (!credentials.complete()) {
        checklist.say("");
        checklist.say("RESULT: FAIL - set all three environment variables and run again.");
        checklist.flush();
        return 1;
    }

    // --- 1. transport ---------------------------------------------------------------------
    checklist.say("1. transport");
    for (const uint16_t port : {static_cast<uint16_t>(8883), static_cast<uint16_t>(990)}) {
        std::string description;
        std::string error;
        const auto  started = std::chrono::steady_clock::now();
        const bool  ok      = NocteMqttClient::probe_tls(credentials.ip, port, 10, description, error);
        const auto  elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - started);
        checklist.check("TCP + TLS on port " + std::to_string(port), ok,
                        ok ? description + ", " + std::to_string(elapsed.count()) + " ms" : error);
    }

    // --- 2. MQTT --------------------------------------------------------------------------
    checklist.say("");
    checklist.say("2. MQTT (8883, protocol level 4, clean session, keepalive 60)");

    std::mutex                  capture_mutex;
    std::vector<CapturedReport> reports;
    std::atomic<int>            connack{-2};
    std::atomic<int>            granted_qos{-1};
    std::string                 disconnect_reason;
    const auto                  origin = std::chrono::steady_clock::now();

    NocteMqttClient client;
    client.set_on_connected([&connack](int rc) { connack.store(rc); });
    client.set_on_subscribed([&granted_qos](int granted) { granted_qos.store(granted); });
    client.set_on_message([&](const std::string& topic, const std::string& payload) {
        (void) topic;
        const double at = std::chrono::duration<double>(std::chrono::steady_clock::now() - origin).count();
        std::lock_guard<std::mutex> lock(capture_mutex);
        reports.push_back(CapturedReport{at, payload});
    });
    client.set_on_disconnected([&](const std::string& reason) {
        std::lock_guard<std::mutex> lock(capture_mutex);
        if (disconnect_reason.empty())
            disconnect_reason = reason;
    });

    MqttConfig config;
    config.host     = credentials.ip;
    config.password = credentials.access_code;
    if (!client.start(config)) {
        checklist.check("MQTT session started", false, "could not start the client thread");
        checklist.say("");
        checklist.say("RESULT: FAIL");
        checklist.flush();
        return 1;
    }

    const auto connack_deadline = std::chrono::steady_clock::now() + std::chrono::seconds(PROBE_CONNACK_WAIT_S);
    while (connack.load() == -2 && std::chrono::steady_clock::now() < connack_deadline)
        std::this_thread::sleep_for(std::chrono::milliseconds(50));

    const int connack_rc = connack.load();
    checklist.check("CONNACK received", connack_rc != -2, connack_rc == -2 ? std::string("no answer in 15 s") : std::string());
    checklist.check("CONNACK rc = 0", connack_rc == 0,
                    (connack_rc == 4 || connack_rc == 5) ?
                        "rc " + std::to_string(connack_rc) + ": the access code is wrong" :
                        (connack_rc >= 0 ? "rc " + std::to_string(connack_rc) : std::string()));
    checklist.check("TLS 1.2 negotiated", client.tls_description().find("TLSv1.2") != std::string::npos,
                    client.tls_description());

    if (connack_rc == 0) {
        client.subscribe(NocteMqttClient::report_topic(credentials.serial));
        const auto suback_deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
        while (granted_qos.load() < 0 && std::chrono::steady_clock::now() < suback_deadline)
            std::this_thread::sleep_for(std::chrono::milliseconds(20));
        checklist.check("SUBACK for device/<serial>/report granted QoS 0", granted_qos.load() == 0,
                        granted_qos.load() < 0 ? std::string("no SUBACK") :
                                                 "granted " + std::to_string(granted_qos.load()));

        client.publish_pushall(NocteMqttClient::request_topic(credentials.serial), 0);
        client.publish(NocteMqttClient::request_topic(credentials.serial),
                       R"({"info":{"sequence_id":"2","command":"get_version"}})");

        std::this_thread::sleep_for(std::chrono::seconds(PROBE_LISTEN_SECONDS));
    }

    client.stop();

    std::vector<CapturedReport> captured;
    std::string                 reason;
    {
        std::lock_guard<std::mutex> lock(capture_mutex);
        captured = reports;
        reason   = disconnect_reason;
    }

    if (connack_rc == 0) {
        checklist.check("at least one report arrived", !captured.empty(),
                        captured.empty() ? (reason.empty() ? std::string("nothing on the report topic") : reason) :
                                           std::string());

        bool   saw_print   = false;
        size_t max_payload = 0;
        for (const CapturedReport& report : captured) {
            max_payload = (std::max)(max_payload, report.payload.size());
            const nlohmann::json parsed = nlohmann::json::parse(report.payload, nullptr, /*allow_exceptions=*/false);
            if (parsed.is_discarded() || !parsed.is_object())
                continue;
            const auto print_it = parsed.find("print");
            if (!saw_print && print_it != parsed.end() && print_it->is_object()) {
                saw_print = true;
                checklist.check("first `print` report parsed", true,
                                std::to_string(report.payload.size()) + " B, top-level {" + join_keys(parsed, 8) + "}, " +
                                    std::to_string(print_it->size()) + " keys under `print`");
            }
            const auto info_it = parsed.find("info");
            if (info_it != parsed.end() && info_it->is_object()) {
                const auto module_it = info_it->find("module");
                if (module_it != info_it->end() && module_it->is_array())
                    checklist.check("get_version answered", true, std::to_string(module_it->size()) + " modules");
            }
        }
        if (!saw_print)
            checklist.check("first `print` report parsed", false, "no report carried a `print` object");

        // The remaining-length field is multi-byte in practice: the pushall answer alone was
        // 3362 B on an idle A1, and a one-byte decoder desynchronises on it.
        checklist.check("multi-byte remaining length decoded", max_payload > 127,
                        "largest payload " + std::to_string(max_payload) + " B");
        const double rate = captured.empty() ? 0.0
                                             : static_cast<double>(captured.size()) / static_cast<double>(PROBE_LISTEN_SECONDS);
        checklist.say("  note   " + std::to_string(captured.size()) + " messages in " + std::to_string(PROBE_LISTEN_SECONDS) +
                      " s (" + std::to_string(rate) + " msg/s)");
    }

    // --- 3. FTPS --------------------------------------------------------------------------
    checklist.say("");
    checklist.say("3. FTPS (990, implicit TLS, PROT P)");

    FtpsConfig ftps_config;
    ftps_config.host     = credentials.ip;
    ftps_config.password = credentials.access_code;
    NocteFtps ftps(ftps_config);

    std::vector<std::string> names;
    std::string              error;
    const bool               listed = ftps.list(names, error);
    checklist.check("NLST at the SD-card root", listed, listed ? std::to_string(names.size()) + " entries" : error);

    if (listed) {
        std::string archive;
        const bool  built = build_probe_archive(archive);
        checklist.check("probe .gcode.3mf built in memory", built,
                        built ? std::to_string(archive.size()) + " B" :
                                std::string("miniz refused to build the archive"));

        if (built) {
            const bool uploaded = ftps.upload_memory(archive, PROBE_SD_NAME, error);
            checklist.check("STOR the probe file and confirm it by NLST", uploaded, uploaded ? std::string() : error);

            if (uploaded) {
                const bool removed = ftps.remove(PROBE_SD_NAME, error);
                checklist.check("DELE the probe file and confirm it is gone", removed, removed ? std::string() : error);
                if (!removed)
                    checklist.say("  note   " + std::string(PROBE_SD_NAME) + " may still be on the SD card; delete it by hand");
            }
        }
    }

    checklist.say("");
    checklist.say(checklist.failures() == 0 ? std::string("RESULT: PASS") :
                                              "RESULT: FAIL (" + std::to_string(checklist.failures()) + ")");
    checklist.say("checklist also written to ./" + std::string(PROBE_REPORT_FILE));
    checklist.flush();
    return checklist.failures() == 0 ? 0 : 1;
}

} // namespace Nocte
} // namespace Slic3r
