// NØCTE Slicer — Copyright (c) 2026 NØCTE Engineering. AGPL-3.0-or-later.

#include "NocteFtps.hpp"
#include "NocteMqttClient.hpp" // redact_host()

#include "slic3r/Utils/Http.hpp"

#include <boost/algorithm/string.hpp>
#include <boost/log/trivial.hpp>
#include <boost/nowide/cstdio.hpp>

#include <curl/curl.h>

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <utility>

namespace Slic3r {
namespace Nocte {

namespace {

// curl_global_init() has to have happened before any easy handle exists. Http owns that in this
// tree; the probe can run before the GUI does, so ask for it explicitly and only once.
void ensure_curl_global_init()
{
    static const std::string message = Http::tls_global_init();
    (void) message;
}

struct CurlHandle
{
    CURL* handle = nullptr;

    CurlHandle() : handle(::curl_easy_init()) {}
    ~CurlHandle()
    {
        if (handle != nullptr)
            ::curl_easy_cleanup(handle);
    }

    CurlHandle(const CurlHandle&)            = delete;
    CurlHandle& operator=(const CurlHandle&) = delete;

    explicit operator bool() const { return handle != nullptr; }
};

struct MemorySource
{
    const std::string* data   = nullptr;
    size_t             offset = 0;
};

struct ProgressState
{
    const NocteFtps::ProgressFn* progress = nullptr;
    const NocteFtps::CancelFn*   cancel   = nullptr;
};

size_t write_to_string(char* pointer, size_t size, size_t count, void* user_data)
{
    const size_t bytes = size * count;
    static_cast<std::string*>(user_data)->append(pointer, bytes);
    return bytes;
}

size_t write_discard(char*, size_t size, size_t count, void*) { return size * count; }

size_t read_from_file(char* buffer, size_t size, size_t count, void* user_data)
{
    return std::fread(buffer, size, count, static_cast<std::FILE*>(user_data));
}

size_t read_from_memory(char* buffer, size_t size, size_t count, void* user_data)
{
    MemorySource& source    = *static_cast<MemorySource*>(user_data);
    const size_t  wanted    = size * count;
    const size_t  available = source.data->size() - source.offset;
    const size_t  bytes     = (std::min)(wanted, available);
    if (bytes > 0) {
        std::memcpy(buffer, source.data->data() + source.offset, bytes);
        source.offset += bytes;
    }
    return bytes;
}

int report_progress(void* user_data, curl_off_t, curl_off_t, curl_off_t upload_total, curl_off_t upload_now)
{
    ProgressState& state = *static_cast<ProgressState*>(user_data);
    if (state.cancel != nullptr && *state.cancel && (*state.cancel)())
        return 1; // -> CURLE_ABORTED_BY_CALLBACK
    if (state.progress != nullptr && *state.progress && upload_total > 0)
        (*state.progress)(static_cast<double>(upload_now) / static_cast<double>(upload_total));
    return 0;
}

std::vector<std::string> split_listing(const std::string& text)
{
    std::vector<std::string> names;
    std::string              line;
    for (const char character : text) {
        if (character == '\n' || character == '\r') {
            if (!line.empty()) {
                if (line != "." && line != "..")
                    names.push_back(line);
                line.clear();
            }
            continue;
        }
        line.push_back(character);
    }
    if (!line.empty() && line != "." && line != "..")
        names.push_back(line);
    return names;
}

} // namespace

NocteFtps::NocteFtps(FtpsConfig config) : m_config(std::move(config)) {}

bool NocteFtps::is_safe_remote_name(const std::string& name)
{
    if (name.empty() || name.size() > 120)
        return false;
    if (name.front() == '-' || name.front() == '.')
        return false;
    for (const char character : name) {
        const unsigned char value = static_cast<unsigned char>(character);
        if (value < 0x20 || value > 0x7E)
            return false; // the printer wants ASCII names
        if (std::strchr("/\\:*?\"<>|,;", character) != nullptr)
            return false;
        if (character == ' ')
            return false;
    }
    return boost::iends_with(name, ".3mf") || boost::iends_with(name, ".txt");
}

std::string NocteFtps::sanitize_remote_name(const std::string& local_path)
{
    size_t start = local_path.find_last_of("/\\");
    start        = (start == std::string::npos) ? 0 : start + 1;
    std::string leaf = local_path.substr(start);

    std::string cleaned;
    cleaned.reserve(leaf.size());
    for (const char character : leaf) {
        const unsigned char value = static_cast<unsigned char>(character);
        if (value < 0x20 || value > 0x7E || std::strchr("/\\:*?\"<>|,; ", character) != nullptr)
            cleaned.push_back('_');
        else
            cleaned.push_back(character);
    }
    while (!cleaned.empty() && (cleaned.front() == '-' || cleaned.front() == '.'))
        cleaned.erase(cleaned.begin());
    if (cleaned.size() > 120)
        cleaned = cleaned.substr(cleaned.size() - 120);
    return cleaned;
}

std::string NocteFtps::base_url() const
{
    // ftps:// + the implicit-TLS port is what selects implicit mode; libcurl must never be given
    // the chance to try `AUTH TLS` on this link.
    return "ftps://" + m_config.host + ":" + std::to_string(m_config.port) + "/";
}

std::string NocteFtps::file_url(const std::string& remote_name) const { return base_url() + remote_name; }

std::string NocteFtps::scrub(const std::string& text) const
{
    std::string out = text;
    if (!m_config.host.empty()) {
        boost::replace_all(out, m_config.host, redact_host(m_config.host));
        // The 227 reply spells the same address as h1,h2,h3,h4 - the dotted form above misses it.
        std::string comma_form = m_config.host;
        boost::replace_all(comma_form, ".", ",");
        boost::replace_all(out, comma_form, "<pasv-addr>");
    }
    if (!m_config.password.empty())
        boost::replace_all(out, m_config.password, "<access-code>");
    return out;
}

void NocteFtps::apply_common_options(void* curl_handle, char* error_buffer) const
{
    CURL* curl = static_cast<CURL*>(curl_handle);
    ::curl_easy_setopt(curl, CURLOPT_USE_SSL, static_cast<long>(CURLUSESSL_ALL)); // PROT P
    // The certificate is issued by a private "BBL CA" with CN = the printer serial and we dial
    // an IP, so neither check can ever pass. The channel does not leave the LAN.
    ::curl_easy_setopt(curl, CURLOPT_SSL_VERIFYPEER, 0L);
    ::curl_easy_setopt(curl, CURLOPT_SSL_VERIFYHOST, 0L);
    ::curl_easy_setopt(curl, CURLOPT_USERNAME, m_config.username.c_str());
    ::curl_easy_setopt(curl, CURLOPT_PASSWORD, m_config.password.c_str());
    // Passive mode is what the printer offers; libcurl's EPSV-then-PASV default is fine. The
    // address in the 227 reply is the printer's own and is ignored in favour of the control
    // connection's peer, which keeps NAT and multi-homed hosts working.
    ::curl_easy_setopt(curl, CURLOPT_FTP_SKIP_PASV_IP, 1L);
    ::curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT, m_config.connect_timeout_s);
    ::curl_easy_setopt(curl, CURLOPT_TIMEOUT, m_config.transfer_timeout_s);
    // Bound the wait for a reply the printer often never sends, instead of the socket timeout.
    ::curl_easy_setopt(curl, CURLOPT_FTP_RESPONSE_TIMEOUT, m_config.response_timeout_s);
    ::curl_easy_setopt(curl, CURLOPT_NOSIGNAL, 1L);
    ::curl_easy_setopt(curl, CURLOPT_ERRORBUFFER, error_buffer);
    error_buffer[0] = '\0';
}

bool NocteFtps::list(std::vector<std::string>& out_names, std::string& error)
{
    out_names.clear();
    error.clear();
    ensure_curl_global_init();

    CurlHandle curl;
    if (!curl) {
        error = "could not create a libcurl handle";
        return false;
    }

    char        error_buffer[CURL_ERROR_SIZE] = {0};
    std::string listing;
    apply_common_options(curl.handle, error_buffer);
    ::curl_easy_setopt(curl.handle, CURLOPT_URL, base_url().c_str());
    ::curl_easy_setopt(curl.handle, CURLOPT_DIRLISTONLY, 1L); // NLST rather than LIST
    ::curl_easy_setopt(curl.handle, CURLOPT_WRITEFUNCTION, &write_to_string);
    ::curl_easy_setopt(curl.handle, CURLOPT_WRITEDATA, &listing);

    const CURLcode result = ::curl_easy_perform(curl.handle);
    if (result != CURLE_OK) {
        error = scrub(std::string(::curl_easy_strerror(result)) +
                      (error_buffer[0] != '\0' ? std::string(": ") + error_buffer : std::string()));
        return false;
    }

    out_names = split_listing(listing);
    return true;
}

bool NocteFtps::upload_file(const std::string& local_path,
                            const std::string& remote_name,
                            std::string&       error,
                            const ProgressFn&  progress,
                            const CancelFn&    cancel)
{
    error.clear();
    if (!is_safe_remote_name(remote_name)) {
        error = "refusing to upload under the name '" + remote_name + "'";
        return false;
    }
    ensure_curl_global_init();

    std::FILE* file = boost::nowide::fopen(local_path.c_str(), "rb");
    if (file == nullptr) {
        error = "could not open the job file for reading";
        return false;
    }
    if (std::fseek(file, 0, SEEK_END) != 0) {
        std::fclose(file);
        error = "could not measure the job file";
        return false;
    }
    const long file_size = std::ftell(file);
    std::rewind(file);
    if (file_size < 0) {
        std::fclose(file);
        error = "could not measure the job file";
        return false;
    }

    CurlHandle curl;
    if (!curl) {
        std::fclose(file);
        error = "could not create a libcurl handle";
        return false;
    }

    char          error_buffer[CURL_ERROR_SIZE] = {0};
    ProgressState progress_state;
    progress_state.progress = &progress;
    progress_state.cancel   = &cancel;

    apply_common_options(curl.handle, error_buffer);
    ::curl_easy_setopt(curl.handle, CURLOPT_URL, file_url(remote_name).c_str());
    ::curl_easy_setopt(curl.handle, CURLOPT_UPLOAD, 1L); // STOR
    ::curl_easy_setopt(curl.handle, CURLOPT_READFUNCTION, &read_from_file);
    ::curl_easy_setopt(curl.handle, CURLOPT_READDATA, file);
    ::curl_easy_setopt(curl.handle, CURLOPT_INFILESIZE_LARGE, static_cast<curl_off_t>(file_size));
    ::curl_easy_setopt(curl.handle, CURLOPT_WRITEFUNCTION, &write_discard);
    ::curl_easy_setopt(curl.handle, CURLOPT_NOPROGRESS, 0L);
    ::curl_easy_setopt(curl.handle, CURLOPT_XFERINFOFUNCTION, &report_progress);
    ::curl_easy_setopt(curl.handle, CURLOPT_XFERINFODATA, &progress_state);

    const CURLcode result = ::curl_easy_perform(curl.handle);
    curl_off_t     sent   = 0;
    ::curl_easy_getinfo(curl.handle, CURLINFO_SIZE_UPLOAD_T, &sent);
    std::fclose(file);

    if (result == CURLE_ABORTED_BY_CALLBACK) {
        error = "upload cancelled";
        return false;
    }
    if (result != CURLE_OK && sent < static_cast<curl_off_t>(file_size)) {
        error = scrub(std::string(::curl_easy_strerror(result)) +
                      (error_buffer[0] != '\0' ? std::string(": ") + error_buffer : std::string()));
        return false;
    }
    if (result != CURLE_OK) {
        // Every byte arrived and the printer simply never sent the final 226. This is the normal
        // outcome on this firmware, not a failure; the listing below is what decides.
        BOOST_LOG_TRIVIAL(info) << "NocteFtps[" << redact_host(m_config.host)
                                << "]: no final reply after STOR (" << ::curl_easy_strerror(result)
                                << "); verifying by NLST";
    }

    std::vector<std::string> names;
    std::string              list_error;
    if (!list(names, list_error)) {
        error = "upload could not be verified: " + list_error;
        return false;
    }
    if (std::find(names.begin(), names.end(), remote_name) == names.end()) {
        error = "the SD card does not list '" + remote_name + "' after the upload";
        return false;
    }
    return true;
}

bool NocteFtps::upload_memory(const std::string& data, const std::string& remote_name, std::string& error)
{
    error.clear();
    if (!is_safe_remote_name(remote_name)) {
        error = "refusing to upload under the name '" + remote_name + "'";
        return false;
    }
    ensure_curl_global_init();

    CurlHandle curl;
    if (!curl) {
        error = "could not create a libcurl handle";
        return false;
    }

    char         error_buffer[CURL_ERROR_SIZE] = {0};
    MemorySource source;
    source.data = &data;

    apply_common_options(curl.handle, error_buffer);
    ::curl_easy_setopt(curl.handle, CURLOPT_URL, file_url(remote_name).c_str());
    ::curl_easy_setopt(curl.handle, CURLOPT_UPLOAD, 1L);
    ::curl_easy_setopt(curl.handle, CURLOPT_READFUNCTION, &read_from_memory);
    ::curl_easy_setopt(curl.handle, CURLOPT_READDATA, &source);
    ::curl_easy_setopt(curl.handle, CURLOPT_INFILESIZE_LARGE, static_cast<curl_off_t>(data.size()));
    ::curl_easy_setopt(curl.handle, CURLOPT_WRITEFUNCTION, &write_discard);

    const CURLcode result = ::curl_easy_perform(curl.handle);
    curl_off_t     sent   = 0;
    ::curl_easy_getinfo(curl.handle, CURLINFO_SIZE_UPLOAD_T, &sent);

    if (result != CURLE_OK && sent < static_cast<curl_off_t>(data.size())) {
        error = scrub(std::string(::curl_easy_strerror(result)) +
                      (error_buffer[0] != '\0' ? std::string(": ") + error_buffer : std::string()));
        return false;
    }

    std::vector<std::string> names;
    std::string              list_error;
    if (!list(names, list_error)) {
        error = "upload could not be verified: " + list_error;
        return false;
    }
    if (std::find(names.begin(), names.end(), remote_name) == names.end()) {
        error = "the SD card does not list '" + remote_name + "' after the upload";
        return false;
    }
    return true;
}

bool NocteFtps::remove(const std::string& remote_name, std::string& error)
{
    error.clear();
    if (!is_safe_remote_name(remote_name)) {
        error = "refusing to delete '" + remote_name + "'";
        return false;
    }
    ensure_curl_global_init();

    CurlHandle curl;
    if (!curl) {
        error = "could not create a libcurl handle";
        return false;
    }

    char              error_buffer[CURL_ERROR_SIZE] = {0};
    const std::string command                       = "DELE " + remote_name;
    curl_slist*       commands                      = ::curl_slist_append(nullptr, command.c_str());
    if (commands == nullptr) {
        error = "could not build the DELE command";
        return false;
    }

    apply_common_options(curl.handle, error_buffer);
    ::curl_easy_setopt(curl.handle, CURLOPT_URL, base_url().c_str());
    ::curl_easy_setopt(curl.handle, CURLOPT_NOBODY, 1L);
    ::curl_easy_setopt(curl.handle, CURLOPT_QUOTE, commands);
    ::curl_easy_setopt(curl.handle, CURLOPT_WRITEFUNCTION, &write_discard);

    const CURLcode result = ::curl_easy_perform(curl.handle);
    ::curl_slist_free_all(commands);

    if (result != CURLE_OK) {
        error = scrub(std::string(::curl_easy_strerror(result)) +
                      (error_buffer[0] != '\0' ? std::string(": ") + error_buffer : std::string()));
        return false;
    }

    std::vector<std::string> names;
    std::string              list_error;
    if (!list(names, list_error)) {
        error = "deletion could not be verified: " + list_error;
        return false;
    }
    if (std::find(names.begin(), names.end(), remote_name) != names.end()) {
        error = "the SD card still lists '" + remote_name + "' after the deletion";
        return false;
    }
    return true;
}

} // namespace Nocte
} // namespace Slic3r
