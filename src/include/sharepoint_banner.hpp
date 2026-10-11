#pragma once

#include "datazoo_banner_duckdb.hpp"

// Shared identity for the load banner and the issue-link error footer, as in
// the sibling gdrive. The user-facing SharepointFileSystem methods wrap their
// bodies in DATAZOO_GUARDED_BLOCK (the surface is a FileSystem, not registered
// function pointers, so DATAZOO_GUARD does not apply).
//
// Defined in sharepoint_extension.cpp.
extern const datazoo::BannerInfo SHAREPOINT_BANNER;
