// NØCTE Slicer — Copyright (c) 2026 NØCTE Engineering. AGPL-3.0-or-later.

#include "NocteMqttClient.hpp"

#include <boost/asio/buffer.hpp>
#include <boost/asio/connect.hpp>
#include <boost/asio/io_context.hpp>
#include <boost/asio/ip/tcp.hpp>
#include <boost/asio/post.hpp>
#include <boost/asio/read.hpp>
#include <boost/asio/ssl.hpp>
#include <boost/asio/steady_timer.hpp>
#include <boost/asio/write.hpp>
#include <boost/log/trivial.hpp>

#include <openssl/ssl.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <deque>
#include <memory>
#include <mutex>
#include <random>
#include <system_error>
#include <thread>

namespace Slic3r {
namespace Nocte {

namespace asio = boost::asio;
using tcp      = asio::ip::tcp;

namespace {

// MQTT 3.1.1 control packet types, already shifted into the high nibble of byte 1.
constexpr uint8_t MQTT_CONNECT    = 0x10;
constexpr uint8_t MQTT_CONNACK    = 0x20;
constexpr uint8_t MQTT_PUBLISH    = 0x30;
constexpr uint8_t MQTT_PUBACK     = 0x40;
constexpr uint8_t MQTT_SUBSCRIBE  = 0x80;
constexpr uint8_t MQTT_SUBACK     = 0x90;
constexpr uint8_t MQTT_PINGREQ    = 0xC0;
constexpr uint8_t MQTT_PINGRESP   = 0xD0;
constexpr uint8_t MQTT_DISCONNECT = 0xE0;

constexpr const char* MQTT_PUSHALL_PAYLOAD = R"({"pushing":{"sequence_id":"1","command":"pushall"}})";

// A multi-plate job with four AMS units and a long `hms` array is much larger than the 3.4 kB
// maximum measured on an idle A1, so the ceiling is generous; it exists only so that a corrupt
// length field cannot make us allocate the world.
constexpr uint32_t MQTT_MAX_PACKET_BYTES = 4u * 1024u * 1024u;

void encode_remaining_length(std::string& out, uint32_t length)
{
    // MQTT-1.5.5: seven bits per byte, continuation bit set while more follow, up to four bytes.
    do {
        uint8_t digit = static_cast<uint8_t>(length % 128u);
        length /= 128u;
        if (length > 0)
            digit = static_cast<uint8_t>(digit | 0x80u);
        out.push_back(static_cast<char>(digit));
    } while (length > 0);
}

void encode_string(std::string& out, const std::string& value)
{
    // MQTT-1.5.3: a two-byte big-endian length followed by the UTF-8 bytes.
    const uint16_t length = static_cast<uint16_t>(value.size());
    out.push_back(static_cast<char>(static_cast<uint8_t>(length >> 8)));
    out.push_back(static_cast<char>(static_cast<uint8_t>(length & 0xFFu)));
    out.append(value);
}

std::string build_packet(uint8_t first_byte, const std::string& body)
{
    std::string packet;
    packet.reserve(body.size() + 5u);
    packet.push_back(static_cast<char>(first_byte));
    encode_remaining_length(packet, static_cast<uint32_t>(body.size()));
    packet.append(body);
    return packet;
}

uint8_t byte_at(const std::string& buffer, size_t index)
{
    return static_cast<uint8_t>(static_cast<unsigned char>(buffer[index]));
}

std::string describe_tls(SSL* ssl)
{
    if (ssl == nullptr)
        return std::string();
    const char* version = ::SSL_get_version(ssl);
    if (version == nullptr)
        return std::string();
    std::string       out    = version;
    const SSL_CIPHER* cipher = ::SSL_get_current_cipher(ssl);
    if (cipher != nullptr) {
        int bits = 0;
        (void) ::SSL_CIPHER_get_bits(cipher, &bits);
        const char* name = ::SSL_CIPHER_get_name(cipher);
        out += " ";
        out += (name != nullptr) ? name : "<cipher>";
        out += " (" + std::to_string(bits) + " bit)";
    }
    return out;
}

} // namespace

// --- Redaction -------------------------------------------------------------------------------

std::string redact_host(const std::string& host)
{
    const size_t last_dot = host.rfind('.');
    if (last_dot == std::string::npos || last_dot + 1 >= host.size())
        return "<host>";
    const std::string last_octet = host.substr(last_dot + 1);
    if (last_octet.size() > 3 || last_octet.find_first_not_of("0123456789") != std::string::npos)
        return "<host>";
    return "x.x.x." + last_octet;
}

std::string redact_serial(const std::string& serial)
{
    if (serial.empty())
        return "<none>";
    return serial.substr(0, (std::min)(serial.size(), static_cast<size_t>(3))) + "\xE2\x80\xA6";
}

// --- Impl ------------------------------------------------------------------------------------

struct NocteMqttClient::Impl
{
    // Declaration order matters: io_context has to outlive everything built from it.
    asio::io_context                                io;
    asio::ssl::context                              ssl_context;
    std::unique_ptr<asio::ssl::stream<tcp::socket>> stream;
    tcp::resolver                                   resolver;
    asio::steady_timer                              deadline;
    asio::steady_timer                              ping_timer;
    asio::steady_timer                              pushall_timer;
    std::thread                                     thread;

    MqttConfig  config;
    std::string client_id;
    std::string pushall_topic;
    int         pushall_period_s = 0;

    // The front element is the buffer an outstanding async_write points at, so it must not be
    // touched until that write completes - including on the teardown path.
    std::deque<std::string> write_queue;
    bool                    write_in_flight = false; // io thread only
    std::string             disconnect_packet;       // its own buffer, never queued

    // Incoming packet state; touched only on the io thread.
    uint8_t     header_byte       = 0;
    uint8_t     length_byte       = 0;
    uint32_t    remaining_length  = 0;
    uint32_t    length_multiplier = 1;
    int         length_bytes_read = 0;
    std::string body;
    uint16_t    next_packet_id = 1;

    std::atomic<bool> connected{false};
    std::atomic<bool> running{false};
    std::atomic<int>  connack_rc{MqttConnackTransportFailure};
    bool              stopping          = false; // io thread only
    bool              shutdown_finished = false; // io thread only
    bool              ping_outstanding  = false; // io thread only

    mutable std::mutex callback_mutex;
    OnMessageFn        on_message;
    OnConnectedFn      on_connected;
    OnSubscribedFn     on_subscribed;
    OnDisconnectedFn   on_disconnected;

    mutable std::mutex tls_mutex;
    std::string        tls_desc;

    Impl() : ssl_context(asio::ssl::context::tlsv12_client), resolver(io), deadline(io), ping_timer(io), pushall_timer(io)
    {
        // The printer's certificate is issued by a private "BBL CA" with CN = the serial, and we
        // dial an IP, so hostname verification can never succeed. Peer verification is off; the
        // channel never leaves the LAN. No client certificate is requested, and no SNI is sent.
        ssl_context.set_verify_mode(asio::ssl::verify_none);
    }

    std::string tag() const { return "NocteMqtt[" + redact_host(config.host) + "]: "; }

    // --- callback fan-out (callbacks run on the io thread) ---
    void fire_connected(int rc)
    {
        OnConnectedFn fn;
        {
            std::lock_guard<std::mutex> lock(callback_mutex);
            fn = on_connected;
        }
        if (fn)
            fn(rc);
    }

    void fire_subscribed(int granted_qos)
    {
        OnSubscribedFn fn;
        {
            std::lock_guard<std::mutex> lock(callback_mutex);
            fn = on_subscribed;
        }
        if (fn)
            fn(granted_qos);
    }

    void fire_disconnected(const std::string& reason)
    {
        OnDisconnectedFn fn;
        {
            std::lock_guard<std::mutex> lock(callback_mutex);
            fn = on_disconnected;
        }
        if (fn)
            fn(reason);
    }

    void fire_message(const std::string& topic, const std::string& payload)
    {
        OnMessageFn fn;
        {
            std::lock_guard<std::mutex> lock(callback_mutex);
            fn = on_message;
        }
        if (fn)
            fn(topic, payload);
    }

    std::string tls_description_locked() const
    {
        std::lock_guard<std::mutex> lock(tls_mutex);
        return tls_desc;
    }

    // --- connect ---
    void arm_deadline(int seconds, std::string what)
    {
        deadline.expires_after(std::chrono::seconds(seconds));
        deadline.async_wait([this, what](const boost::system::error_code& ec) {
            if (ec)
                return; // cancelled or re-armed
            fail_session("timed out waiting for " + what);
        });
    }

    void start_connect()
    {
        // start() posts this, so the drain at the top of the next start() can find a stale one
        // still queued - after a start() whose std::thread constructor threw, run() never ran it.
        // Without this guard the drain would open a second session on the old config, and its
        // resolve completion would then run against the stream the new session installs.
        if (stopping)
            return;
        stream = std::make_unique<asio::ssl::stream<tcp::socket>>(io, ssl_context);
        // A healthy TCP + TLS + CONNACK round trip costs ~0.9 s; the budget is deliberately
        // generous for a busy machine, but a real failure is usually immediate.
        arm_deadline(config.connect_timeout_s, "CONNACK");
        resolver.async_resolve(config.host, std::to_string(config.port),
                               [this](const boost::system::error_code& ec, const tcp::resolver::results_type& results) {
                                   if (stopping)
                                       return; // torn down, or drained by the next start()
                                   if (ec) {
                                       fail_session("resolve failed: " + ec.message());
                                       return;
                                   }
                                   do_tcp_connect(results);
                               });
    }

    void do_tcp_connect(const tcp::resolver::results_type& results)
    {
        asio::async_connect(stream->lowest_layer(), results,
                            [this](const boost::system::error_code& ec, const tcp::endpoint&) {
                                if (stopping)
                                    return;
                                if (ec) {
                                    fail_session("connect failed: " + ec.message());
                                    return;
                                }
                                boost::system::error_code option_ec;
                                stream->lowest_layer().set_option(tcp::no_delay(true), option_ec);
                                do_handshake();
                            });
    }

    void do_handshake()
    {
        stream->async_handshake(asio::ssl::stream_base::client, [this](const boost::system::error_code& ec) {
            if (stopping)
                return;
            if (ec) {
                fail_session("TLS handshake failed: " + ec.message());
                return;
            }
            {
                std::lock_guard<std::mutex> lock(tls_mutex);
                tls_desc = describe_tls(stream->native_handle());
            }
            send_connect();
            read_fixed_header();
        });
    }

    void send_connect()
    {
        std::string body_bytes;
        // Variable header (MQTT-3.1.2): protocol name, level, connect flags, keep alive.
        encode_string(body_bytes, "MQTT");
        body_bytes.push_back(static_cast<char>(0x04)); // protocol level 4 == MQTT 3.1.1
        body_bytes.push_back(static_cast<char>(0xC2)); // user name | password | clean session
        body_bytes.push_back(static_cast<char>(static_cast<uint8_t>(config.keepalive_s >> 8)));
        body_bytes.push_back(static_cast<char>(static_cast<uint8_t>(config.keepalive_s & 0xFFu)));
        // Payload (MQTT-3.1.3), in the order the flags declare it: client id, user name, password.
        encode_string(body_bytes, client_id);
        encode_string(body_bytes, config.username);
        encode_string(body_bytes, config.password);
        enqueue_write(build_packet(MQTT_CONNECT, body_bytes));
    }

    void start_keepalive()
    {
        if (config.keepalive_s == 0)
            return;
        const int period = (std::max)(1, static_cast<int>(config.keepalive_s) / 2);
        ping_timer.expires_after(std::chrono::seconds(period));
        ping_timer.async_wait([this](const boost::system::error_code& ec) {
            if (ec || stopping)
                return;
            if (ping_outstanding) {
                fail_session("no PINGRESP within one keep-alive period");
                return;
            }
            ping_outstanding = true;
            enqueue_write(build_packet(MQTT_PINGREQ, std::string()));
            start_keepalive();
        });
    }

    void start_pushall_timer()
    {
        if (pushall_period_s <= 0 || pushall_topic.empty())
            return;
        pushall_timer.expires_after(std::chrono::seconds(pushall_period_s));
        pushall_timer.async_wait([this](const boost::system::error_code& ec) {
            if (ec || stopping || !connected.load())
                return;
            enqueue_write(make_publish(pushall_topic, MQTT_PUSHALL_PAYLOAD));
            start_pushall_timer();
        });
    }

    // --- writing ---
    std::string make_publish(const std::string& topic, const std::string& payload) const
    {
        // QoS 0 PUBLISH (MQTT-3.3): flags 0, topic name, NO packet identifier, payload. QoS 1 is
        // never used - the printer delivers the message but never returns a PUBACK.
        std::string body_bytes;
        body_bytes.reserve(topic.size() + payload.size() + 2u);
        encode_string(body_bytes, topic);
        body_bytes.append(payload);
        return build_packet(MQTT_PUBLISH, body_bytes);
    }

    void enqueue_write(std::string packet)
    {
        if (stopping || !stream)
            return;
        write_queue.push_back(std::move(packet));
        if (!write_in_flight)
            do_write();
    }

    void do_write()
    {
        write_in_flight = true;
        asio::async_write(*stream, asio::buffer(write_queue.front()),
                          [this](const boost::system::error_code& ec, std::size_t) {
                              write_in_flight = false;
                              if (ec) {
                                  fail_session("write failed: " + ec.message());
                                  return;
                              }
                              if (!write_queue.empty())
                                  write_queue.pop_front();
                              if (!stopping && !write_queue.empty())
                                  do_write();
                          });
    }

    // --- reading ---
    void read_fixed_header()
    {
        asio::async_read(*stream, asio::buffer(&header_byte, 1), [this](const boost::system::error_code& ec, std::size_t) {
            if (stopping)
                return;
            if (ec) {
                fail_session("read failed: " + ec.message());
                return;
            }
            remaining_length  = 0;
            length_multiplier = 1;
            length_bytes_read = 0;
            read_remaining_length();
        });
    }

    void read_remaining_length()
    {
        asio::async_read(*stream, asio::buffer(&length_byte, 1), [this](const boost::system::error_code& ec, std::size_t) {
            if (stopping)
                return;
            if (ec) {
                fail_session("read failed: " + ec.message());
                return;
            }
            ++length_bytes_read;
            remaining_length += static_cast<uint32_t>(length_byte & 0x7Fu) * length_multiplier;
            if ((length_byte & 0x80u) != 0) {
                // The very first `pushall` answer already needs two bytes; a one-byte decoder
                // desynchronises the stream on it and every later packet is garbage.
                if (length_bytes_read >= 4) {
                    fail_session("malformed remaining length");
                    return;
                }
                length_multiplier *= 128u;
                read_remaining_length();
                return;
            }
            if (remaining_length > MQTT_MAX_PACKET_BYTES) {
                fail_session("oversized packet");
                return;
            }
            if (remaining_length == 0) {
                body.clear();
                handle_packet();
                if (!stopping)
                    read_fixed_header();
                return;
            }
            body.assign(static_cast<size_t>(remaining_length), '\0');
            read_body();
        });
    }

    void read_body()
    {
        asio::async_read(*stream, asio::buffer(&body[0], body.size()),
                         [this](const boost::system::error_code& ec, std::size_t) {
                             if (stopping)
                                 return;
                             if (ec) {
                                 fail_session("read failed: " + ec.message());
                                 return;
                             }
                             handle_packet();
                             if (!stopping)
                                 read_fixed_header();
                         });
    }

    void handle_packet()
    {
        switch (header_byte & 0xF0u) {
        case MQTT_CONNACK: handle_connack(); break;
        case MQTT_SUBACK: handle_suback(); break;
        case MQTT_PUBLISH: handle_publish(); break;
        case MQTT_PINGRESP: ping_outstanding = false; break;
        default: break; // nothing else is expected on this link
        }
    }

    void handle_connack()
    {
        // CONNACK (MQTT-3.2): one byte of session-present flags, then the return code.
        if (body.size() < 2) {
            fail_session("short CONNACK");
            return;
        }
        const int rc = static_cast<int>(byte_at(body, 1));
        connack_rc.store(rc);
        deadline.cancel();
        if (rc != MqttConnackAccepted) {
            BOOST_LOG_TRIVIAL(error) << tag() << "CONNACK rc=" << rc
                                     << (rc == MqttConnackBadCredentials || rc == MqttConnackNotAuthorized
                                             ? " (the access code is wrong)"
                                             : "");
            connected.store(false);
            fire_connected(rc);
            begin_shutdown(/*graceful=*/false);
            return;
        }
        connected.store(true);
        BOOST_LOG_TRIVIAL(info) << tag() << "connected over " << tls_description_locked();
        start_keepalive();
        fire_connected(rc);
    }

    void handle_suback()
    {
        // SUBACK (MQTT-3.9): packet identifier, then one return code per filter. 0x80 = refused.
        // A refusal here, or an immediate disconnect, is what a wildcard filter looks like.
        const int granted = body.size() >= 3 ? static_cast<int>(byte_at(body, 2)) : 0x80;
        if (granted == 0x80)
            BOOST_LOG_TRIVIAL(error) << tag() << "SUBACK refused the subscription";
        fire_subscribed(granted);
    }

    void handle_publish()
    {
        const uint8_t qos = static_cast<uint8_t>((header_byte >> 1) & 0x03u);
        if (body.size() < 2)
            return;
        const size_t topic_length = (static_cast<size_t>(byte_at(body, 0)) << 8) | static_cast<size_t>(byte_at(body, 1));
        size_t       offset       = 2;
        if (body.size() < offset + topic_length)
            return;
        const std::string topic = body.substr(offset, topic_length);
        offset += topic_length;
        if (qos > 0) {
            if (body.size() < offset + 2)
                return;
            // The SUBACK grants QoS 0, so this is defensive only: acknowledging keeps a firmware
            // that upgraded the QoS from redelivering the same report forever.
            std::string ack;
            ack.push_back(body[offset]);
            ack.push_back(body[offset + 1]);
            offset += 2;
            if (qos == 1)
                enqueue_write(build_packet(MQTT_PUBACK, ack));
        }
        fire_message(topic, body.substr(offset));
    }

    // --- teardown ---
    void fail_session(const std::string& reason)
    {
        if (stopping)
            return;
        BOOST_LOG_TRIVIAL(warning) << tag() << reason;
        connected.store(false);
        fire_disconnected(reason);
        begin_shutdown(/*graceful=*/false);
    }

    void begin_shutdown(bool graceful)
    {
        if (stopping)
            return;
        stopping = true;
        deadline.cancel();
        ping_timer.cancel();
        pushall_timer.cancel();

        const bool was_connected = connected.exchange(false);
        // A write already in flight owns write_queue.front(); starting a second one on the same
        // stream is undefined, and clearing the queue under it would leave a dangling buffer. In
        // that case the DISCONNECT is simply skipped - dropping the TCP connection is a normal
        // end of session for this printer.
        if (!graceful || !was_connected || !stream || write_in_flight) {
            finish_shutdown();
            return;
        }

        // Best-effort DISCONNECT, bounded so that a half-dead socket cannot hold the thread.
        disconnect_packet = build_packet(MQTT_DISCONNECT, std::string());
        deadline.expires_after(std::chrono::seconds(1));
        deadline.async_wait([this](const boost::system::error_code& ec) {
            if (ec)
                return;
            finish_shutdown();
        });
        asio::async_write(*stream, asio::buffer(disconnect_packet),
                          [this](const boost::system::error_code&, std::size_t) {
                              deadline.cancel();
                              finish_shutdown();
                          });
    }

    void finish_shutdown()
    {
        if (shutdown_finished)
            return;
        shutdown_finished = true;
        if (stream) {
            boost::system::error_code ec;
            // A printer that drops TCP without a TLS close_notify is normal here, so every
            // error from the teardown path is tolerated.
            stream->lowest_layer().cancel(ec);
            stream->lowest_layer().shutdown(tcp::socket::shutdown_both, ec);
            stream->lowest_layer().close(ec);
        }
        io.stop();
    }
};

// --- NocteMqttClient -------------------------------------------------------------------------

NocteMqttClient::NocteMqttClient() : m_impl(std::make_unique<Impl>()) {}

NocteMqttClient::~NocteMqttClient() { stop(); }

void NocteMqttClient::set_on_message(OnMessageFn fn)
{
    std::lock_guard<std::mutex> lock(m_impl->callback_mutex);
    m_impl->on_message = std::move(fn);
}

void NocteMqttClient::set_on_connected(OnConnectedFn fn)
{
    std::lock_guard<std::mutex> lock(m_impl->callback_mutex);
    m_impl->on_connected = std::move(fn);
}

void NocteMqttClient::set_on_subscribed(OnSubscribedFn fn)
{
    std::lock_guard<std::mutex> lock(m_impl->callback_mutex);
    m_impl->on_subscribed = std::move(fn);
}

void NocteMqttClient::set_on_disconnected(OnDisconnectedFn fn)
{
    std::lock_guard<std::mutex> lock(m_impl->callback_mutex);
    m_impl->on_disconnected = std::move(fn);
}

bool NocteMqttClient::start(const MqttConfig& config)
{
    stop();

    Impl* impl = m_impl.get();

    // The previous session's thread is joined by now, so this is single-threaded again.
    //
    // io_context::stop() does NOT discard queued handlers: everything the last session left
    // behind - its cancelled read and write completions, and the shutdown stop() posts when the
    // context has already stopped - is still in the queue and would run as the first thing this
    // session's thread sees. That tore the new session down before start_connect() ever ran.
    // Drain them here instead, with `stopping` still set so that every one of them is a no-op.
    impl->stopping = true;
    impl->io.restart();
    impl->io.poll();
    impl->io.restart();

    impl->config    = config;
    impl->client_id = make_client_id();
    impl->connack_rc.store(MqttConnackTransportFailure);
    impl->connected.store(false);
    impl->write_queue.clear();
    impl->write_in_flight = false;
    impl->disconnect_packet.clear();
    impl->pushall_topic.clear();
    impl->pushall_period_s  = 0;
    impl->ping_outstanding  = false;
    impl->stopping          = false;
    impl->shutdown_finished = false;
    {
        std::lock_guard<std::mutex> lock(impl->tls_mutex);
        impl->tls_desc.clear();
    }

    impl->io.restart();
    // Posted before the thread exists, so run() can never find the queue empty and return early;
    // start_connect() then keeps outstanding work alive until finish_shutdown() stops the context.
    asio::post(impl->io, [impl]() { impl->start_connect(); });

    try {
        impl->thread = std::thread([impl]() {
            // A handler that throws unwinds out of run() but leaves the context runnable, so
            // keep running it: otherwise the session would look connected with no thread
            // behind it. The loop ends when run() returns normally (context stopped).
            for (;;) {
                try {
                    impl->io.run();
                    break;
                } catch (const std::exception& e) {
                    BOOST_LOG_TRIVIAL(error) << impl->tag() << "handler threw: " << e.what();
                } catch (...) {
                    BOOST_LOG_TRIVIAL(error) << impl->tag() << "handler threw";
                }
            }
        });
    } catch (const std::system_error& e) {
        BOOST_LOG_TRIVIAL(error) << impl->tag() << "could not start the MQTT thread: " << e.what();
        impl->io.stop();
        return false;
    }

    impl->running.store(true);
    return true;
}

void NocteMqttClient::stop()
{
    Impl* impl = m_impl.get();
    if (impl->running.exchange(false))
        asio::post(impl->io, [impl]() { impl->begin_shutdown(/*graceful=*/true); });
    if (impl->thread.joinable())
        impl->thread.join();
    impl->connected.store(false);
}

bool NocteMqttClient::is_connected() const { return m_impl->connected.load(); }

int NocteMqttClient::last_connack_rc() const { return m_impl->connack_rc.load(); }

std::string NocteMqttClient::tls_description() const { return m_impl->tls_description_locked(); }

bool NocteMqttClient::subscribe(const std::string& topic)
{
    Impl* impl = m_impl.get();
    if (!impl->connected.load() || topic.empty())
        return false;
    asio::post(impl->io, [impl, topic]() {
        // SUBSCRIBE (MQTT-3.8) carries the reserved flags 0010 in the low nibble, hence 0x82:
        // packet identifier, then the topic filter and its requested QoS.
        std::string    body_bytes;
        const uint16_t packet_id = impl->next_packet_id++;
        if (impl->next_packet_id == 0)
            impl->next_packet_id = 1;
        body_bytes.push_back(static_cast<char>(static_cast<uint8_t>(packet_id >> 8)));
        body_bytes.push_back(static_cast<char>(static_cast<uint8_t>(packet_id & 0xFFu)));
        encode_string(body_bytes, topic);
        body_bytes.push_back(static_cast<char>(0x00)); // requested QoS 0
        impl->enqueue_write(build_packet(static_cast<uint8_t>(MQTT_SUBSCRIBE | 0x02u), body_bytes));
    });
    return true;
}

bool NocteMqttClient::publish(const std::string& topic, const std::string& payload)
{
    Impl* impl = m_impl.get();
    if (!impl->connected.load() || topic.empty())
        return false;
    asio::post(impl->io, [impl, topic, payload]() { impl->enqueue_write(impl->make_publish(topic, payload)); });
    return true;
}

bool NocteMqttClient::publish_pushall(const std::string& request_topic_name, int repeat_s)
{
    Impl* impl = m_impl.get();
    if (!impl->connected.load() || request_topic_name.empty())
        return false;
    asio::post(impl->io, [impl, request_topic_name, repeat_s]() {
        impl->pushall_topic    = request_topic_name;
        impl->pushall_period_s = repeat_s;
        impl->enqueue_write(impl->make_publish(request_topic_name, MQTT_PUSHALL_PAYLOAD));
        impl->start_pushall_timer();
    });
    return true;
}

std::string NocteMqttClient::make_client_id()
{
    // Unique per call, so two NØCTE instances on the same LAN never evict each other.
    std::random_device device;
    std::mt19937       engine(static_cast<unsigned>(device()) ^
                        static_cast<unsigned>(std::chrono::steady_clock::now().time_since_epoch().count()));
    std::uniform_int_distribution<int> digit(0, 15);
    static const char                  hex[] = "0123456789abcdef";
    std::string                        id;
    id.reserve(20);
    for (int i = 0; i < 20; ++i)
        id.push_back(hex[digit(engine)]);
    return id;
}

std::string NocteMqttClient::report_topic(const std::string& serial) { return "device/" + serial + "/report"; }

std::string NocteMqttClient::request_topic(const std::string& serial) { return "device/" + serial + "/request"; }

bool NocteMqttClient::probe_tls(const std::string& host,
                                uint16_t           port,
                                int                timeout_s,
                                std::string&       out_description,
                                std::string&       out_error)
{
    out_description.clear();
    out_error.clear();

    asio::io_context   io;
    asio::ssl::context context(asio::ssl::context::tlsv12_client);
    context.set_verify_mode(asio::ssl::verify_none);

    asio::ssl::stream<tcp::socket> stream(io, context);
    tcp::resolver                  resolver(io);

    bool        succeeded = false;
    std::string failure;

    resolver.async_resolve(
        host, std::to_string(port), [&](const boost::system::error_code& ec, const tcp::resolver::results_type& results) {
            if (ec) {
                failure = "resolve failed: " + ec.message();
                return;
            }
            asio::async_connect(
                stream.lowest_layer(), results, [&](const boost::system::error_code& connect_ec, const tcp::endpoint&) {
                    if (connect_ec) {
                        failure = "connect failed: " + connect_ec.message();
                        return;
                    }
                    stream.async_handshake(asio::ssl::stream_base::client, [&](const boost::system::error_code& tls_ec) {
                        if (tls_ec) {
                            failure = "TLS handshake failed: " + tls_ec.message();
                            return;
                        }
                        out_description = describe_tls(stream.native_handle());
                        succeeded       = true;
                    });
                });
        });

    io.run_for(std::chrono::seconds((std::max)(1, timeout_s)));

    boost::system::error_code ec;
    stream.lowest_layer().cancel(ec);
    stream.lowest_layer().close(ec);

    if (succeeded)
        return true;
    out_error = failure.empty() ? "timed out after " + std::to_string(timeout_s) + " s" : failure;
    return false;
}

} // namespace Nocte
} // namespace Slic3r
