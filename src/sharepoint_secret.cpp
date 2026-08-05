// The `sharepoint` secret type.
//
// Redaction: every field that can carry secret material -- access_token,
// refresh_token, client_secret -- is added to `redact_keys`, so
// `SELECT * FROM duckdb_secrets()` never displays it. tenant_id, client_id,
// drive_id and site_id are identifiers, not secrets, so they are shown.
#include "sharepoint_secret.hpp"

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

void ApplyDefaultScope(KeyValueSecret &result) {
	// Named "graph_scope", NOT "scope", for the reason the sibling gdrive
	// extension paid to learn: DuckDB's CREATE SECRET grammar hard-codes SCOPE
	// as a top-level clause, so an option literally named `scope` is diverted
	// into the secret's PATH-matching prefix list and never reaches
	// input.options -- silently rescoping the secret so it matches no
	// sharepoint:// path at all. `graph_scope` sidesteps the collision.
	if (result.secret_map.find("graph_scope") == result.secret_map.end()) {
		result.secret_map["graph_scope"] = Value(std::string(SCOPE_GRAPH_DEFAULT));
	}
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
unique_ptr<BaseSecret> CreateClientCredentialsSecret(ClientContext &, CreateSecretInput &input) {
	auto result = make_uniq<KeyValueSecret>(input.scope, input.type, input.provider, input.name);

	CopyOption(input, *result, "tenant_id");
	CopyOption(input, *result, "client_id");
	CopyOption(input, *result, "client_secret");
	CopyOption(input, *result, "drive_id");
	CopyOption(input, *result, "site_id");
	CopyOption(input, *result, "graph_scope");

	for (const char *required : {"tenant_id", "client_id", "client_secret"}) {
		if (!HasNonEmptyOption(input, required)) {
			throw InvalidInputException(
			    "sharepoint secret (PROVIDER client_credentials) needs TENANT_ID, CLIENT_ID and "
			    "CLIENT_SECRET. Register an app in Entra ID, grant it the Sites.ReadWrite.All (or "
			    "Files.ReadWrite.All) APPLICATION permission with admin consent, then:\n"
			    "  CREATE SECRET sp (TYPE sharepoint, PROVIDER client_credentials,\n"
			    "                    TENANT_ID '...', CLIENT_ID '...', CLIENT_SECRET '...',\n"
			    "                    DRIVE_ID 'b!...');");
		}
	}

	ApplyDefaultScope(*result);
	result->redact_keys.insert("client_secret");
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
	CopyOption(input, *result, "drive_id");
	CopyOption(input, *result, "site_id");
	CopyOption(input, *result, "graph_scope");

	if (!HasNonEmptyOption(input, "access_token") && !HasNonEmptyOption(input, "refresh_token")) {
		throw InvalidInputException(
		    "sharepoint secret (PROVIDER config) needs at least ACCESS_TOKEN or REFRESH_TOKEN. Example:\n"
		    "  CREATE SECRET sp_tok (TYPE sharepoint, PROVIDER config, ACCESS_TOKEN '...');");
	}
	// A REFRESH_TOKEN with no ACCESS_TOKEN needs TENANT_ID + CLIENT_ID +
	// CLIENT_SECRET to mint a fresh access token (Azure AD's
	// grant_type=refresh_token POST needs them). Catch it here, with no
	// network call, rather than on first use.
	if (!HasNonEmptyOption(input, "access_token") &&
	    (!HasNonEmptyOption(input, "tenant_id") || !HasNonEmptyOption(input, "client_id") ||
	     !HasNonEmptyOption(input, "client_secret"))) {
		throw InvalidInputException(
		    "sharepoint secret (PROVIDER config) has REFRESH_TOKEN but no ACCESS_TOKEN; refreshing needs "
		    "TENANT_ID, CLIENT_ID and CLIENT_SECRET too.");
	}

	ApplyDefaultScope(*result);
	result->redact_keys.insert("access_token");
	result->redact_keys.insert("refresh_token");
	result->redact_keys.insert("client_secret");
	return std::move(result);
}

void RegisterCommonParameters(CreateSecretFunction &function) {
	// drive_id: a Graph drive id (a document library). site_id: a Graph site
	// id, from which the default document library's drive is resolved when
	// drive_id is not given. Exactly one is expected.
	function.named_parameters["drive_id"] = LogicalType(LogicalTypeId::VARCHAR);
	function.named_parameters["site_id"] = LogicalType(LogicalTypeId::VARCHAR);
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

	CreateSecretFunction config_fn = {kSecretTypeName, "config", CreateConfigSecret, {}};
	config_fn.named_parameters["access_token"] = LogicalType(LogicalTypeId::VARCHAR);
	config_fn.named_parameters["refresh_token"] = LogicalType(LogicalTypeId::VARCHAR);
	config_fn.named_parameters["tenant_id"] = LogicalType(LogicalTypeId::VARCHAR);
	config_fn.named_parameters["client_id"] = LogicalType(LogicalTypeId::VARCHAR);
	config_fn.named_parameters["client_secret"] = LogicalType(LogicalTypeId::VARCHAR);
	RegisterCommonParameters(config_fn);
	loader.RegisterFunction(config_fn);
}

} // namespace sharepoint
} // namespace duckdb
