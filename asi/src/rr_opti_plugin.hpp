#pragma once

#include "optiscaler_plugin.h"

namespace openrr::pe::opti_plugin {

using DeferredInitFn = void (*)();

// Called from exported OptiPluginInit (runs under loader lock for normal ASI plugins).
// Registers the OpenRR tab under OptiScaler's PLUGINS sidebar and, when Enabled=true and
// OptiScaler exposes Plugin API v3 RegisterProvider, registers OpenRR as an OptiProvider_Dlssd.
bool init(const OptiPluginApi* api, OptiPlugin* plugin, OptiGetProcFn provider_get_proc,
          DeferredInitFn deferred_init);

// Re-applies a pending [Menu] TTFFontPath write on process exit if OptiScaler's Save INI overwrote it.
void flush_pending_font();

} // namespace openrr::pe::opti_plugin
