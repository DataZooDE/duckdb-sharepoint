#define DUCKDB_EXTENSION_MAIN

#include "sharepoint_extension.hpp"
#include "sharepoint_banner.hpp"
#include "sharepoint_filesystem.hpp"
#include "sharepoint_stats.hpp"
#include "sharepoint_version.hpp"
#include "sharepoint_vfs_functions.hpp"

#ifndef EMSCRIPTEN
#include "telemetry.hpp"
#endif

#include "duckdb/main/config.hpp"
#include "duckdb/parser/parsed_data/create_scalar_function_info.hpp"
#include "duckdb/main/database.hpp"
#include "duckdb/main/extension/extension_loader.hpp"
#include "duckdb/function/scalar_function.hpp"

// The version comes from SharepointVersionCString(), not a literal: a copied
// literal is what left the sibling gdrive's banner announcing "0.1.0".
const datazoo::BannerInfo SHAREPOINT_BANNER {"sharepoint", duckdb::sharepoint::SharepointVersionCString(),
                                             "https://github.com/DataZooDE/duckdb-sharepoint"};

namespace duckdb {

namespace sharepoint {
// Defined in sharepoint_secret.cpp.
void RegisterSharepointSecrets(ExtensionLoader &loader);
} // namespace sharepoint

static void SharepointVersionScalar(DataChunk &args, ExpressionState &state, Vector &result) {
	result.SetVectorType(VectorType::CONSTANT_VECTOR);
	auto version = sharepoint::SharepointVersion();
	ConstantVector::GetData<string_t>(result)[0] = StringVector::AddString(result, version);
}

static void LoadInternal(ExtensionLoader &loader) {
#ifndef EMSCRIPTEN
	// Anonymous usage telemetry, as in the sibling DataZoo extensions. Opt out
	// with `SET sharepoint_telemetry_enabled = false` or the environment
	// variable DATAZOO_DISABLE_TELEMETRY.
	PostHogTelemetry::Instance().SetAPIKey("phc_t3wwRLtpyEmLHYaZCSszG0MqVr74J6wnCrj9D41zk2t");
	PostHogTelemetry::Instance().SetProduct("sharepoint", sharepoint::SharepointVersion(), "oss");
	PostHogTelemetry::Instance().AssociateGroup("deployment", PostHogTelemetry::GetDistinctId());
	PostHogTelemetry::Instance().CaptureExtensionLoad("sharepoint", sharepoint::SharepointVersion());
	loader.GetDatabaseInstance().config.AddExtensionOption(
	    "sharepoint_telemetry_enabled", "Enable or disable anonymous usage telemetry for the sharepoint extension.",
	    LogicalType::BOOLEAN, Value::BOOLEAN(true), [](ClientContext &, SetScope, Value &parameter) {
		    PostHogTelemetry::Instance().SetEnabled(BooleanValue::Get(parameter));
	    });
#endif

	{
		CreateScalarFunctionInfo info(
		    ScalarFunction("sharepoint_version", {}, LogicalType::VARCHAR, SharepointVersionScalar));
		FunctionDescription d;
		d.description = "Returns the version of the loaded sharepoint extension.";
		d.examples = {"SELECT sharepoint_version()"};
		d.categories = {"sharepoint", "meta"};
		info.descriptions.push_back(std::move(d));
		loader.RegisterFunction(std::move(info));
	}

	// The extension point: everything layered on DuckDB's virtual filesystem
	// -- read_parquet, read_csv, COPY, glob, ATTACH of a DuckLake DATA_PATH --
	// inherits sharepoint:// from here without knowing it exists. Registered
	// once; the filesystem object spans every ClientContext for the life of
	// the DatabaseInstance.
	loader.GetDatabaseInstance().GetFileSystem().RegisterSubSystem(make_uniq<sharepoint::SharepointFileSystem>());

	// RemoveFile sends the item to the SharePoint/OneDrive recycle bin by
	// default (recoverable, the safer default). Opt into permanent deletion.
	loader.GetDatabaseInstance().config.AddExtensionOption(
	    "sharepoint_permanent_delete",
	    "RemoveFile permanently deletes instead of moving to the recycle bin (default: false).",
	    LogicalType::BOOLEAN, Value::BOOLEAN(false));

	// The Microsoft Graph base URL. Overridable for sovereign/national clouds
	// (e.g. https://graph.microsoft.us for GCC-High, or a Graph proxy in
	// tests). Default is the global cloud.
	loader.GetDatabaseInstance().config.AddExtensionOption(
	    "sharepoint_graph_endpoint",
	    "Base URL for Microsoft Graph (default https://graph.microsoft.com). Set for national clouds "
	    "such as https://graph.microsoft.us or https://microsoftgraph.chinacloudapi.cn.",
	    LogicalType::VARCHAR, Value("https://graph.microsoft.com"));

	// The Azure AD login authority. National clouds use a different host.
	loader.GetDatabaseInstance().config.AddExtensionOption(
	    "sharepoint_login_endpoint",
	    "Base URL for the Azure AD login authority (default https://login.microsoftonline.com). Set for "
	    "national clouds, e.g. https://login.microsoftonline.us.",
	    LogicalType::VARCHAR, Value("https://login.microsoftonline.com"));

	// Streaming writes: a write handle holds at most one chunk in memory. A
	// file up to this size is uploaded in a single PUT on close; a larger one
	// streams through a deferCommit upload session (createUploadSession), one
	// chunk at a time, as it is written. Graph requires session chunks to be a
	// multiple of 320 KiB; the default (10 MiB) is a multiple.
	loader.GetDatabaseInstance().config.AddExtensionOption(
	    "sharepoint_upload_chunk_bytes",
	    "Upload chunk size, and the most memory one file being written holds (default 10 MiB; rounded "
	    "down to a multiple of 320 KiB and capped below 60 MiB per the Graph upload-session contract). "
	    "Files up to this size upload in one PUT on close; larger ones stream through a resumable "
	    "upload session as they are written.",
	    LogicalType::UBIGINT, Value::UBIGINT(10ULL * 1024 * 1024));

	// Like gdrive's block cache: a ranged read pays a round trip regardless of
	// size, so a larger block amortises fewer requests. 0 disables and reads
	// exact ranges. Served by SharepointBlockCache (sharepoint_blockcache.*).
	loader.GetDatabaseInstance().config.AddExtensionOption(
	    "sharepoint_block_size_bytes",
	    "Block size for cached reads (default 8 MiB, at most 64 MiB; 0 disables the cache and reads "
	    "exact ranges).",
	    LogicalType::UBIGINT, Value::UBIGINT(8ULL * 1024 * 1024));

	// Suppresses the per-open metadata refresh for prefixes whose files are
	// never rewritten in place (a DuckLake/Iceberg/Delta DATA_PATH). Declaring
	// a prefix whose files ARE rewritten causes stale reads -- opt-in only.
	loader.GetDatabaseInstance().config.AddExtensionOption(
	    "sharepoint_immutable_prefixes",
	    "Comma-separated sharepoint:// path prefixes whose files are never overwritten in place "
	    "(e.g. a DuckLake DATA_PATH). Skips the per-open metadata refresh for them. Matching is on "
	    "whole path segments. Empty (the default) disables it.",
	    LogicalType::VARCHAR, Value(""));

	// Testing aid: cap how long a minted token is trusted, so token expiry
	// mid-handle can be exercised in seconds instead of an hour. 0 = Entra
	// ID's own lifetime. Not for production use.
	loader.GetDatabaseInstance().config.AddExtensionOption(
	    "sharepoint_debug_token_max_age_seconds",
	    "TESTING ONLY: treat access tokens as expiring after at most this many seconds (0 = their real "
	    "lifetime).",
	    LogicalType::BIGINT, Value::BIGINT(0));

	sharepoint::RegisterSharepointSecrets(loader);

	// write_blob()/remove_file()/move_file()/file_size(): the FileSystem
	// operations SQL never got. Scheme-generic -- they know nothing about
	// Graph and dispatch on the path's scheme, so loading this extension gives
	// them to s3://, gs:// and local paths too. See sharepoint_vfs_functions.hpp
	// for the one caveat: do not load this AND duckdb-gdrive into the same
	// process, since both register the same four names.
	sharepoint::RegisterVfsFunctions(loader);
	sharepoint::RegisterSharepointStats(loader);

	datazoo::RegisterBannerOption(loader);
	// Last, so a load that fails earlier never advertises itself. Silent unless
	// stderr is a terminal and the once-a-day stamp has expired.
	datazoo::ShowBanner(SHAREPOINT_BANNER);
}

void SharepointExtension::Load(ExtensionLoader &loader) {
	LoadInternal(loader);
}

string SharepointExtension::Name() {
	return "sharepoint";
}

string SharepointExtension::Version() const {
	return sharepoint::SharepointVersion();
}

} // namespace duckdb

extern "C" {

DUCKDB_CPP_EXTENSION_ENTRY(sharepoint, loader) {
	duckdb::LoadInternal(loader);
}
}
