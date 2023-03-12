#pragma once

#include <string>
#include <filesystem>
#include <fstream>
#include <functional>
#include <optional>
#include <chrono>

#include <spdlog/spdlog.h>
#include <fmt/format.h>
#include <nlohmann/json.hpp>

#include "../insertable.h"
#include "../not_found_exception.h"

namespace fs = std::filesystem;
using json = nlohmann::json;

namespace stardust {
	class Dependency {
	public:
		const fs::path dependent_path;
		const std::optional<fs::file_time_type> last_write_time;

		Dependency(const fs::path& dependent_path)
			: dependent_path(dependent_path),
			last_write_time(fs::exists(dependent_path) ? std::make_optional(fs::last_write_time(dependent_path)) : std::nullopt)
		{
			spdlog::debug(fmt::format("Dependency created on '{}' -> {}", 
				dependent_path.string(), 
				fs::exists(dependent_path) ? "exists" : "missing"
			));
		}

		Dependency(const json& j) : dependent_path(j.at("path")), last_write_time(
			std::chrono::time_point<fs::_File_time_clock>(
				std::chrono::milliseconds(j["timestamp"].get<long>()))
			) {}

		json toJson() const {
			auto j{ json({ "path", dependent_path.string() }) };
			if (last_write_time.has_value()) {
				j["timestamp"] = last_write_time.value().time_since_epoch().count();
			}
			else {
				j["timestamp"] = nullptr;
			}
			return j;
		}

		bool operator==(const Dependency& other) const {
			return dependent_path == other.dependent_path && last_write_time == other.last_write_time;
		}
	};
}

namespace std {
	template<>
	struct hash<stardust::Dependency> {
		size_t operator()(const stardust::Dependency& dependency) const {
			return hash<fs::path>{}(dependency.dependent_path);
		}
	};
}
