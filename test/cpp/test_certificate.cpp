// Certificate credentials (#71): thumbprint and RS256 signature over a
// certificate made here, verified with its public key. Links OpenSSL, so it
// is in the main build's unit tests, not the standalone pure-logic binary.
#include "sharepoint_assertion.hpp"
#include "sharepoint_certificate.hpp"

#include <catch2/catch_test_macros.hpp>

#include <openssl/evp.h>
#include <openssl/pem.h>
#include <openssl/rsa.h>
#include <openssl/x509.h>

#include <string>

using namespace duckdb::sharepoint;

namespace {

std::string ToPem(BIO *bio) {
	char *data = nullptr;
	const long len = BIO_get_mem_data(bio, &data);
	return std::string(data, static_cast<size_t>(len));
}

//! A self-signed certificate and its key, as one PEM file holds them.
struct TestCert {
	EVP_PKEY *key = nullptr;
	X509 *cert = nullptr;
	std::string pem;           // certificate, then the key in the clear
	std::string encrypted_pem; // key encrypted with "pw", then the certificate

	TestCert() {
		key = EVP_RSA_gen(2048);
		cert = X509_new();
		X509_set_version(cert, 2);
		ASN1_INTEGER_set(X509_get_serialNumber(cert), 1);
		X509_gmtime_adj(X509_getm_notBefore(cert), 0);
		X509_gmtime_adj(X509_getm_notAfter(cert), 3600);
		X509_set_pubkey(cert, key);
		X509_NAME_add_entry_by_txt(X509_get_subject_name(cert), "CN", MBSTRING_ASC,
		                           reinterpret_cast<const unsigned char *>("test"), -1, -1, 0);
		X509_set_issuer_name(cert, X509_get_subject_name(cert));
		X509_sign(cert, key, EVP_sha256());

		BIO *a = BIO_new(BIO_s_mem());
		PEM_write_bio_X509(a, cert);
		PEM_write_bio_PrivateKey(a, key, nullptr, nullptr, 0, nullptr, nullptr);
		pem = ToPem(a);
		BIO_free(a);

		BIO *b = BIO_new(BIO_s_mem());
		PEM_write_bio_PKCS8PrivateKey(b, key, EVP_aes_256_cbc(), nullptr, 0, nullptr, const_cast<char *>("pw"));
		PEM_write_bio_X509(b, cert);
		encrypted_pem = ToPem(b);
		BIO_free(b);
	}
	~TestCert() {
		X509_free(cert);
		EVP_PKEY_free(key);
	}

	bool Verifies(const std::string &input, const std::string &signature) const {
		EVP_MD_CTX *ctx = EVP_MD_CTX_new();
		const bool ok = EVP_DigestVerifyInit(ctx, nullptr, EVP_sha256(), nullptr, key) == 1 &&
		                EVP_DigestVerify(ctx, reinterpret_cast<const unsigned char *>(signature.data()),
		                                 signature.size(), reinterpret_cast<const unsigned char *>(input.data()),
		                                 input.size()) == 1;
		EVP_MD_CTX_free(ctx);
		return ok;
	}
};

} // namespace

TEST_CASE("CertificateThumbprint is the base64url SHA-1 of the DER certificate", "[certificate]") {
	TestCert t;
	unsigned char digest[EVP_MAX_MD_SIZE];
	unsigned int len = 0;
	REQUIRE(X509_digest(t.cert, EVP_sha1(), digest, &len) == 1);
	std::string x5t, error;
	REQUIRE(CertificateThumbprint(t.pem, x5t, error));
	REQUIRE(x5t == Base64Url(std::string(reinterpret_cast<const char *>(digest), len)));
	REQUIRE(x5t.size() == 27); // 20 bytes, unpadded
	// Wherever the certificate is in the file.
	std::string x5t2;
	REQUIRE(CertificateThumbprint(t.encrypted_pem, x5t2, error));
	REQUIRE(x5t2 == x5t);
	REQUIRE_FALSE(CertificateThumbprint("not a pem", x5t, error));
}

TEST_CASE("SignRs256 signs with the file's key, clear or encrypted", "[certificate]") {
	TestCert t;
	const std::string input = ClientAssertionSigningInput("app", "aud", "x5t", "jti", 1700000000);
	std::string signature, error;
	REQUIRE(SignRs256(t.pem, "", input, signature, error));
	REQUIRE(signature.size() == 256);
	REQUIRE(t.Verifies(input, signature));

	REQUIRE(SignRs256(t.encrypted_pem, "pw", input, signature, error));
	REQUIRE(t.Verifies(input, signature));

	REQUIRE_FALSE(SignRs256(t.encrypted_pem, "wrong", input, signature, error));
	REQUIRE(error.find("CERTIFICATE_PASSWORD") != std::string::npos);
	REQUIRE(error.find("pw") == std::string::npos); // nothing secret in the message
	REQUIRE_FALSE(SignRs256(t.encrypted_pem, "", input, signature, error));
}
