#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace duckdb {
namespace sharepoint {

// ---------------------------------------------------------------------------
// PROVIDER credential_chain (#72): an Azure workload's own identity, no
// secret in DuckDB. No duckdb.hpp and no I/O; the requests are made by
// sharepoint_auth.cpp.
// ---------------------------------------------------------------------------

//! The sources, tried in this order (as Azure's DefaultAzureCredential):
enum class ChainSource {
	WORKLOAD_IDENTITY, //!< AKS / federated: AZURE_FEDERATED_TOKEN_FILE + AZURE_CLIENT_ID + AZURE_TENANT_ID
	APP_SERVICE,       //!< App Service, Functions, Container Apps: IDENTITY_ENDPOINT + IDENTITY_HEADER
	IMDS,              //!< a VM's (or VM scale set's) managed identity: 169.254.169.254
	AZURE_CLI          //!< `az account get-access-token`, e.g. a developer's machine
};

const char *ChainSourceName(ChainSource source);

//! What decides the chain, read from the environment.
struct ChainEnv {
	std::string federated_token_file; // AZURE_FEDERATED_TOKEN_FILE
	std::string client_id;            // AZURE_CLIENT_ID
	std::string tenant_id;            // AZURE_TENANT_ID
	std::string identity_endpoint;    // IDENTITY_ENDPOINT
	std::string identity_header;      // IDENTITY_HEADER
};

//! The sources that apply, in order. App Service and IMDS are alternatives:
//! where IDENTITY_ENDPOINT is set, IMDS is not the managed identity.
std::vector<ChainSource> ChainOrder(const ChainEnv &env);

//! The resource a managed identity or az is asked for: the Graph endpoint,
//! without a trailing slash.
std::string GraphResource(const std::string &graph_endpoint);

//! The query string of a managed identity token request; `client_id` picks a
//! user-assigned identity ("" for the system-assigned one).
std::string ManagedIdentityQuery(ChainSource source, const std::string &resource, const std::string &client_id);

struct ChainToken {
	bool ok = false;
	std::string token;
	int64_t expires_on = 0; // seconds since 1970
	std::string error;      // why not, with no token in it
};

//! An IMDS or App Service reply.
ChainToken ParseManagedIdentityReply(int status, const std::string &body, int64_t now);

//! `az account get-access-token --output json`; anything else is the CLI's
//! error text. Without expires_on (old az), the token is kept 5 minutes.
ChainToken ParseAzureCliReply(const std::string &output, int64_t now);

} // namespace sharepoint
} // namespace duckdb
