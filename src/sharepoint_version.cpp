#include "sharepoint_version.hpp"

namespace duckdb {
namespace sharepoint {

const char *SharepointVersionCString() {
	// CalVer, vYYYY.MM.DD, matching the tag and the sibling DataZoo extensions
	// (gdrive, erpl, anofox-*, quack_oauth). Kept in step with the tag by
	// scripts/check_extension_stamp.sh, which fails the build if HEAD is on a
	// tag and this string is not it.
	return "2026.10.18";
}

std::string SharepointVersion() {
	return SharepointVersionCString();
}

} // namespace sharepoint
} // namespace duckdb
