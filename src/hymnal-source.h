#pragma once

#include <obs-module.h>

#ifdef __cplusplus
extern "C" {
#endif

extern struct obs_source_info hymnal_text_source_info;

/* Releases the shared overlay shader; call from obs_module_unload. */
void hymnal_source_free_effect(void);

#ifdef __cplusplus
}
#endif
