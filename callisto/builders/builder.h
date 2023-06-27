#pragma once

#include <memory>

#include <nlohmann/json.hpp>

#include "../configuration/configuration.h"

#include "../insertables/exgraphics.h"
#include "../insertables/graphics.h"
#include "../insertables/shared_palettes.h"
#include "../insertables/overworld.h"
#include "../insertables/title_screen.h"
#include "../insertables/global_exanimation.h"
#include "../insertables/credits.h"
#include "../insertables/title_moves.h"
#include "../insertables/levels.h"
#include "../insertables/binary_map16.h"
#include "../insertables/text_map16.h"
#include "../insertables/external_tool.h"
#include "../insertables/patch.h"
#include "../insertables/globule.h"

#include "../insertable.h"
#include "../saver/saver.h"
#include "../descriptor.h"

#include "../time_util.h"

using json = nlohmann::json;

namespace callisto {
	class Builder {
	protected:
		static constexpr auto BUILD_REPORT_VERSION{ 1 };

		static constexpr auto DEFINE_PREFIX{ "CALLISTO" };
		static constexpr auto VERSION_DEFINE_NAME{ "VERSION" };
		static constexpr auto MAJOR_VERSION_DEFINE_NAME{ "MAJOR" };
		static constexpr auto MINOR_VERSION_DEFINE_NAME{ "MINOR" };
		static constexpr auto PATCH_VERSION_DEFINE_NAME{ "PATCH" };
		static constexpr auto ASSEMBLING_DEFINE_NAME{ "ASSEMBLING" };
		static constexpr auto PROFILE_DEFINE_NAME{ "PROFILE" };

		using Insertables = std::vector<std::pair<Descriptor, std::shared_ptr<Insertable>>>;
		using DependencyVector = std::vector<std::pair<Descriptor, std::pair<std::unordered_set<ResourceDependency>,
			std::unordered_set<ConfigurationDependency>>>>;
	
		static Insertables buildOrderToInsertables(const Configuration& config);
		static std::shared_ptr<Insertable> descriptorToInsertable(const Descriptor& descriptor, const Configuration& config);

		static json createBuildReport(const Configuration& config, const json& dependency_report);
		static void writeBuildReport(const fs::path& project_root, const json& j);

		static void cacheGlobules(const fs::path& project_root);
		static void moveTempToOutput(const Configuration& config);

		static void init(const Configuration& config);
		static void ensureCacheStructure(const Configuration& config);
		static void generateAssemblyLevelInformation(const Configuration& config);
		static void generateGlobuleCallFile(const Configuration& config);

		static void writeIfDifferent(const std::string& str, const fs::path& out_file);

		static void removeBuildReport(const fs::path& project_root);

		static std::vector<std::thread> createInitThreads(const Insertables& insertables);

	public:
		virtual void build(const Configuration& config) = 0;
	};
}