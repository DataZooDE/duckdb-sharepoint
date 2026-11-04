#include "sharepoint_permissions.hpp"

#include <picojson.h>

namespace duckdb {
namespace sharepoint {

namespace {

const picojson::object *Obj(const picojson::object &o, const char *key) {
	auto it = o.find(key);
	return it != o.end() && it->second.is<picojson::object>() ? &it->second.get<picojson::object>() : nullptr;
}

std::string Str(const picojson::object &o, const char *key) {
	auto it = o.find(key);
	return it != o.end() && it->second.is<std::string>() ? it->second.get<std::string>() : std::string();
}

struct Grantee {
	std::string type, name, email;
};

// An identitySet (grantedToV2 or one of grantedToIdentitiesV2): who it is.
// Several kinds may be set at once (siteUser beside user); the first of this
// order wins.
bool GranteeOf(const picojson::object &set, Grantee &out) {
	static const struct {
		const char *key;
		const char *type;
	} kinds[] = {{"user", "user"},           {"group", "group"},           {"application", "application"},
	             {"siteGroup", "site_group"}, {"siteUser", "site_user"},   {"device", "device"}};
	for (auto &kind : kinds) {
		if (auto who = Obj(set, kind.key)) {
			out.type = kind.type;
			out.name = Str(*who, "displayName");
			out.email = Str(*who, "email");
			return true;
		}
	}
	return false;
}

} // namespace

bool ParsePermissions(const std::string &body, std::vector<PermissionRow> &out, std::string &next_link,
                      std::string &error) {
	picojson::value v;
	const auto parse_error = picojson::parse(v, body);
	if (!parse_error.empty() || !v.is<picojson::object>()) {
		error = "not a JSON object";
		return false;
	}
	const auto &page = v.get<picojson::object>();
	auto values = page.find("value");
	if (values == page.end() || !values->second.is<picojson::array>()) {
		error = "no 'value' array";
		return false;
	}
	next_link = Str(page, "@odata.nextLink");
	for (auto &p : values->second.get<picojson::array>()) {
		if (!p.is<picojson::object>()) {
			continue;
		}
		const auto &perm = p.get<picojson::object>();
		PermissionRow base;
		base.permission_id = Str(perm, "id");
		auto roles = perm.find("roles");
		if (roles != perm.end() && roles->second.is<picojson::array>()) {
			for (auto &r : roles->second.get<picojson::array>()) {
				if (r.is<std::string>()) {
					base.roles.push_back(r.get<std::string>());
				}
			}
		}
		base.expires = Str(perm, "expirationDateTime");
		if (auto inherited = Obj(perm, "inheritedFrom")) {
			base.inherited_from = Str(*inherited, "path");
			if (base.inherited_from.empty()) {
				base.inherited_from = Str(*inherited, "id");
			}
		}
		const auto link = Obj(perm, "link");
		if (link) {
			base.link_type = Str(*link, "type");
			base.link_scope = Str(*link, "scope");
			base.link_url = Str(*link, "webUrl");
		}
		base.granted_by = link ? "link" : (Obj(perm, "inheritedFrom") ? "inherited" : "direct");

		std::vector<Grantee> grantees;
		Grantee g;
		if (auto to = Obj(perm, "grantedToV2")) {
			if (GranteeOf(*to, g)) {
				grantees.push_back(g);
			}
		}
		auto many = perm.find("grantedToIdentitiesV2");
		if (many != perm.end() && many->second.is<picojson::array>()) {
			for (auto &set : many->second.get<picojson::array>()) {
				if (set.is<picojson::object>() && GranteeOf(set.get<picojson::object>(), g)) {
					grantees.push_back(g);
				}
			}
		}
		if (grantees.empty()) {
			out.push_back(base); // a link nobody has used, say: still a way in
			continue;
		}
		for (auto &who : grantees) {
			auto row = base;
			row.grantee_type = who.type;
			row.grantee = who.name;
			row.grantee_email = who.email;
			out.push_back(std::move(row));
		}
	}
	return true;
}

} // namespace sharepoint
} // namespace duckdb
