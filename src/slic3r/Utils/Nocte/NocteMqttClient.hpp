// NØCTE Slicer — Copyright (c) 2026 NØCTE Engineering. AGPL-3.0-or-later.
//
// A minimal MQTT 3.1.1 client, just large enough for a Bambu printer in LAN Developer Mode
// (ADR-002). It is deliberately not a general MQTT library:
//
//   * QoS 0 only on publish. The printer accepts a QoS 1 PUBLISH and answers it, but never
//     returns a PUBACK, so an in-flight window of one would stall the client forever.
//   * One subscription, to the exact topic `device/<serial>/report`. Any wildcard form gets
//     the client disconnected in ~20 ms, which is easy to misread as a credential failure.
//   * TLS 1.2, peer verification off, no SNI, no client certificate. The printer's certificate
//     is issued by a private "BBL CA" with CN = the printer serial, and we dial an IP, so
//     hostname verification can never succeed.
//
// Everything after start() runs on one private thread driving one io_context; the public
// methods are safe to call from any thread and marshal work onto it.

#ifndef slic3r_Utils_Nocte_NocteMqttClient_hpp_
#define slic3r_Utils_Nocte_NocteMqttClient_hpp_

#include <cstdint>
#include <functional>
#include <memory>
#include <string>

namespace Slic3r {
namespace Nocte {

// --- Redaction -------------------------------------------------------------------------------
//
// Shared by every NØCTE LAN transport. This is the lowest-level header of the set, so both
// transports reach the helpers without a file that exists only to hold two functions.
// There is deliberately no helper for the access code: it is never logged in any form,
// masked or otherwise.

// "192.168.1.47" -> "x.x.x.47". Anything that does not look like a dotted quad becomes "<host>".
std::string redact_host(const std::string& host);
// "01P00A123456789" -> "01P…". Empty stays "<none>".
std::string redact_serial(const std::string& serial);

// --- Client ----------------------------------------------------------------------------------

struct MqttConfig
{
    std::string host;                 // printer IP; never logged unredacted
    uint16_t    port           = 8883;
    std::string username       = "bblp";
    std::string password;             // printer access code
    uint16_t    keepalive_s    = 60;  // PINGREQ goes out every keepalive_s / 2
    int         connect_timeout_s = 15; // TCP + TLS + CONNACK; a healthy handshake costs ~0.9 s
};

// MQTT 3.1.1 CONNACK return codes (MQTT-3.2.2-3). 4 and 5 mean the access code is wrong.
enum MqttConnackCode {
    MqttConnackAccepted               = 0,
    MqttConnackUnacceptableProtocol   = 1,
    MqttConnackIdentifierRejected     = 2,
    MqttConnackServerUnavailable      = 3,
    MqttConnackBadCredentials         = 4,
    MqttConnackNotAuthorized          = 5,
    MqttConnackTransportFailure       = -1, // never reached the CONNACK: TCP/TLS/timeout
};

class NocteMqttClient
{
public:
    using OnMessageFn      = std::function<void(const std::string& topic, const std::string& payload)>;
    using OnConnectedFn    = std::function<void(int connack_rc)>;
    using OnSubscribedFn   = std::function<void(int granted_qos)>; // 0x80 = subscription refused
    using OnDisconnectedFn = std::function<void(const std::string& reason)>;

    NocteMqttClient();
    ~NocteMqttClient();

    NocteMqttClient(const NocteMqttClient&)            = delete;
    NocteMqttClient& operator=(const NocteMqttClient&) = delete;

    // Callbacks fire on the client's own thread. Set them before start().
    void set_on_message(OnMessageFn fn);
    void set_on_connected(OnConnectedFn fn);
    void set_on_subscribed(OnSubscribedFn fn);
    void set_on_disconnected(OnDisconnectedFn fn);

    // Starts the private thread and begins connecting. Returns false only if a thread could not
    // be started; a failed connection is reported through on_connected/on_disconnected.
    bool start(const MqttConfig& config);
    // Sends DISCONNECT if connected, tears the session down and joins the thread. Idempotent.
    void stop();

    bool is_connected() const;
    int  last_connack_rc() const;
    // "TLSv1.2 ECDHE-RSA-AES256-GCM-SHA384 (256 bit)" once the handshake is done, else empty.
    std::string tls_description() const;

    // SUBSCRIBE at QoS 0 to exactly this topic. No wildcards: see the header comment.
    bool subscribe(const std::string& topic);
    // PUBLISH at QoS 0. Returns false if the session is not up.
    bool publish(const std::string& topic, const std::string& payload);

    // {"pushing":{"sequence_id":"1","command":"pushall"}} - reports are partial deltas, so the
    // full state has to be asked for. With repeat_s > 0 it is re-issued on that period
    // (ADR-002 uses 300 s); pass 0 for a one-shot.
    bool publish_pushall(const std::string& request_topic, int repeat_s);

    static std::string make_client_id();                            // 20 random hex characters
    static std::string report_topic(const std::string& serial);     // device/<serial>/report
    static std::string request_topic(const std::string& serial);    // device/<serial>/request

    // One TCP connect + TLS handshake, synchronous, for the diagnostics probe. On success
    // out_description is filled like tls_description(); on failure out_error carries a
    // already-redacted reason. Nothing MQTT is spoken, so it also serves port 990.
    static bool probe_tls(const std::string& host,
                          uint16_t           port,
                          int                timeout_s,
                          std::string&       out_description,
                          std::string&       out_error);

private:
    struct Impl;
    std::unique_ptr<Impl> m_impl;
};

} // namespace Nocte
} // namespace Slic3r

#endif // slic3r_Utils_Nocte_NocteMqttClient_hpp_
