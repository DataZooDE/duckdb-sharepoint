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
//!
//! The interactive delegated flows (authorization_code / device_code) are a
//! documented follow-up: they need the datazoo-oauth2 submodule the sibling
//! gdrive uses, which this scaffold does not yet wire in.
void RegisterSharepointSecrets(ExtensionLoader &loader);

} // namespace sharepoint
} // namespace duckdb
