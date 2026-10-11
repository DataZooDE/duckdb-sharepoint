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
#include <poll.h>
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

using Clock = std::chrono::steady_clock;

int MillisLeft(Clock::time_point deadline) {
	const auto left = std::chrono::duration_cast<std::chrono::milliseconds>(deadline - Clock::now()).count();
	return left <= 0 ? 0 : static_cast<int>(std::min<long long>(left, 1LL << 30));
}

// Wait until a non-blocking connect on `fd` finished (writable, or an error)
// or the deadline passes, then report whether it SUCCEEDED (SO_ERROR == 0).
// poll(), not select(): select's FD_SET writes out of bounds for a descriptor
// >= FD_SETSIZE, and a busy DuckDB process can hold that many. On Windows a
// refused connect is signalled as an error event, which poll reports too.
bool WaitConnected(int fd, Clock::time_point deadline) {
	const int ms = MillisLeft(deadline);
	if (ms == 0) {
		return false;
	}
#ifdef _WIN32
	WSAPOLLFD pfd;
	pfd.fd = static_cast<SOCKET>(fd);
	pfd.events = POLLWRNORM;
	pfd.revents = 0;
	if (WSAPoll(&pfd, 1, ms) <= 0) {
		return false;
	}
#else
	struct pollfd pfd;
	pfd.fd = fd;
	pfd.events = POLLOUT;
	pfd.revents = 0;
	if (poll(&pfd, 1, ms) <= 0) {
		return false;
	}
#endif
	int so_error = 0;
#ifdef _WIN32
	int len = sizeof(so_error); // Winsock takes int*, not socklen_t*
	if (getsockopt(static_cast<SOCKET>(fd), SOL_SOCKET, SO_ERROR, reinterpret_cast<char *>(&so_error), &len) != 0) {
		return false;
	}
#else
	socklen_t len = sizeof(so_error);
	if (getsockopt(fd, SOL_SOCKET, SO_ERROR, &so_error, &len) != 0) {
		return false;
	}
#endif
	return so_error == 0;
}

// Install receive/send timeouts (milliseconds, at least 1). Fails CLOSED: a
// socket we cannot bound is a socket that can hang a DuckDB thread forever.
bool SetSocketTimeouts(int fd, int millis) {
	millis = std::max(millis, 1);
#ifdef _WIN32
	DWORD ms = static_cast<DWORD>(millis);
	return setsockopt(static_cast<SOCKET>(fd), SOL_SOCKET, SO_RCVTIMEO, reinterpret_cast<const char *>(&ms),
	                  sizeof(ms)) == 0 &&
	       setsockopt(static_cast<SOCKET>(fd), SOL_SOCKET, SO_SNDTIMEO, reinterpret_cast<const char *>(&ms),
	                  sizeof(ms)) == 0;
#else
	struct timeval tv;
	tv.tv_sec = millis / 1000;
	tv.tv_usec = (millis % 1000) * 1000;
	return setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv)) == 0 &&
	       setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv)) == 0;
#endif
}

//! Connecting gets at most this long, whatever the overall budget.
constexpr int kConnectTimeoutSeconds = 30;

// TCP connect to host:port by `deadline` (and within kConnectTimeoutSeconds),
// then switch the socket back to blocking with timeouts bounded by `deadline`.
BioPtr ConnectWithDeadline(const std::string &host, int port, Clock::time_point deadline, int &fd_out) {
	const std::string hostport = host + ":" + std::to_string(port);
	BioPtr conn(BIO_new_connect(hostport.c_str()));
	if (!conn) {
		ThrowTransport("socket setup", host);
	}
	BIO_set_nbio(conn.get(), 1);
	const auto connect_deadline = std::min(deadline, Clock::now() + std::chrono::seconds(kConnectTimeoutSeconds));
	while (BIO_do_connect(conn.get()) <= 0) {
		int fd = -1;
		if (!BIO_should_retry(conn.get()) || BIO_get_fd(conn.get(), &fd) < 0 || fd < 0) {
			ThrowTransport("connect", host);
		}
		if (!WaitConnected(fd, connect_deadline)) {
			ThrowTransport(Clock::now() >= connect_deadline ? "connect (timed out)" : "connect", host);
		}
	}
	int fd = -1;
	if (BIO_get_fd(conn.get(), &fd) < 0 || fd < 0 || !BIO_socket_nbio(fd, 0) ||
	    !SetSocketTimeouts(fd, MillisLeft(deadline))) {
		ThrowTransport("socket timeout setup", host);
	}
	BIO_set_nbio(conn.get(), 0);
	fd_out = fd;
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
	// One deadline for the whole exchange -- connect, handshake, send, receive
	// -- so a peer that trickles bytes cannot hold a worker indefinitely (a
	// per-read timeout alone resets on every byte).
	const auto deadline = Clock::now() + std::chrono::seconds(timeout_seconds);
	int fd = -1;
	// TCP first (bounded, timeouts installed), THEN TLS on top, so the
	// handshake itself is bounded too.
	BioPtr conn = ConnectWithDeadline(host, port, deadline, fd);
	BioPtr bio(BIO_new_ssl(ctx, 1));
	if (!bio) {
		ThrowTransport("TLS setup", host);
	}
	SSL *ssl = nullptr;
	BIO_get_ssl(bio.get(), &ssl);
	SSL_set_mode(ssl, SSL_MODE_AUTO_RETRY);
	// SNI, and verify the certificate is for THIS host (not merely trusted).
	// Fail closed: if hostname verification cannot be armed, any trusted
	// certificate would be accepted for any host.
	if (!ssl || SSL_set_tlsext_host_name(ssl, host.c_str()) != 1 || SSL_set1_host(ssl, host.c_str()) != 1) {
		ThrowTransport("TLS hostname verification setup", host);
	}
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
		const int left = MillisLeft(deadline);
		if (left == 0 || !SetSocketTimeouts(fd, left)) {
			ThrowTransport("reading the response (timed out)", host);
		}
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
		// Usually a connection that ended (reset, or timed out on Windows, where
		// a receive timeout is not reported as "retry") mid-response.
		throw IOException("sharepoint: incomplete or malformed response from %s (%llu bytes received)", host,
		                  (unsigned long long)raw.size());
	}
	const bool framed = !response.Header("Content-Length").empty() || !response.Header("Transfer-Encoding").empty();
	if (!framed && !clean_eof) {
		throw IOException("sharepoint: connection to %s ended (or timed out) before the response completed",
		                  host);
	}
	return response;
}

} // namespace sharepoint
} // namespace duckdb
