// TLS transport for Graph's pre-authenticated URLs. The request bytes come from
// the pure BuildRawHttpRequest and the response is parsed by the pure
// ParseRawHttpResponse / RawHttpResponseComplete; this file only moves bytes
// over a verified TLS connection, with every phase bounded in time and size.
// Proven by the live SQL suite (reads go through the /content download
// redirect, multi-chunk writes through the uploadUrl).
#include "sharepoint_preauth.hpp"
#include "sharepoint_upload.hpp"

#include "duckdb/common/exception.hpp"

// Only for detail::load_system_certs_on_windows -- the same trust-store loader
// httplib's own SSLClient uses there, so both transports trust the same roots.
#define CPPHTTPLIB_OPENSSL_SUPPORT
#include "httplib.hpp"

#include <openssl/bio.h>
#include <openssl/err.h>
#include <openssl/ssl.h>
#include <openssl/x509v3.h>

#ifdef _WIN32
#include <winsock2.h>
#else
#include <sys/select.h>
#include <sys/socket.h>
#include <sys/time.h>
#endif

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <memory>
#include <mutex>

namespace duckdb {
namespace sharepoint {

namespace {

//! Room for the status line and headers on top of the body cap.
constexpr std::size_t kMaxHeadBytes = 64 * 1024;

// One client context for the process: the trust store is loaded once.
SSL_CTX *SharedContext() {
	static SSL_CTX *ctx = nullptr;
	static std::once_flag once;
	std::call_once(once, [] {
		SSL_CTX *c = SSL_CTX_new(TLS_client_method());
		if (!c) {
			return;
		}
		SSL_CTX_set_min_proto_version(c, TLS1_2_VERSION);
		SSL_CTX_set_verify(c, SSL_VERIFY_PEER, nullptr);
		bool loaded = false;
#ifdef _WIN32
		loaded = duckdb_httplib_openssl::detail::load_system_certs_on_windows(SSL_CTX_get_cert_store(c));
#endif
		if (!loaded) {
			SSL_CTX_set_default_verify_paths(c);
		}
		ctx = c;
	});
	return ctx;
}

struct BioDeleter {
	void operator()(BIO *b) const {
		BIO_free_all(b);
	}
};
using BioPtr = std::unique_ptr<BIO, BioDeleter>;

[[noreturn]] void ThrowTransport(const std::string &what, const std::string &host) {
	// The host is safe to show; the path and query (the credential) are not.
	unsigned long err = ERR_get_error();
	char buf[256] = {0};
	if (err) {
		ERR_error_string_n(err, buf, sizeof(buf));
	}
	ERR_clear_error();
	throw IOException("sharepoint: %s with %s failed%s%s", what, host, err ? ": " : "", buf);
}

// Wait until `fd` is writable (a non-blocking connect finished) or the
// deadline passes. Returns false on timeout or error.
bool WaitWritable(int fd, std::chrono::steady_clock::time_point deadline) {
	const auto now = std::chrono::steady_clock::now();
	if (now >= deadline) {
		return false;
	}
	const auto left = std::chrono::duration_cast<std::chrono::microseconds>(deadline - now).count();
	fd_set wfds;
	FD_ZERO(&wfds);
#ifdef _WIN32
	FD_SET(static_cast<SOCKET>(fd), &wfds);
#else
	FD_SET(fd, &wfds);
#endif
	struct timeval tv;
	tv.tv_sec = static_cast<long>(left / 1000000);
	tv.tv_usec = static_cast<long>(left % 1000000);
	return select(fd + 1, nullptr, &wfds, nullptr, &tv) > 0;
}

// Install receive/send timeouts. Fails CLOSED: a socket we cannot bound is a
// socket that can hang a DuckDB thread forever.
bool SetSocketTimeouts(int fd, int seconds) {
#ifdef _WIN32
	DWORD ms = static_cast<DWORD>(seconds) * 1000;
	return setsockopt(static_cast<SOCKET>(fd), SOL_SOCKET, SO_RCVTIMEO, reinterpret_cast<const char *>(&ms),
	                  sizeof(ms)) == 0 &&
	       setsockopt(static_cast<SOCKET>(fd), SOL_SOCKET, SO_SNDTIMEO, reinterpret_cast<const char *>(&ms),
	                  sizeof(ms)) == 0;
#else
	struct timeval tv;
	tv.tv_sec = seconds;
	tv.tv_usec = 0;
	return setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv)) == 0 &&
	       setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv)) == 0;
#endif
}

// TCP connect to host:port within `timeout_seconds`, then switch the socket
// back to blocking with send/receive timeouts installed.
BioPtr ConnectWithDeadline(const std::string &host, int port, int timeout_seconds) {
	const std::string hostport = host + ":" + std::to_string(port);
	BioPtr conn(BIO_new_connect(hostport.c_str()));
	if (!conn) {
		ThrowTransport("socket setup", host);
	}
	BIO_set_nbio(conn.get(), 1);
	const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(timeout_seconds);
	while (BIO_do_connect(conn.get()) <= 0) {
		int fd = -1;
		if (!BIO_should_retry(conn.get()) || BIO_get_fd(conn.get(), &fd) < 0 || fd < 0) {
			ThrowTransport("connect", host);
		}
		if (!WaitWritable(fd, deadline)) {
			ThrowTransport("connect (timed out)", host);
		}
	}
	int fd = -1;
	if (BIO_get_fd(conn.get(), &fd) < 0 || fd < 0 || !BIO_socket_nbio(fd, 0) ||
	    !SetSocketTimeouts(fd, timeout_seconds)) {
		ThrowTransport("socket timeout setup", host);
	}
	BIO_set_nbio(conn.get(), 0);
	return conn;
}

} // namespace

RawHttpResponse PreauthenticatedRequest(const std::string &method, const std::string &url,
                                        const RawHeaders &headers, const std::string &body, int timeout_seconds,
                                        std::size_t max_body_bytes) {
	// Refusals: nothing has been sent, and retrying cannot help.
	std::string origin, target, host;
	int port = 0;
	if (!IsHttpsUrl(url) || !SplitAbsoluteUrl(url, origin, target) || !SplitHttpsOrigin(origin, host, port)) {
		throw InvalidInputException(
		    "sharepoint: refusing a pre-authenticated URL that is not a well-formed https URL");
	}
	std::string request;
	if (!BuildRawHttpRequest(method, host, port, target, headers, body, request)) {
		throw InvalidInputException("sharepoint: refusing to send a malformed request to %s", host);
	}
	if (timeout_seconds <= 0) {
		throw InvalidInputException("sharepoint: pre-authenticated request needs a positive timeout");
	}

	SSL_CTX *ctx = SharedContext();
	if (!ctx) {
		ThrowTransport("TLS setup", host);
	}
	// TCP first (bounded by a deadline, timeouts installed), THEN TLS on top, so
	// the handshake itself is bounded too.
	BioPtr conn = ConnectWithDeadline(host, port, timeout_seconds);
	BioPtr bio(BIO_new_ssl(ctx, 1));
	if (!bio) {
		ThrowTransport("TLS setup", host);
	}
	SSL *ssl = nullptr;
	BIO_get_ssl(bio.get(), &ssl);
	SSL_set_mode(ssl, SSL_MODE_AUTO_RETRY);
	// SNI, and verify the certificate is for THIS host (not merely trusted).
	SSL_set_tlsext_host_name(ssl, host.c_str());
	SSL_set1_host(ssl, host.c_str());
	BIO_push(bio.get(), conn.release()); // `bio` now owns the whole chain
	if (BIO_do_handshake(bio.get()) <= 0 || SSL_get_verify_result(ssl) != X509_V_OK) {
		ThrowTransport("TLS handshake / certificate verification", host);
	}

	std::size_t sent = 0;
	while (sent < request.size()) {
		const int chunk = static_cast<int>(std::min<std::size_t>(request.size() - sent, 1 << 20));
		const int n = BIO_write(bio.get(), request.data() + sent, chunk);
		if (n <= 0) {
			// Blocking socket + SSL_MODE_AUTO_RETRY: a "retry" here can only be
			// SO_SNDTIMEO firing, so it is a timeout, not a reason to spin.
			ThrowTransport(BIO_should_retry(bio.get()) ? "sending the request (timed out)" : "sending the request",
			               host);
		}
		sent += static_cast<std::size_t>(n);
	}

	// Read until the response is complete per its framing, or the peer closes.
	// Stopping at a complete framed response means a peer that ignores
	// `Connection: close` costs nothing instead of a full socket timeout.
	const std::size_t max_raw = max_body_bytes + kMaxHeadBytes;
	std::string raw;
	bool clean_eof = false;
	char buf[64 * 1024];
	while (true) {
		const int n = BIO_read(bio.get(), buf, sizeof(buf));
		if (n > 0) {
			raw.append(buf, static_cast<std::size_t>(n));
			if (raw.size() > max_raw) {
				throw IOException("sharepoint: response from %s exceeds %llu bytes (did the server ignore Range?)",
				                  host, (unsigned long long)max_body_bytes);
			}
			if (RawHttpResponseComplete(raw)) {
				break;
			}
			continue;
		}
		if (n < 0 && BIO_should_retry(bio.get())) {
			ThrowTransport("reading the response (timed out)", host); // SO_RCVTIMEO, see above
		}
		// 0 = orderly close (close_notify). <0 = the stream ended without one:
		// fine for a response whose framing says it is complete, never for an
		// unframed body, which could be silently truncated.
		clean_eof = (n == 0);
		break;
	}
	ERR_clear_error();

	RawHttpResponse response;
	if (!ParseRawHttpResponse(raw, response)) {
		throw IOException("sharepoint: malformed or truncated response from %s (%llu bytes)", host,
		                  (unsigned long long)raw.size());
	}
	const bool framed = !response.Header("Content-Length").empty() || !response.Header("Transfer-Encoding").empty();
	if (!framed && !clean_eof) {
		throw IOException("sharepoint: connection to %s ended without a complete response", host);
	}
	return response;
}

} // namespace sharepoint
} // namespace duckdb
