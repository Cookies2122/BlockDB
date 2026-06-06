#ifndef _INCLUDE_METAMOD_SOURCE_STUB_PLUGIN_H_
#define _INCLUDE_METAMOD_SOURCE_STUB_PLUGIN_H_

#include <ISmmPlugin.h>
#include <stdio.h>
#include <sh_vector.h>
#include "utlvector.h"
#include "ehandle.h"
#include <iserver.h>
#include <entity2/entitysystem.h>
#include "igameevents.h"
#include "vector.h"
#include <deque>
#include <functional>
#include <utlstring.h>
#include "metamod_oslink.h"
#include "schemasystem/schemasystem.h"
#include <steam/steam_gameserver.h>
#include <filesystem.h>
#include <nlohmann/json.hpp>
#include <thread>
#include <chrono>
#include <mutex>
#include <condition_variable>
#include <string>
#include <vector>
#include <ctime>
#include <cstring>
#include <cstdarg>
#include <sys/stat.h>
#include <sys/types.h>
#include "CCSPlayerController.h"
#include "include/menus.h"
#include "include/admin.h"


class BlockDB final : public ISmmPlugin, public IMetamodListener
{
public:
	bool Load(PluginId id, ISmmAPI* ismm, char* error, size_t maxlen, bool late);
	bool Unload(char* error, size_t maxlen);
	void AllPluginsLoaded();
private:
	const char* GetAuthor();
	const char* GetName();
	const char* GetDescription();
	const char* GetURL();
	const char* GetLicense();
	const char* GetVersion();
	const char* GetDate();
	const char* GetLogTag();
private:
	void OnGameServerSteamAPIActivated();
	void Authorization();
};

#endif //_INCLUDE_METAMOD_SOURCE_STUB_PLUGIN_H_
