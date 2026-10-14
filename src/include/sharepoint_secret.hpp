#pragma once

#include <string>

namespace duckdb {

class ExtensionLoader;

namespace sharepoint {

// The secret type name and its field keys, shared between registration
// (sharepoint_secret.cpp) and consumption (sharepoint_auth.cpp).
constexpr const char *kSecretTypeName = "sharepoint";

//! Registers the `sharepoint` secret type and its creation providers:
//!   PROVIDER client_credentials (default) -- TENANT_ID + CLIENT_ID +
//!       CLIENT_SECRET, the unattended app-only flow (Azure AD app
//!       registration with Sites.ReadWrite.All / Files.ReadWrite.All
//!       application permission + admin consent).
//!   PROVIDER config -- paste a pre-obtained ACCESS_TOKEN (and optionally a
//!       REFRESH_TOKEN + CLIENT_ID/CLIENT_SECRET/TENANT_ID to refresh it).
//!   PROVIDER device_code -- delegated (user) auth for headless shells: prints
//!       a URL + code at first use (RFC 8628), public client, no secret.
//!   PROVIDER authorization_code -- delegated auth via the browser and a
//!       localhost redirect (PKCE), public client, no secret.
//! Both store the resulting refresh token back into the secret.
void RegisterSharepointSecrets(ExtensionLoader &loader);

} // namespace sharepoint
} // namespace duckdb
