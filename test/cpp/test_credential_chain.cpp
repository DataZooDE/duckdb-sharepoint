// PROVIDER credential_chain (#72): which sources apply, in which order, and
// how each one's token reply reads. The requests themselves are the e2e test.
#include "sharepoint_credential_chain.hpp"

#include <catch2/catch_test_macros.hpp>

#include <string>

using namespace duckdb::sharepoint;

TEST_CASE("ChainOrder follows DefaultAzureCredential, skipping what is not configured", "[chain]") {
	ChainEnv env;
	REQUIRE(ChainOrder(env) == std::vector<ChainSource> {ChainSource::IMDS, ChainSource::AZURE_CLI});
	env.identity_endpoint = "http://localhost:42356/msi/token";
	env.identity_header = "h";
	REQUIRE(ChainOrder(env) == std::vector<ChainSource> {ChainSource::APP_SERVICE, ChainSource::AZURE_CLI});
	env.federated_token_file = "/var/run/secrets/azure/tokens/azure-identity-token";
	env.client_id = "c";
	env.tenant_id = "t";
	REQUIRE(ChainOrder(env) == std::vector<ChainSource> {ChainSource::WORKLOAD_IDENTITY, ChainSource::APP_SERVICE,
	                                                       ChainSource::AZURE_CLI});
	env.tenant_id.clear(); // workload identity needs all three
	REQUIRE(ChainOrder(env).front() == ChainSource::APP_SERVICE);
}

TEST_CASE("GraphResource: the resource a managed identity is asked for", "[chain]") {
	REQUIRE(GraphResource("https://graph.microsoft.com") == "https://graph.microsoft.com");
	REQUIRE(GraphResource("https://graph.microsoft.us/") == "https://graph.microsoft.us");
}

TEST_CASE("ManagedIdentityQuery: IMDS and App Service forms, user-assigned optional", "[chain]") {
	REQUIRE(ManagedIdentityQuery(ChainSource::IMDS, "https://graph.microsoft.com", "") ==
	        "api-version=2018-02-01&resource=https%3A%2F%2Fgraph.microsoft.com");
	REQUIRE(ManagedIdentityQuery(ChainSource::APP_SERVICE, "https://graph.microsoft.com", "abc") ==
	        "api-version=2019-08-01&resource=https%3A%2F%2Fgraph.microsoft.com&client_id=abc");
}

TEST_CASE("ParseManagedIdentityReply: expires_in or expires_on, numbers or strings", "[chain]") {
	auto imds = ParseManagedIdentityReply(200, R"({"access_token":"tok","expires_in":"3599","expires_on":"1700003599"})",
	                                      1700000000);
	REQUIRE(imds.ok);
	REQUIRE(imds.token == "tok");
	REQUIRE(imds.expires_on == 1700003599);
	auto app = ParseManagedIdentityReply(200, R"({"access_token":"t2","expires_on":1700001000})", 1700000000);
	REQUIRE(app.expires_on == 1700001000);
	auto in_only = ParseManagedIdentityReply(200, R"({"access_token":"t3","expires_in":600})", 1700000000);
	REQUIRE(in_only.expires_on == 1700000600);
	auto none = ParseManagedIdentityReply(400, R"({"error":"invalid_request","error_description":"Identity not found"})",
	                                      0);
	REQUIRE_FALSE(none.ok);
	REQUIRE(none.error.find("Identity not found") != std::string::npos);
	REQUIRE(none.error.find("tok") == std::string::npos);
}

TEST_CASE("ParseAzureCliReply: az account get-access-token output", "[chain]") {
	auto r = ParseAzureCliReply(
	    R"({"accessToken":"cli","expiresOn":"2026-11-03 20:15:00.000000","expires_on":1793736900,"tokenType":"Bearer"})",
	    1793730000);
	REQUIRE(r.ok);
	REQUIRE(r.token == "cli");
	REQUIRE(r.expires_on == 1793736900);
	// Older az: no expires_on -> a short, safe lifetime.
	auto old = ParseAzureCliReply(R"({"accessToken":"cli2","expiresOn":"2026-11-03 20:15:00.000000"})", 1000);
	REQUIRE(old.ok);
	REQUIRE(old.expires_on == 1000 + 300);
	auto bad = ParseAzureCliReply("ERROR: Please run 'az login' to setup account.", 0);
	REQUIRE_FALSE(bad.ok);
	REQUIRE(bad.error.find("az login") != std::string::npos);
}
