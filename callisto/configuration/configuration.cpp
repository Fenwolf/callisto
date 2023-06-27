#include "configuration.h"

namespace callisto {
	Configuration::Configuration(const ConfigFileMap& config_file_map, const VariableFileMap& variable_file_map, 
		const fs::path& callisto_root_directory, bool allow_user_input) : allow_user_input(allow_user_input) {

		const auto user_variables{ parseVariableFiles(variable_file_map) };
		const auto config_files{ parseConfigFiles(config_file_map) };

		discoverProjectRoot(config_files, user_variables, callisto_root_directory);
		discoverLogFilePath(config_files, user_variables);
		discoverGenericTools(config_files, user_variables);
		discoverToolDirectories(config_files, user_variables);
		discoverEmulators(config_files, user_variables);

		fillInConfiguration(config_files, user_variables);

		finalizeBuildOrder();
	}

	void Configuration::verifyPatchGlobuleExclusivity() {
		if (patches.isSet() && globules.isSet()) {
			for (const auto& patch : patches.getOrThrow()) {
				for (const auto& globule : globules.getOrThrow()) {
					if (patch == globule) {
						throw ConfigException(fmt::format(
							"{} cannot be used as a globule and patch at the same time",
							patch.string()
						));
					}
				}
			}
		}
	}

	void Configuration::verifyGlobuleExclusivity() {
		if (globules.isSet()) {
			std::unordered_set<std::string> seen_file_names{};
			for (const auto& globule : globules.getOrThrow()) {
				const auto file_name{ globule.filename().string() };
				if (seen_file_names.count(file_name) != 0) {
					throw ConfigException(fmt::format(
						"Multiple globules called {} exist, but globule names must be unique",
						file_name
					));
				}
				else {
					seen_file_names.insert(file_name);
				}
			}
		}
	}

	void Configuration::verifyPatchUniqueness() {
		if (patches.isSet()) {
			std::unordered_set<fs::path> seen_files{};
			for (const auto& patch : patches.getOrThrow()) {
				if (seen_files.count(patch) != 0) {
					throw ConfigException(fmt::format(
						"Patch '{}' present multiple times in patches list",
						fs::relative(patch, project_root.getOrThrow()).string()
					));
				}
				else {
					seen_files.insert(patch);
				}
			}
		}
	}

	void Configuration::finalizeBuildOrder() {
		verifyGlobuleExclusivity();
		verifyPatchGlobuleExclusivity();
		verifyPatchUniqueness();

		for (const auto& symbol : _build_order.getOrDefault({})) {
			const auto descriptors{ symbolToDescriptor(symbol) };
			build_order.insert(build_order.end(), descriptors.begin(), descriptors.end());
		}
	}

	bool Configuration::trySet(StringConfigVariable& variable, const toml::value& table, 
		ConfigurationLevel level, const std::map<std::string, std::string>& user_variable_map){
		bool res{ variable.trySet(table, level, user_variable_map) };

		if (res) {
			key_val_map[variable.name] = variable.getOrThrow();
		}

		return res;
	}

	bool Configuration::trySet(PathConfigVariable& variable, const toml::value& table, ConfigurationLevel level, 
		const PathConfigVariable& relative_to, const std::map<std::string, std::string>& user_variable_map) {
		bool res{ variable.trySet(table, level, relative_to, user_variable_map) };

		if (res) {
			key_val_map[variable.name] = variable.getOrThrow().string();
		}

		return res;
	}

	bool Configuration::trySet(BoolConfigVariable& variable, const toml::value& table, ConfigurationLevel level) {
		bool res{ variable.trySet(table, level) };

		if (res) {
			key_val_map[variable.name] = variable.getOrThrow();
		}

		return res;
	}

	std::variant<std::monostate, std::string, bool> Configuration::getByKey(const std::string& key) const {
		if (key_val_map.find(key) != key_val_map.end()) {
			return key_val_map.at(key);
		}
		else {
			return std::monostate();
		}
	}

	std::unordered_set<fs::path> Configuration::getExplicitGlobules() const {
		std::unordered_set<fs::path> explicit_globules{};

		for (const auto& entry : _build_order.getOrDefault({})) {
			const auto path{ PathUtil::normalize(entry, project_root.getOrThrow()) };
			for (const auto& globule : globules.getOrDefault({})) {
				if (path == globule) {
					explicit_globules.insert(path);
					break;
				}
			}
		}

		return explicit_globules;
	}

	std::unordered_set<fs::path> Configuration::getExplicitPatches() const {
		std::unordered_set<fs::path> explicit_patches{};

		for (const auto& entry : _build_order.getOrDefault({})) {
			const auto path{ PathUtil::normalize(entry, project_root.getOrThrow()) };
			for (const auto& patch : patches.getOrDefault({})) {
				if (path == patch) {
					explicit_patches.insert(path);
					break;
				}
			}
		}

		return explicit_patches;
	}

	std::map<std::string, std::string> Configuration::parseUserVariables(const toml::value& table,
		const std::map<std::string, std::string>& previous_user_variables) {

		std::map<std::string, std::string> user_variables{ };
		std::map<std::string, const toml::value&> toml_values{};

		std::map<std::string, std::string> local_user_variables{};
		std::map<std::string, const toml::value&> local_toml_values{};

		toml::table variables;

		try {
			variables = toml::find<toml::table>(table, "variables");
		}
		catch (const std::out_of_range&) {
			// no variable table in file
			return {};
		}

		for (const auto& [key, value] : variables) {
			if (user_variables.count(key) != 0) {
				throw TomlException2(
					value,
					toml_values.at(key),
					"Duplicate user variable",
					{
						"The same user variable cannot be specified multiple times at the same level of configuration"
					},
					fmt::format("User variable '{}' specified multiple times", key),
					"Previously specified here"
				);
			}

			auto joined_vars{ previous_user_variables };

			for (const auto& [key, value] : local_user_variables) {
				joined_vars[key] = value;
			}

			local_user_variables.insert({ 
				key, 
				ConfigVariable<toml::string, std::string>::formatUserVariables(value, joined_vars)
			});
			local_toml_values.insert({ key, value });
		}

		user_variables.insert(local_user_variables.begin(), local_user_variables.end());
		toml_values.insert(local_toml_values.begin(), local_toml_values.end());

		return user_variables;
	}

	Configuration::UserVariableMap Configuration::parseVariableFiles(const VariableFileMap& variable_file_map) {
		UserVariableMap user_variable_map{};

		for (int i{ 0 }; i != static_cast<int>(ConfigurationLevel::_COUNT); ++i) {
			const auto config_level{ static_cast<ConfigurationLevel>(i) };
			if (variable_file_map.count(config_level) != 0) {
				const auto potential_variable_file{ variable_file_map.at(config_level) };

				if (fs::exists(potential_variable_file) && fs::is_regular_file(potential_variable_file)) {
					auto inherited_variables{ config_level == ConfigurationLevel::PROFILE
						? user_variable_map.at(ConfigurationLevel::PROJECT) : std::map<std::string, std::string>() };

					user_variable_map.insert({
						config_level,
						parseUserVariables(toml::parse(potential_variable_file), inherited_variables)
						});
				}
				else {
					user_variable_map.insert({ config_level, {} });
				}
			}
			else {
				user_variable_map.insert({ config_level, {} });
			}
		}

		return user_variable_map;
	}

	Configuration::ParsedConfigFileMap Configuration::parseConfigFiles(const ConfigFileMap& config_file_map) {
		ParsedConfigFileMap parsed_config_file_map{};

		for (const auto& [level, config_file_paths] : config_file_map) {
			parsed_config_file_map.insert({ level, {} });
			for (const auto& config_file_path : config_file_paths) {
				parsed_config_file_map.at(level).push_back(toml::parse(config_file_path));
			}
		}

		for (int i = 0; i != static_cast<int>(ConfigurationLevel::_COUNT); ++i) {
			const auto config_level{ static_cast<ConfigurationLevel>(i) };
			if (parsed_config_file_map.count(config_level) == 0) {
				parsed_config_file_map.insert({ config_level, {} });
			}
		}

		return parsed_config_file_map;
	}

	void Configuration::discoverProjectRoot(const ParsedConfigFileMap& config_file_map, const UserVariableMap& user_variable_map,
		const fs::path& callisto_root_directory) {
		for (int i = 0; i != static_cast<int>(ConfigurationLevel::_COUNT); ++i) {
			const auto config_level{ static_cast<ConfigurationLevel>(i) };
			for (const auto& config : config_file_map.at(config_level)) {
				project_root.trySet(config, config_level, callisto_root_directory,
					user_variable_map.at(config_level));
				if (project_root.isSet()) {
					key_val_map[project_root.name] = project_root.getOrThrow().string();
				}
			}
		}
	}

	void Configuration::discoverLogFilePath(const ParsedConfigFileMap& config_file_map, const UserVariableMap& user_variable_map) {
		for (int i = 0; i != static_cast<int>(ConfigurationLevel::_COUNT); ++i) {
			const auto config_level{ static_cast<ConfigurationLevel>(i) };
			for (const auto& config : config_file_map.at(config_level)) {
				log_file.trySet(config, config_level, project_root, user_variable_map.at(config_level));
			}
		}
	}

	void Configuration::discoverGenericTools(const ParsedConfigFileMap& config_file_map, const UserVariableMap& user_variable_map) {
		for (int i = 0; i != static_cast<int>(ConfigurationLevel::_COUNT); ++i) {
			const auto config_level{ static_cast<ConfigurationLevel>(i) };
			for (const auto& config : config_file_map.at(config_level)) {
				try {
					const auto& tools_table{ toml::find(config, "tools") };
					const auto& generic_tools_table{ toml::find<toml::table>(tools_table, "generic") };

					for (const auto& [key, value] : generic_tools_table) {
						generic_tool_configurations.insert({ key, ToolConfiguration(key) });
					}
				}
				catch (const std::out_of_range&) {
					continue;
				}
			}
		}
	}

	void Configuration::discoverToolDirectories(const ParsedConfigFileMap& config_file_map, const UserVariableMap& user_variable_map) {
		for (int i = 0; i != static_cast<int>(ConfigurationLevel::_COUNT); ++i) {
			const auto config_level{ static_cast<ConfigurationLevel>(i) };
			for (const auto& config : config_file_map.at(config_level)) {
				try {
					const auto& tools_table{ toml::find(config, "tools") };
					const auto& generic_tools_table{ toml::find<toml::table>(tools_table, "generic") };

					for (const auto& [key, value] : generic_tools_table) {
						trySet(generic_tool_configurations.at(key).working_directory,
							config, config_level, project_root, user_variable_map.at(config_level)
						);
					}
				}
				catch (const std::out_of_range&) {
					continue;
				}
			}
		}
	}

	void Configuration::discoverEmulators(const ParsedConfigFileMap& config_file_map, const UserVariableMap& user_variable_map) {
		for (int i = 0; i != static_cast<int>(ConfigurationLevel::_COUNT); ++i) {
			const auto config_level{ static_cast<ConfigurationLevel>(i) };
			for (const auto& config : config_file_map.at(config_level)) {
				try {
					const auto& emulators_table{ toml::find<toml::table>(config, "emulators") };

					for (const auto& [key, value] : emulators_table) {
						EmulatorConfiguration emulator_config{ key };
						emulator_config.executable.trySet(config, config_level, project_root, user_variable_map.at(config_level));
						emulator_config.options.trySet(config, config_level, user_variable_map.at(config_level));
						emulator_configurations.insert({ key, emulator_config });
					}
				}
				catch (const std::out_of_range&) {
					continue;
				}
			}
		}
	}

	void Configuration::fillInConfiguration(const ParsedConfigFileMap& config_file_map, const UserVariableMap& user_variable_map) {
		for (int i = 0; i != static_cast<int>(ConfigurationLevel::_COUNT); ++i) {
			const auto config_level{ static_cast<ConfigurationLevel>(i) };
			fillOneConfigurationLevel(config_file_map.at(config_level), config_level, 
				user_variable_map.at(config_level));
		}
	}

	void Configuration::fillOneConfigurationLevel(const std::vector<toml::value>& config_files, ConfigurationLevel level,
		const std::map<std::string, std::string>& user_variables) {
		for (const auto& config_file : config_files) {
			fillOneConfigurationFile(config_file, level, user_variables);
			discoverBuildOrder(config_file, level, user_variables);
		}
	}

	void Configuration::fillOneConfigurationFile(const toml::value& config_file, ConfigurationLevel level,
		const std::map<std::string, std::string>& user_variables) {

		const auto& root{ project_root };

		trySet(config_name, config_file, level, user_variables);

		rom_size.trySet(config_file, level, user_variables);

		trySet(use_text_map16_format, config_file, level);

		trySet(clean_rom, config_file, level, root, user_variables);
		
		trySet(project_rom, config_file, level, root, user_variables);
		trySet(temporary_rom, config_file, level, root, user_variables);
		trySet(bps_package, config_file, level, root, user_variables);

		trySet(level_import_flag, config_file, level, user_variables);

		trySet(check_conflicts, config_file, level, user_variables);
		trySet(conflict_log_file, config_file, level, root, user_variables);

		trySet(flips_path, config_file, level, root, user_variables);
		
		trySet(lunar_magic_path, config_file, level, root, user_variables);
		trySet(lunar_magic_level_import_flags, config_file, level, user_variables);

		trySet(initial_patch, config_file, level, root, user_variables);
		trySet(graphics, config_file, level, root, user_variables);
		trySet(ex_graphics, config_file, level, root, user_variables);
		trySet(levels, config_file, level, root, user_variables);
		trySet(shared_palettes, config_file, level, root, user_variables);
		trySet(map16, config_file, level, root, user_variables);
		trySet(overworld, config_file, level, root, user_variables);
		trySet(titlescreen, config_file, level, root, user_variables);
		trySet(title_moves, config_file, level, root, user_variables);
		trySet(credits, config_file, level, root, user_variables);
		trySet(global_exanimation, config_file, level, root, user_variables);

		patches.trySet(config_file, level, root, user_variables);
		globules.trySet(config_file, level, root, user_variables);

		trySet(globule_header, config_file, level, root, user_variables);

		for (auto& [_, tool] : generic_tool_configurations) {
			trySet(tool.executable, config_file, level, tool.working_directory, user_variables);
			trySet(tool.options, config_file, level, user_variables);
			trySet(tool.takes_user_input, config_file, level);
			tool.static_dependencies.trySet(config_file, level, tool.working_directory, user_variables);
			tool.dependency_report_file.trySet(config_file, level, tool.working_directory, user_variables);
			trySet(tool.pass_rom, config_file, level);
		}
	}

	void Configuration::discoverBuildOrder(const toml::value& config_file, ConfigurationLevel level,
		const std::map<std::string, std::string>& user_variables) {
		try {
			const auto& orders{ toml::find(config_file, "orders") };
			const auto& toml_build_order{ toml::find(orders, "build_order") };

			if (verifyBuildOrder(toml::get<toml::array>(toml_build_order), user_variables)) {
				_build_order.trySet(config_file, level, user_variables);
			}
		}
		catch (const std::out_of_range&) {
			return;
		}
	}

	bool Configuration::verifyBuildOrder(const toml::array& build_order, const std::map<std::string, std::string>& user_variables) const {
		for (const auto& symbol : build_order) {
			const auto& as_string{ 
				ConfigVariable<toml::string, std::string>::formatUserVariables(toml::get<std::string>(symbol), user_variables)
			};

			if (isValidStaticBuildOrderSymbol(as_string)) {
				continue;
			} 
			if (generic_tool_configurations.count(as_string) != 0) {
				continue;
			}
			const auto path{ PathUtil::normalize(as_string, project_root.getOrThrow()) };

			if (isValidPatchSymbol(path) || isValidGlobuleSymbol(path)) {
				continue;
			}

			throw TomlException(
				symbol,
				"Invalid symbol in build order",
				{
					fmt::format(
						"Valid symbols are paths that appear in the resources.patches or resources.globules lists, "
						"the name of any tool appearing in the tools.generic table or any of the "
						"following: {}",
						fmt::join(VALID_STATIC_BUILD_ORDER_SYMBOLS, ", ")
					)
				},
				fmt::format("'{}' is not a valid build symbol", as_string)
			);
		}
		return true;
	}

	bool Configuration::isValidPatchSymbol(const fs::path& patch_path) const {
		if (!patches.isSet()) {
			return false;
		}

		for (const auto& patch : patches.getOrThrow()) {
			if (patch_path == patch) {
				return true;
			}
		}
		return false;
	}

	bool Configuration::isValidGlobuleSymbol(const fs::path& globule_path) const {
		if (!globules.isSet()) {
			return false;
		}

		for (const auto& globule : globules.getOrThrow()) {
			if (globule_path == globule) {
				return true;
			}
		}
		return false;
	}

	bool Configuration::isValidStaticBuildOrderSymbol(const std::string& symbol) const {
		for (const auto& entry : VALID_STATIC_BUILD_ORDER_SYMBOLS) {
			if (entry == symbol) {
				return true;
			}
		}
		return false;
	}

	std::vector<Descriptor> Configuration::symbolToDescriptor(const std::string& symbol) const {
		if (symbol == "Graphics") {
			return { Descriptor(Symbol::GRAPHICS) };
		}
		else if (symbol == "ExGraphics") {
			return { Descriptor(Symbol::EX_GRAPHICS) };
		}
		else if (symbol == "Map16") {
			return { Descriptor(Symbol::MAP16) };
		}
		else if (symbol == "TitleScreenMovement") {
			return { Descriptor(Symbol::TITLE_SCREEN_MOVEMENT) };
		}
		else if (symbol == "SharedPalettes") {
			return { Descriptor(Symbol::SHARED_PALETTES) }; 
		}
		else if (symbol == "Overworld") {
			return { Descriptor(Symbol::OVERWORLD) };
		}
		else if (symbol == "TitleScreen") {
			return { Descriptor(Symbol::TITLE_SCREEN) };
		}
		else if (symbol == "Credits") {
			return { Descriptor(Symbol::CREDITS) };
		}
		else if (symbol == "GlobalExAnimation") {
			return { Descriptor(Symbol::GLOBAL_EX_ANIMATION) };
		}
		else if (symbol == "Patches") {
			if (patches.isSet()) {
				std::vector<Descriptor> patch_descriptors{};

				const auto explicit_patches{ getExplicitPatches() };

				for (const auto& patch : patches.getOrDefault({})) {
					if (explicit_patches.count(patch) == 0) {
						patch_descriptors.emplace_back(Symbol::PATCH, patch.string());
					}
				}

				return patch_descriptors;
			}
			else {
				return {};
			}
		}
		else if (symbol == "Globules") {
			if (globules.isSet()) {
				std::vector<Descriptor> globule_descriptors{};

				const auto explicit_globules{ getExplicitGlobules() };

				for (const auto& globule : globules.getOrDefault({})) {
					if (explicit_globules.count(globule) == 0) {
						globule_descriptors.emplace_back(Symbol::GLOBULE, globule.string());
					}
				}

				return globule_descriptors;
			}

			return {};
		}
		else if (symbol == "Levels") {
			return { Descriptor(Symbol::LEVELS) };
		}
		else {
			if (isValidPatchSymbol(PathUtil::normalize(symbol, project_root.getOrThrow()))) {
				return { Descriptor(Symbol::PATCH, PathUtil::normalize(symbol, project_root.getOrThrow()).string()) };
			}
			if (isValidGlobuleSymbol(PathUtil::normalize(symbol, project_root.getOrThrow()))) {
				return { Descriptor(Symbol::GLOBULE, PathUtil::normalize(symbol, project_root.getOrThrow()).string()) };
			}
			for (const auto& [name, config] : generic_tool_configurations) {
				if (symbol == name) {
					return { Descriptor(Symbol::EXTERNAL_TOOL, symbol) };
				}
			}
			throw ConfigException(fmt::format(
				"Unknown build order symbol '{}'",
				symbol
			));
		}
	}
}