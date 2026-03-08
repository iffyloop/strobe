#include "pch.h"

#include "sg_plugins.h"

#include "io_utils.h"

namespace {

constexpr char const* k_plugins_dir = "shaders/sg_plugins";
constexpr char const* k_raymarch_template_file = "shaders/raymarch_template.frag";

sg_plugins_t g_plugins;
bool g_plugins_initialized = false;

std::string sg_read_text_file(std::string const& path) {
	std::ifstream in(path, std::ios::binary | std::ios::ate);
	if (!in) {
		return "";
	}
	std::streamsize const bytes = in.tellg();
	if (bytes < 0) {
		return "";
	}
	std::string out;
	out.resize(static_cast<size_t>(bytes));
	in.seekg(0, std::ios::beg);
	if (bytes > 0) {
		in.read(out.data(), bytes);
	}
	return out;
}

sg_plugin_category_t sg_parse_category(std::string const& s) {
	if (s == "combine") {
		return sg_plugin_category_t::COMBINE;
	}
	if (s == "primitive") {
		return sg_plugin_category_t::PRIMITIVE;
	}
	if (s == "effect") {
		return sg_plugin_category_t::EFFECT;
	}
	return sg_plugin_category_t::INVALID;
}

bool sg_parse_plugin_manifest(
	std::string const& manifest_path, nlohmann::json const& json, sg_plugin_def_t& out_def, std::string& out_error) {
	if (!json.contains("id") || !json["id"].is_string()) {
		out_error = "missing string field 'id' in " + manifest_path;
		return false;
	}
	if (!json.contains("category") || !json["category"].is_string()) {
		out_error = "missing string field 'category' in " + manifest_path;
		return false;
	}
	if (!json.contains("function") || !json["function"].is_string()) {
		out_error = "missing string field 'function' in " + manifest_path;
		return false;
	}

	out_def.id = json["id"].get<std::string>();
	out_def.display_name = json.value("display_name", out_def.id);
	out_def.function_name = json["function"].get<std::string>();
	out_def.category = sg_parse_category(json["category"].get<std::string>());
	out_def.default_on_nodes = json.value("default_on_nodes", false);

	if (out_def.category == sg_plugin_category_t::INVALID) {
		out_error = "invalid category for plugin '" + out_def.id + "' in " + manifest_path;
		return false;
	}

	std::string glsl_file = json.value("glsl", out_def.id + ".glsl");
	std::filesystem::path manifest_fs_path(manifest_path);
	std::filesystem::path glsl_path = manifest_fs_path.parent_path() / glsl_file;
	out_def.glsl_path = glsl_path.lexically_normal().string();
	out_def.glsl_source = sg_read_text_file(out_def.glsl_path);
	if (out_def.glsl_source.empty()) {
		out_error = "failed to read GLSL source for plugin '" + out_def.id + "' at " + out_def.glsl_path;
		return false;
	}

	out_def.params.clear();
	if (json.contains("params")) {
		if (!json["params"].is_array()) {
			out_error = "'params' must be an array in " + manifest_path;
			return false;
		}
		for (auto const& p : json["params"]) {
			if (!p.is_object() || !p.contains("name") || !p["name"].is_string()) {
				out_error = "each param must have string field 'name' in " + manifest_path;
				return false;
			}

			sg_plugin_param_t param;
			param.name = p["name"].get<std::string>();
			param.default_value = p.value("default", 0.0f);
			param.min_value = p.value("min", -std::numeric_limits<f32>::infinity());
			param.max_value = p.value("max", std::numeric_limits<f32>::infinity());
			param.drag_speed = p.value("drag_speed", 0.01f);
			out_def.params.emplace_back(std::move(param));
		}
	}

	return true;
}

bool sg_load_plugin_manifests(sg_plugins_t& out_plugins, std::string& out_error) {
	out_plugins = sg_plugins_t();

	std::string const base_dir = get_resources_prefix() + std::string(k_plugins_dir);
	std::filesystem::path plugins_dir(base_dir);
	if (!std::filesystem::exists(plugins_dir) || !std::filesystem::is_directory(plugins_dir)) {
		out_error = "plugin directory missing: " + plugins_dir.string();
		return false;
	}

	std::vector<std::filesystem::path> manifest_paths;
	for (auto const& entry : std::filesystem::directory_iterator(plugins_dir)) {
		if (!entry.is_regular_file()) {
			continue;
		}
		if (entry.path().extension() == ".json") {
			manifest_paths.emplace_back(entry.path());
		}
	}
	std::sort(manifest_paths.begin(), manifest_paths.end());

	if (manifest_paths.empty()) {
		out_error = "no plugin manifests found in " + plugins_dir.string();
		return false;
	}

	for (auto const& manifest_path : manifest_paths) {
		std::string const manifest_data = sg_read_text_file(manifest_path.string());
		if (manifest_data.empty()) {
			out_error = "failed to read manifest: " + manifest_path.string();
			return false;
		}

		nlohmann::json json;
		try {
			json = nlohmann::json::parse(manifest_data);
		} catch (std::exception const& e) {
			out_error = "invalid JSON in " + manifest_path.string() + ": " + e.what();
			return false;
		}

		sg_plugin_def_t def;
		if (!sg_parse_plugin_manifest(manifest_path.string(), json, def, out_error)) {
			return false;
		}

		switch (def.category) {
		case sg_plugin_category_t::COMBINE:
			out_plugins.combine_defs.emplace_back(std::move(def));
			break;
		case sg_plugin_category_t::PRIMITIVE:
			out_plugins.primitive_defs.emplace_back(std::move(def));
			break;
		case sg_plugin_category_t::EFFECT:
			out_plugins.effect_defs.emplace_back(std::move(def));
			break;
		default:
			out_error = "invalid plugin category in " + manifest_path.string();
			return false;
		}
	}

	if (out_plugins.combine_defs.empty()) {
		out_error = "at least one combine plugin is required";
		return false;
	}
	if (out_plugins.primitive_defs.empty()) {
		out_error = "at least one primitive plugin is required";
		return false;
	}

	s32 next_runtime_id = 1;

	auto assign_ids = [&](std::vector<sg_plugin_def_t>& defs, std::unordered_map<std::string, s32>& id_map,
				      char const* category_name) {
		std::sort(defs.begin(), defs.end(), [](sg_plugin_def_t const& a, sg_plugin_def_t const& b) {
			return a.id < b.id;
		});

		id_map.clear();
		for (auto& def : defs) {
			if (id_map.contains(def.id)) {
				out_error = "duplicate " + std::string(category_name) + " plugin id: " + def.id;
				return false;
			}
			def.runtime_id = next_runtime_id;
			id_map.emplace(def.id, next_runtime_id);
			next_runtime_id += 1;
		}
		return true;
	};

	if (!assign_ids(out_plugins.combine_defs, out_plugins.combine_id_to_runtime_id, "combine")) {
		return false;
	}
	if (!assign_ids(out_plugins.primitive_defs, out_plugins.primitive_id_to_runtime_id, "primitive")) {
		return false;
	}
	if (!assign_ids(out_plugins.effect_defs, out_plugins.effect_id_to_runtime_id, "effect")) {
		return false;
	}

	return true;
}

void sg_build_dispatch_case(std::ostringstream& out, s32 runtime_id, std::string const& body) {
	out << "\tif (op_id == " << runtime_id << ") {\n";
	out << "\t\t" << body << "\n";
	out << "\t}\n";
}

} // namespace

void sg_plugins_init_or_die() {
	if (g_plugins_initialized) {
		return;
	}

	sg_plugins_t candidate;
	std::string error;
	if (!sg_plugins_load_candidate(candidate, error)) {
		std::cerr << "[sg_plugins] failed to initialize plugins: " << error << std::endl;
		assert_release(false);
	}

	sg_plugins_commit(std::move(candidate));
	g_plugins_initialized = true;
}

sg_plugins_t const& sg_plugins_get() {
	return g_plugins;
}

void sg_plugins_commit(sg_plugins_t&& plugins) {
	plugins.generation = g_plugins.generation + 1;
	g_plugins = std::move(plugins);
	g_plugins_initialized = true;
}

bool sg_plugins_load_candidate(sg_plugins_t& out_plugins, std::string& out_error) {
	return sg_load_plugin_manifests(out_plugins, out_error);
}

bool sg_plugins_build_raymarch_fragment_source(
	sg_plugins_t const& plugins, std::string& out_src, std::string& out_error) {
	std::string const template_path = get_resources_prefix() + std::string(k_raymarch_template_file);
	std::string template_src = sg_read_text_file(template_path);
	if (template_src.empty()) {
		out_error = "failed to read template shader: " + template_path;
		return false;
	}

	std::ostringstream plugin_functions;
	plugin_functions << "\n";
	auto append_plugin_sources = [&plugin_functions](std::vector<sg_plugin_def_t> const& defs, char const* category) {
		for (auto const& def : defs) {
			plugin_functions << "// plugin: " << category << "/" << def.id << "\n";
			plugin_functions << def.glsl_source << "\n\n";
		}
	};
	append_plugin_sources(plugins.combine_defs, "combine");
	append_plugin_sources(plugins.primitive_defs, "primitive");
	append_plugin_sources(plugins.effect_defs, "effect");

	std::ostringstream primitive_dispatch;
	primitive_dispatch << "float sg_eval_primitive_plugin(int op_id, vec3 p, vec4 params) {\n";
	for (auto const& def : plugins.primitive_defs) {
		sg_build_dispatch_case(
			primitive_dispatch, def.runtime_id, "return " + def.function_name + "(p, params);");
	}
	primitive_dispatch << "\treturn 1e6;\n";
	primitive_dispatch << "}\n";

	std::ostringstream combine_dispatch;
	combine_dispatch << "float sg_eval_combine_plugin(int op_id, float a, float b, vec4 params) {\n";
	for (auto const& def : plugins.combine_defs) {
		sg_build_dispatch_case(
			combine_dispatch, def.runtime_id, "return " + def.function_name + "(a, b, params);");
	}
	combine_dispatch << "\treturn a;\n";
	combine_dispatch << "}\n";

	std::ostringstream effect_dispatch;
	effect_dispatch << "vec3 sg_apply_effect_plugin(int op_id, vec3 p, vec4 params) {\n";
	for (auto const& def : plugins.effect_defs) {
		sg_build_dispatch_case(effect_dispatch, def.runtime_id, "return " + def.function_name + "(p, params);");
	}
	effect_dispatch << "\treturn p;\n";
	effect_dispatch << "}\n";

	auto replace_marker = [&](std::string const& marker, std::string const& replacement) {
		size_t pos = template_src.find(marker);
		if (pos == std::string::npos) {
			out_error = "template shader missing marker: " + marker;
			return false;
		}
		template_src.replace(pos, marker.size(), replacement);
		return true;
	};

	if (!replace_marker("/*__SG_PLUGIN_FUNCTIONS__*/", plugin_functions.str())) {
		return false;
	}
	if (!replace_marker("/*__SG_PLUGIN_PRIMITIVE_DISPATCH__*/", primitive_dispatch.str())) {
		return false;
	}
	if (!replace_marker("/*__SG_PLUGIN_COMBINE_DISPATCH__*/", combine_dispatch.str())) {
		return false;
	}
	if (!replace_marker("/*__SG_PLUGIN_EFFECT_DISPATCH__*/", effect_dispatch.str())) {
		return false;
	}

	out_src = std::move(template_src);
	return true;
}

sg_plugin_def_t const* sg_plugins_find_combine_by_runtime_id(s32 runtime_id) {
	for (auto const& def : g_plugins.combine_defs) {
		if (def.runtime_id == runtime_id) {
			return &def;
		}
	}
	return nullptr;
}

sg_plugin_def_t const* sg_plugins_find_primitive_by_runtime_id(s32 runtime_id) {
	for (auto const& def : g_plugins.primitive_defs) {
		if (def.runtime_id == runtime_id) {
			return &def;
		}
	}
	return nullptr;
}

sg_plugin_def_t const* sg_plugins_find_effect_by_runtime_id(s32 runtime_id) {
	for (auto const& def : g_plugins.effect_defs) {
		if (def.runtime_id == runtime_id) {
			return &def;
		}
	}
	return nullptr;
}

sg_plugin_def_t const* sg_plugins_find_combine_by_id(std::string const& id) {
	auto it = g_plugins.combine_id_to_runtime_id.find(id);
	if (it == g_plugins.combine_id_to_runtime_id.end()) {
		return nullptr;
	}
	return sg_plugins_find_combine_by_runtime_id(it->second);
}

sg_plugin_def_t const* sg_plugins_find_primitive_by_id(std::string const& id) {
	auto it = g_plugins.primitive_id_to_runtime_id.find(id);
	if (it == g_plugins.primitive_id_to_runtime_id.end()) {
		return nullptr;
	}
	return sg_plugins_find_primitive_by_runtime_id(it->second);
}

sg_plugin_def_t const* sg_plugins_find_effect_by_id(std::string const& id) {
	auto it = g_plugins.effect_id_to_runtime_id.find(id);
	if (it == g_plugins.effect_id_to_runtime_id.end()) {
		return nullptr;
	}
	return sg_plugins_find_effect_by_runtime_id(it->second);
}

sg_plugin_def_t const* sg_plugins_find_node_def_by_runtime_id(s32 runtime_id) {
	if (auto const* combine_def = sg_plugins_find_combine_by_runtime_id(runtime_id)) {
		return combine_def;
	}
	return sg_plugins_find_primitive_by_runtime_id(runtime_id);
}

sg_plugin_def_t const* sg_plugins_find_node_def_by_id(std::string const& id) {
	if (auto const* combine_def = sg_plugins_find_combine_by_id(id)) {
		return combine_def;
	}
	return sg_plugins_find_primitive_by_id(id);
}

std::vector<sg_plugin_def_t const*> sg_plugins_list_node_defs() {
	std::vector<sg_plugin_def_t const*> defs;
	defs.reserve(g_plugins.combine_defs.size() + g_plugins.primitive_defs.size());
	for (auto const& def : g_plugins.primitive_defs) {
		defs.emplace_back(&def);
	}
	for (auto const& def : g_plugins.combine_defs) {
		defs.emplace_back(&def);
	}
	std::sort(defs.begin(), defs.end(), [](sg_plugin_def_t const* a, sg_plugin_def_t const* b) {
		if (a->category != b->category) {
			if (a->category == sg_plugin_category_t::PRIMITIVE) {
				return true;
			}
			if (b->category == sg_plugin_category_t::PRIMITIVE) {
				return false;
			}
			return static_cast<s32>(a->category) < static_cast<s32>(b->category);
		}
		return a->display_name < b->display_name;
	});
	return defs;
}

std::vector<sg_plugin_def_t const*> sg_plugins_list_effect_defs() {
	std::vector<sg_plugin_def_t const*> defs;
	defs.reserve(g_plugins.effect_defs.size());
	for (auto const& def : g_plugins.effect_defs) {
		defs.emplace_back(&def);
	}
	std::sort(defs.begin(), defs.end(), [](sg_plugin_def_t const* a, sg_plugin_def_t const* b) {
		return a->display_name < b->display_name;
	});
	return defs;
}
