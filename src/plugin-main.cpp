/*
Hymnal Plugin for OBS Studio
*/

#include <obs-module.h>
#include <obs-frontend-api.h>
#include <plugin-support.h>

#include "hymnal-source.h"
#include "hymnal-dock.hpp"

OBS_DECLARE_MODULE()
OBS_MODULE_USE_DEFAULT_LOCALE(PLUGIN_NAME, "en-US")

bool obs_module_load(void)
{
	obs_register_source(&hymnal_text_source_info);
	RegisterHymnalDock();

	obs_log(LOG_INFO, "plugin loaded successfully (version %s)", PLUGIN_VERSION);
	return true;
}

void obs_module_unload(void)
{
	hymnal_source_free_effect();
	obs_log(LOG_INFO, "plugin unloaded");
}
