#pragma once

#include "duckdb/main/extension.hpp"

namespace duckdb {

class ExtensionLoader;

class SharepointExtension : public Extension {
public:
	void Load(ExtensionLoader &loader) override;
	string Name() override;
	string Version() const override;
};

} // namespace duckdb
