// Certificate credentials (#71): see sharepoint_certificate.hpp.
#include "sharepoint_certificate.hpp"
#include "sharepoint_assertion.hpp"

#include <openssl/bio.h>
#include <openssl/evp.h>
#include <openssl/pem.h>
#include <openssl/x509.h>

#include <memory>

namespace duckdb {
namespace sharepoint {

namespace {

struct BioFree {
	void operator()(BIO *b) const {
		BIO_free(b);
	}
};
struct X509Free {
	void operator()(X509 *x) const {
		X509_free(x);
	}
};
struct KeyFree {
	void operator()(EVP_PKEY *k) const {
		EVP_PKEY_free(k);
	}
};
struct CtxFree {
	void operator()(EVP_MD_CTX *c) const {
		EVP_MD_CTX_free(c);
	}
};

std::unique_ptr<BIO, BioFree> Bio(const std::string &pem) {
	return std::unique_ptr<BIO, BioFree>(BIO_new_mem_buf(pem.data(), static_cast<int>(pem.size())));
}

} // namespace

bool CertificateThumbprint(const std::string &pem, std::string &x5t, std::string &error) {
	auto bio = Bio(pem);
	// Finds the CERTIFICATE block wherever it is in the file.
	std::unique_ptr<X509, X509Free> cert(bio ? PEM_read_bio_X509(bio.get(), nullptr, nullptr, nullptr) : nullptr);
	if (!cert) {
		error = "no PEM certificate (-----BEGIN CERTIFICATE-----) in the file";
		return false;
	}
	unsigned char digest[EVP_MAX_MD_SIZE];
	unsigned int len = 0;
	if (X509_digest(cert.get(), EVP_sha1(), digest, &len) != 1) {
		error = "could not compute the certificate's thumbprint";
		return false;
	}
	x5t = Base64Url(std::string(reinterpret_cast<const char *>(digest), len));
	return true;
}

bool SignRs256(const std::string &pem, const std::string &password, const std::string &input,
               std::string &signature, std::string &error) {
	auto bio = Bio(pem);
	// With a NULL callback, OpenSSL takes `u` as the passphrase.
	std::unique_ptr<EVP_PKEY, KeyFree> key(
	    bio ? PEM_read_bio_PrivateKey(bio.get(), nullptr, nullptr, const_cast<char *>(password.c_str())) : nullptr);
	if (!key) {
		error = password.empty() ? "no readable private key in the file (if it is encrypted, set "
		                           "CERTIFICATE_PASSWORD)"
		                         : "no readable private key in the file, or CERTIFICATE_PASSWORD is wrong";
		return false;
	}
	if (EVP_PKEY_base_id(key.get()) != EVP_PKEY_RSA) {
		error = "the private key is not an RSA key (Entra signs certificate assertions with RS256)";
		return false;
	}
	std::unique_ptr<EVP_MD_CTX, CtxFree> ctx(EVP_MD_CTX_new());
	size_t len = 0;
	if (!ctx || EVP_DigestSignInit(ctx.get(), nullptr, EVP_sha256(), nullptr, key.get()) != 1 ||
	    EVP_DigestSignUpdate(ctx.get(), input.data(), input.size()) != 1 ||
	    EVP_DigestSignFinal(ctx.get(), nullptr, &len) != 1) {
		error = "signing the client assertion failed";
		return false;
	}
	signature.resize(len);
	if (EVP_DigestSignFinal(ctx.get(), reinterpret_cast<unsigned char *>(&signature[0]), &len) != 1) {
		error = "signing the client assertion failed";
		return false;
	}
	signature.resize(len);
	return true;
}

} // namespace sharepoint
} // namespace duckdb
