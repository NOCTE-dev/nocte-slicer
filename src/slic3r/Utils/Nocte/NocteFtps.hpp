// NØCTE Slicer — Copyright (c) 2026 NØCTE Engineering. AGPL-3.0-or-later.
//
// FTPS against a Bambu printer in LAN Developer Mode (ADR-002), over the vendored libcurl.
//
// Port 990 is *implicit* TLS: the handshake starts on the first byte, with no `AUTH TLS`.
// Getting that wrong is the classic hang, so the URL scheme and the port are both pinned here
// and never taken from a caller. The SD card has no directories as far as this class is
// concerned: every name is a bare file name at the root.
//
// The printer frequently omits the final `226` after a `STOR` even though every byte arrived,
// so upload success is decided by re-listing the SD card, never by the reply code.

#ifndef slic3r_Utils_Nocte_NocteFtps_hpp_
#define slic3r_Utils_Nocte_NocteFtps_hpp_

#include <cstdint>
#include <functional>
#include <string>
#include <vector>

namespace Slic3r {
namespace Nocte {

struct FtpsConfig
{
    std::string host;                    // printer IP; never logged unredacted
    uint16_t    port     = 990;
    std::string username = "bblp";
    std::string password;                // printer access code
    long        connect_timeout_s = 15;
    // The post-transfer wait, NOT the socket timeout: the printer simply never answers, and a
    // 60 s block would look like a hung UI. Bound it and verify by listing instead.
    long        response_timeout_s = 10;
    long        transfer_timeout_s = 900; // whole STOR/NLST operation
};

class NocteFtps
{
public:
    // Fraction complete in [0, 1] while bytes are moving.
    using ProgressFn = std::function<void(double)>;
    // Return true to abort the transfer.
    using CancelFn = std::function<bool()>;

    explicit NocteFtps(FtpsConfig config);

    // NLST at the SD-card root. Returns false and fills `error` (already redacted) on failure.
    bool list(std::vector<std::string>& out_names, std::string& error);

    // STOR a local file as `remote_name` at the root, then confirm by NLST. Returns true only
    // when the name shows up in the listing afterwards.
    bool upload_file(const std::string& local_path,
                     const std::string& remote_name,
                     std::string&       error,
                     const ProgressFn&  progress = nullptr,
                     const CancelFn&    cancel   = nullptr);

    // Same, from a buffer already in memory (used by the diagnostics probe).
    bool upload_memory(const std::string& data, const std::string& remote_name, std::string& error);

    // DELE `remote_name`, then confirm by NLST that it is gone. Only names this build could have
    // written are accepted (see is_safe_remote_name).
    bool remove(const std::string& remote_name, std::string& error);

    // A bare ASCII file name ending in .3mf or .txt, with no path separator, no wildcard and no
    // leading dash. Anything else is refused before a byte reaches the wire.
    static bool is_safe_remote_name(const std::string& name);

    // The leaf of a local path, stripped down to what is_safe_remote_name() accepts.
    static std::string sanitize_remote_name(const std::string& local_path);

private:
    std::string base_url() const;
    std::string file_url(const std::string& remote_name) const;
    // Masks the printer address in a libcurl message, including the `h1,h2,h3,h4,p1,p2` form a
    // 227 reply uses - without that rule the printer's own address reaches the log on every
    // passive transfer.
    std::string scrub(const std::string& text) const;
    // The options every operation shares: implicit TLS on 990, credentials, timeouts. `curl` is
    // a CURL* and `error_buffer` a CURL_ERROR_SIZE array; kept as void*/char* so that this
    // header does not drag <curl/curl.h> into every consumer.
    void apply_common_options(void* curl, char* error_buffer) const;

    FtpsConfig m_config;
};

} // namespace Nocte
} // namespace Slic3r

#endif // slic3r_Utils_Nocte_NocteFtps_hpp_
