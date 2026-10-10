// TLS transport for Graph's pre-authenticated URLs. The request bytes come from
// the pure BuildRawHttpRequest and the response is parsed by the pure
// ParseRawHttpResponse; this file only moves bytes over a verified TLS
// connection. Proven by the live SQL suite (reads go through the /content
// download redirect, multi-chunk writes through the uploadUrl).
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
#include <sys/socket.h>
#include <sys/time.h>
#endif

#include <algorithm>
#include <cstdint>
#include <memory>
#include <mutex>

namespace duckdb {
namespace sharepoint {

namespace {

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

void SetSocketTimeout(BIO *bio, int seconds) {
	int fd = -1;
	if (BIO_get_fd(bio, &fd) < 0 || fd < 0) {
		return;
	}
#ifdef _WIN32
	DWORD ms = static_cast<DWORD>(seconds) * 1000;
	setsockopt(static_cast<SOCKET>(fd), SOL_SOCKET, SO_RCVTIMEO, reinterpret_cast<const char *>(&ms), sizeof(ms));
	setsockopt(static_cast<SOCKET>(fd), SOL_SOCKET, SO_SNDTIMEO, reinterpret_cast<const char *>(&ms), sizeof(ms));
#else
	struct timeval tv;
	tv.tv_sec = seconds;
	tv.tv_usec = 0;
	setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
	setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));
#endif
}

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

} // namespace

RawHttpResponse PreauthenticatedRequest(const std::string &method, const std::string &url,
                                        const RawHeaders &headers, const std::string &body,
                                        int timeout_seconds) {
	std::string origin, target, host;
	int port = 0;
	if (!IsHttpsUrl(url) || !SplitAbsoluteUrl(url, origin, target) || !SplitHttpsOrigin(origin, host, port)) {
		throw IOException("sharepoint: refusing a pre-authenticated URL that is not a well-formed https URL");
	}
	std::string request;
	if (!BuildRawHttpRequest(method, host, target, headers, body, request)) {
		throw IOException("sharepoint: refusing to send a malformed request to %s", host);
	}

	SSL_CTX *ctx = SharedContext();
	if (!ctx) {
		ThrowTransport("TLS setup", host);
	}
	std::unique_ptr<BIO, BioDeleter> bio(BIO_new_ssl_connect(ctx));
	if (!bio) {
		ThrowTransport("TLS setup", host);
	}
	SSL *ssl = nullptr;
	BIO_get_ssl(bio.get(), &ssl);
	SSL_set_mode(ssl, SSL_MODE_AUTO_RETRY);
	// SNI, and verify the certificate is for THIS host (not merely trusted).
	SSL_set_tlsext_host_name(ssl, host.c_str());
	SSL_set1_host(ssl, host.c_str());
	const std::string hostport = host + ":" + std::to_string(port);
	BIO_set_conn_hostname(bio.get(), hostport.c_str());

	if (BIO_do_connect(bio.get()) <= 0) {
		ThrowTransport("connect", host);
	}
	SetSocketTimeout(bio.get(), timeout_seconds);
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

	// `Connection: close`: the server ends the stream after the response.
	std::string raw;
	char buf[64 * 1024];
	while (true) {
		const int n = BIO_read(bio.get(), buf, sizeof(buf));
		if (n > 0) {
			raw.append(buf, static_cast<std::size_t>(n));
			continue;
		}
		if (n < 0 && BIO_should_retry(bio.get())) {
			ThrowTransport("reading the response (timed out)", host); // SO_RCVTIMEO, see above
		}
		break; // 0 = clean close; <0 = EOF without close_notify, which many servers do
	}
	ERR_clear_error();

	RawHttpResponse response;
	if (!ParseRawHttpResponse(raw, response)) {
		throw IOException("sharepoint: malformed or truncated response from %s (%llu bytes)", host,
		                  (unsigned long long)raw.size());
	}
	return response;
}

} // namespace sharepoint
} // namespace duckdb
