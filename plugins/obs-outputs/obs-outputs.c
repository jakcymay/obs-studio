#include <obs-module.h>

OBS_DECLARE_MODULE()
OBS_MODULE_USE_DEFAULT_LOCALE("obs-outputs", "en-US")

extern struct obs_output_info rtmp_output_info;

bool obs_module_load(void)
{
	obs_register_output(&rtmp_output_info);
	return true;
}

void obs_module_unload(void)
{

}
