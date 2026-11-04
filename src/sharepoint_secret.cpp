// The `sharepoint` secret type.
//
// Redaction: every field that can carry secret material -- access_token,
// refresh_token, client_secret -- is added to `redact_keys`, so
// `SELECT * FROM duckdb_secrets()` never displays it. tenant_id, client_id,
// drive_id and site_id are identifiers, not secrets, so they are shown.
#include "sharepoint_secret.hpp"
#include "sharepoint_delegated.hpp"
#include "sharepoint_weburl.hpp"

#include "duckdb/common/exception.hpp"
#include "duckdb/main/extension/extension_loader.hpp"
#include "duckdb/main/secret/secret_manager.hpp"

namespace duckdb {
namespace sharepoint {

namespace {

void CopyOption(CreateSecretInput &input, KeyValueSecret &result, const char *key) {
	auto it = input.options.find(key);
	if (it != input.options.end()) {
		result.secret_map[key] = it->second;
	}
}

bool HasNonEmptyOption(const CreateSecretInput &input, const char *key) {
	auto it = input.options.find(key);
	return it != input.options.end() && !it->second.ToString().empty();
}

// ---------------------------------------------------------------------------
// PROVIDER client_credentials -- unattended app-only auth (the default).
//
// An Azure AD app registration with a client secret and admin-consented
// APPLICATION permissions (Sites.ReadWrite.All or Files.ReadWrite.All). This
// is the escurel/daemon path: no user, no browser. Validated for presence
// here; the token is not fetched until first use (GetAuthContext), so CREATE
// SECRET makes no network call and a rotated secret needs no recreation.
// ---------------------------------------------------------------------------
// Where the paths hang off: DRIVE_ID, or SITE_ID / SITE_URL (the site's
// default library, or DRIVE by name), resolved at first use. SITE_URL is the
// address from the browser (#95); it is checked here, offline.
void CopyLocationOptions(CreateSecretInput &input, KeyValueSecret &result) {
	CopyOption(input, result, "drive_id");
	CopyOption(input, result, "site_id");
	CopyOption(input, result, "site_url");
	CopyOption(input, result, "drive");
	if (HasNonEmptyOption(input, "site_url")) {
		const auto site_url = input.options.find("site_url")->second.ToString();
		const auto parsed = ParseSharepointWebUrl(site_url);
		if (!parsed.ok || parsed.url.kind != WebUrlKind::DIRECT) {
			throw InvalidInputException("sharepoint secret: SITE_URL '%s' is not a site address; copy it from the "
			                            "browser, e.g. 'https://contoso.sharepoint.com/sites/Sales'",
			                            site_url);
		}
	}
	if (HasNonEmptyOption(input, "drive") && !HasNonEmptyOption(input, "site_url") &&
	    !HasNonEmptyOption(input, "site_id")) {
		throw InvalidInputException("sharepoint secret: DRIVE names a document library of a site; give the site "
		                            "too (SITE_URL or SITE_ID)");
	}
}

unique_ptr<BaseSecret> CreateClientCredentialsSecret(ClientContext &, CreateSecretInput &input) {
	auto result = make_uniq<KeyValueSecret>(input.scope, input.type, input.provider, input.name);

	CopyOption(input, *result, "tenant_id");
	CopyOption(input, *result, "client_id");
	CopyOption(input, *result, "client_secret");
	CopyLocationOptions(input, *result);
	// Named GRAPH_SCOPE, NOT SCOPE: DuckDB's CREATE SECRET grammar hard-codes
	// SCOPE as the secret's path-prefix list, so an option of that name never
	// reaches input.options. Left unset, the scope defaults at use time to
	// "<graph endpoint>/.default", i.e. the right cloud (DefaultGraphScope).
	CopyOption(input, *result, "graph_scope");

	for (const char *required : {"tenant_id", "client_id", "client_secret"}) {
		if (!HasNonEmptyOption(input, required)) {
			throw InvalidInputException(
			    "sharepoint secret (PROVIDER client_credentials) needs TENANT_ID, CLIENT_ID and "
			    "CLIENT_SECRET. Register an app in Entra ID, grant it the Sites.ReadWrite.All (or "
			    "Files.ReadWrite.All) APPLICATION permission with admin consent, then:\n"
			    "  CREATE SECRET sp (TYPE sharepoint, PROVIDER client_credentials,\n"
			    "                    TENANT_ID '...', CLIENT_ID '...', CLIENT_SECRET '...',\n"
			    "                    SITE_URL 'https://contoso.sharepoint.com/sites/Sales');");
		}
	}

	result->redact_keys.insert("client_secret");
	return std::move(result);
}

// ---------------------------------------------------------------------------
// PROVIDER client_certificate -- app-only auth with a certificate (#71).
//
// The same app registration as client_credentials, with a certificate
// uploaded under *Certificates & secrets* instead of a client secret. The
// token request carries a JWT signed with the certificate's key (see
// sharepoint_assertion.hpp). CERTIFICATE_PATH is a local PEM file holding the
// certificate and its RSA private key; CERTIFICATE_PASSWORD decrypts an
// encrypted key. The file is read when the secret is used, so a renewed
// certificate in the same place needs no new secret.
// ---------------------------------------------------------------------------
unique_ptr<BaseSecret> CreateClientCertificateSecret(ClientContext &, CreateSecretInput &input) {
	auto result = make_uniq<KeyValueSecret>(input.scope, input.type, input.provider, input.name);

	CopyOption(input, *result, "tenant_id");
	CopyOption(input, *result, "client_id");
	CopyOption(input, *result, "certificate_path");
	CopyOption(input, *result, "certificate_password");
	CopyLocationOptions(input, *result);
	CopyOption(input, *result, "graph_scope");

	for (const char *required : {"tenant_id", "client_id", "certificate_path"}) {
		if (!HasNonEmptyOption(input, required)) {
			throw InvalidInputException(
			    "sharepoint secret (PROVIDER client_certificate) needs TENANT_ID, CLIENT_ID and "
			    "CERTIFICATE_PATH, a PEM file with the certificate and its private key. Upload the "
			    "certificate to the app registration (Certificates & secrets), then:\n"
			    "  CREATE SECRET sp (TYPE sharepoint, PROVIDER client_certificate,\n"
			    "                    TENANT_ID '...', CLIENT_ID '...', CERTIFICATE_PATH '/etc/sp/app.pem',\n"
			    "                    SITE_URL 'https://contoso.sharepoint.com/sites/Sales');");
		}
	}

	result->redact_keys.insert("certificate_password");
	return std::move(result);
}

// ---------------------------------------------------------------------------
// PROVIDER credential_chain -- the workload's own Azure identity (#72): no
// credential in the secret. Workload identity, then a managed identity (App
// Service or IMDS), then the Azure CLI, as DefaultAzureCredential tries them.
// CLIENT_ID picks a user-assigned managed identity.
// ---------------------------------------------------------------------------
unique_ptr<BaseSecret> CreateCredentialChainSecret(ClientContext &, CreateSecretInput &input) {
	auto result = make_uniq<KeyValueSecret>(input.scope, input.type, input.provider, input.name);
	CopyOption(input, *result, "client_id");
	CopyLocationOptions(input, *result);
	CopyOption(input, *result, "graph_scope");
	if (!HasNonEmptyOption(input, "drive_id") && !HasNonEmptyOption(input, "site_id") &&
	    !HasNonEmptyOption(input, "site_url")) {
		throw InvalidInputException("sharepoint secret (PROVIDER credential_chain) needs SITE_URL, DRIVE_ID or SITE_ID:\n"
		                            "  CREATE SECRET sp (TYPE sharepoint, PROVIDER credential_chain, SITE_URL 'https://contoso.sharepoint.com/sites/Sales');");
	}
	return std::move(result);
}

// ---------------------------------------------------------------------------
// PROVIDER config -- pre-obtained tokens, pasted in.
// ---------------------------------------------------------------------------
unique_ptr<BaseSecret> CreateConfigSecret(ClientContext &, CreateSecretInput &input) {
	auto result = make_uniq<KeyValueSecret>(input.scope, input.type, input.provider, input.name);

	CopyOption(input, *result, "access_token");
	CopyOption(input, *result, "refresh_token");
	CopyOption(input, *result, "tenant_id");
	CopyOption(input, *result, "client_id");
	CopyOption(input, *result, "client_secret");
	CopyLocationOptions(input, *result);
	CopyOption(input, *result, "graph_scope");

	if (!HasNonEmptyOption(input, "access_token") && !HasNonEmptyOption(input, "refresh_token")) {
		throw InvalidInputException(
		    "sharepoint secret (PROVIDER config) needs at least ACCESS_TOKEN or REFRESH_TOKEN. Example:\n"
		    "  CREATE SECRET sp_tok (TYPE sharepoint, PROVIDER config, ACCESS_TOKEN '...');");
	}
	// A REFRESH_TOKEN with no ACCESS_TOKEN needs TENANT_ID + CLIENT_ID to mint
	// a fresh access token (Entra ID's grant_type=refresh_token POST). Catch it
	// here, with no network call, rather than on first use. CLIENT_SECRET is
	// optional: a delegated token from a PUBLIC client (device code,
	// scripts/delegated_login.py) refreshes without one -- and must not send one.
	if (!HasNonEmptyOption(input, "access_token") &&
	    (!HasNonEmptyOption(input, "tenant_id") || !HasNonEmptyOption(input, "client_id"))) {
		throw InvalidInputException(
		    "sharepoint secret (PROVIDER config) has REFRESH_TOKEN but no ACCESS_TOKEN; refreshing needs "
		    "TENANT_ID and CLIENT_ID too (and CLIENT_SECRET for a confidential client).");
	}

	result->redact_keys.insert("access_token");
	result->redact_keys.insert("refresh_token");
	result->redact_keys.insert("client_secret");
	return std::move(result);
}

// ---------------------------------------------------------------------------
// PROVIDER device_code / authorization_code -- delegated (user) auth.
//
// A PUBLIC client: TENANT_ID + CLIENT_ID of an Entra app with "Allow public
// client flows" enabled (device code) or an http://localhost "Mobile and
// desktop applications" redirect (authorization code), and delegated
// Files.ReadWrite.All / Sites.ReadWrite.All / offline_access consented. There is
// no CLIENT_SECRET option: a public client never holds one.
//
// Creation stores configuration ONLY -- the sign-in runs at first use (as in
// gdrive), so CREATE SECRET never blocks on a human and is testable offline.
// After the sign-in the refresh token is written back into the secret (on disk
// for a PERSISTENT one), so the prompt happens once, not hourly. Supplying
// REFRESH_TOKEN skips the sign-in entirely.
// ---------------------------------------------------------------------------
void CopyDelegatedOptions(CreateSecretInput &input, KeyValueSecret &result, const char *provider) {
	CopyOption(input, result, "tenant_id");
	CopyOption(input, result, "client_id");
	CopyOption(input, result, "refresh_token");
	CopyLocationOptions(input, result);
	CopyOption(input, result, "graph_scope");
	if (!HasNonEmptyOption(input, "tenant_id") || !HasNonEmptyOption(input, "client_id")) {
		throw InvalidInputException(
		    "sharepoint secret (PROVIDER %s) needs TENANT_ID and CLIENT_ID of an Entra app registration with "
		    "public client flows enabled and the delegated Files.ReadWrite.All, Sites.ReadWrite.All and "
		    "offline_access permissions, e.g.\n"
		    "  CREATE SECRET sp_user (TYPE sharepoint, PROVIDER %s,\n"
		    "                         TENANT_ID '...', CLIENT_ID '...', SITE_URL 'https://contoso.sharepoint.com/sites/Sales');",
		    provider, provider);
	}
	// Redacted from the outset: the refresh token is written back after the
	// sign-in, and there must be no window in which it could be displayed.
	result.redact_keys.insert("refresh_token");
	result.redact_keys.insert("refresh_token_lineage");
	result.redact_keys.insert("access_token");
}

unique_ptr<BaseSecret> CreateDeviceCodeSecret(ClientContext &, CreateSecretInput &input) {
	auto result = make_uniq<KeyValueSecret>(input.scope, input.type, input.provider, input.name);
	CopyDelegatedOptions(input, *result, "device_code");
	return std::move(result);
}

unique_ptr<BaseSecret> CreateAuthorizationCodeSecret(ClientContext &, CreateSecretInput &input) {
	auto result = make_uniq<KeyValueSecret>(input.scope, input.type, input.provider, input.name);
	CopyDelegatedOptions(input, *result, "authorization_code");
	CopyOption(input, *result, "redirect_port");
	auto it = input.options.find("redirect_port");
	int port = 0;
	if (it != input.options.end() && !ParseRedirectPort(it->second.ToString(), port)) {
		throw InvalidInputException("sharepoint secret (PROVIDER authorization_code): REDIRECT_PORT must be a port "
		                            "between 1024 and 65535 (default 8020)");
	}
	return std::move(result);
}

void RegisterCommonParameters(CreateSecretFunction &function) {
	// drive_id: a Graph drive id (a document library). site_id: a Graph site
	// id, from which the default document library's drive is resolved when
	// drive_id is not given. Exactly one is expected.
	function.named_parameters["drive_id"] = LogicalType(LogicalTypeId::VARCHAR);
	function.named_parameters["site_id"] = LogicalType(LogicalTypeId::VARCHAR);
	// site_url: the site's address from the browser; drive: one of its
	// document libraries by name (default: the site's default library).
	function.named_parameters["site_url"] = LogicalType(LogicalTypeId::VARCHAR);
	function.named_parameters["drive"] = LogicalType(LogicalTypeId::VARCHAR);
	function.named_parameters["graph_scope"] = LogicalType(LogicalTypeId::VARCHAR);
}

} // namespace

void RegisterSharepointSecrets(ExtensionLoader &loader) {
	SecretType type;
	type.name = kSecretTypeName;
	type.deserializer = KeyValueSecret::Deserialize<KeyValueSecret>;
	// The unattended flow is the default: `CREATE SECRET (TYPE sharepoint,
	// PROVIDER client_credentials, ...)` is the headless daemon path escurel
	// uses.
	type.default_provider = "client_credentials";
	loader.RegisterSecretType(type);

	CreateSecretFunction cc_fn = {kSecretTypeName, "client_credentials", CreateClientCredentialsSecret, {}};
	cc_fn.named_parameters["tenant_id"] = LogicalType(LogicalTypeId::VARCHAR);
	cc_fn.named_parameters["client_id"] = LogicalType(LogicalTypeId::VARCHAR);
	cc_fn.named_parameters["client_secret"] = LogicalType(LogicalTypeId::VARCHAR);
	RegisterCommonParameters(cc_fn);
	loader.RegisterFunction(cc_fn);

	CreateSecretFunction cert_fn = {kSecretTypeName, "client_certificate", CreateClientCertificateSecret, {}};
	cert_fn.named_parameters["tenant_id"] = LogicalType(LogicalTypeId::VARCHAR);
	cert_fn.named_parameters["client_id"] = LogicalType(LogicalTypeId::VARCHAR);
	cert_fn.named_parameters["certificate_path"] = LogicalType(LogicalTypeId::VARCHAR);
	cert_fn.named_parameters["certificate_password"] = LogicalType(LogicalTypeId::VARCHAR);
	RegisterCommonParameters(cert_fn);
	loader.RegisterFunction(cert_fn);

	CreateSecretFunction chain_fn = {kSecretTypeName, "credential_chain", CreateCredentialChainSecret, {}};
	chain_fn.named_parameters["client_id"] = LogicalType(LogicalTypeId::VARCHAR);
	RegisterCommonParameters(chain_fn);
	loader.RegisterFunction(chain_fn);

	CreateSecretFunction config_fn = {kSecretTypeName, "config", CreateConfigSecret, {}};
	config_fn.named_parameters["access_token"] = LogicalType(LogicalTypeId::VARCHAR);
	config_fn.named_parameters["refresh_token"] = LogicalType(LogicalTypeId::VARCHAR);
	config_fn.named_parameters["tenant_id"] = LogicalType(LogicalTypeId::VARCHAR);
	config_fn.named_parameters["client_id"] = LogicalType(LogicalTypeId::VARCHAR);
	config_fn.named_parameters["client_secret"] = LogicalType(LogicalTypeId::VARCHAR);
	RegisterCommonParameters(config_fn);
	loader.RegisterFunction(config_fn);

	// Delegated (user) auth. No client_secret parameter: a public client.
	for (auto provider : {"device_code", "authorization_code"}) {
		CreateSecretFunction fn = {kSecretTypeName, provider,
		                           string(provider) == "device_code" ? CreateDeviceCodeSecret
		                                                             : CreateAuthorizationCodeSecret,
		                           {}};
		fn.named_parameters["tenant_id"] = LogicalType(LogicalTypeId::VARCHAR);
		fn.named_parameters["client_id"] = LogicalType(LogicalTypeId::VARCHAR);
		fn.named_parameters["refresh_token"] = LogicalType(LogicalTypeId::VARCHAR);
		if (string(provider) == "authorization_code") {
			fn.named_parameters["redirect_port"] = LogicalType(LogicalTypeId::VARCHAR);
		}
		RegisterCommonParameters(fn);
		loader.RegisterFunction(fn);
	}
}

} // namespace sharepoint
} // namespace duckdb
