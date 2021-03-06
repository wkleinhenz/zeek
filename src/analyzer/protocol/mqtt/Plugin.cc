// See the file  in the main distribution directory for copyright.

#include "MQTT.h"
#include "plugin/Plugin.h"
#include "analyzer/Component.h"

namespace zeek::plugin::detail::Zeek_MQTT {

class Plugin : public zeek::plugin::Plugin {
public:
	zeek::plugin::Configuration Configure() override
		{
		AddComponent(new zeek::analyzer::Component("MQTT",
		             zeek::analyzer::mqtt::MQTT_Analyzer::InstantiateAnalyzer));

		zeek::plugin::Configuration config;
		config.name = "Zeek::MQTT";
		config.description = "Message Queuing Telemetry Transport v3.1.1 Protocol analyzer";
		return config;
		}
} plugin;

} // namespace zeek::plugin::detail::Zeek_MQTT
