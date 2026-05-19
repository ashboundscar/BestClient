#include "reshade_runtime.h"

#include <base/fs.h>
#include <base/system.h>

#include <engine/storage.h>

#include <cstdlib>
#include <string>

namespace
{
enum class EBestClientReShadeAddonStatus
{
	LOG_MISSING = 0,
	READY,
	LIMITED_SUPPORT,
	FAILED_INITIALIZATION,
	PENDING_INITIALIZATION,
};

struct SBestClientReShadeLogCache
{
	char m_aPath[IO_MAX_PATH_LENGTH] = "";
	time_t m_ModifiedTime = 0;
	EBestClientReShadeAddonStatus m_Status = EBestClientReShadeAddonStatus::LOG_MISSING;
	bool m_HasValue = false;
};

SBestClientReShadeLogCache gs_ReShadeLogCache;

void BestClientInvalidateReShadeLogCache()
{
	gs_ReShadeLogCache = SBestClientReShadeLogCache{};
}

void BestClientGetBinaryPath(IStorage *pStorage, const char *pFilename, char *pPath, int PathSize)
{
	if(pPath == nullptr || PathSize <= 0)
		return;

	pPath[0] = '\0';
	if(pStorage == nullptr)
		return;

	pStorage->GetBinaryPath(pFilename, pPath, PathSize);
}

bool BestClientReadAbsoluteTextFile(IStorage *pStorage, const char *pAbsolutePath, std::string &Text)
{
	char *pFileText = pStorage->ReadFileStr(pAbsolutePath, IStorage::TYPE_ABSOLUTE);
	if(pFileText == nullptr)
		return false;

	Text = pFileText;
	free(pFileText);
	return true;
}

EBestClientReShadeAddonStatus BestClientParseReShadeAddonStatus(const std::string &LogText)
{
	if(str_find_nocase(LogText.c_str(), "[BestClient/ReShadeAddon] Add-on initialized.") != nullptr ||
		str_find_nocase(LogText.c_str(), "[BestClient/ReShadeBridge] Add-on initialized.") != nullptr)
	{
		return EBestClientReShadeAddonStatus::READY;
	}

	if(str_find_nocase(LogText.c_str(), "limited add-on functionality") != nullptr)
		return EBestClientReShadeAddonStatus::LIMITED_SUPPORT;

	if(str_find_nocase(LogText.c_str(), "Failed to load add-on") != nullptr ||
		str_find_nocase(LogText.c_str(), "Failed to register an event") != nullptr)
	{
		return EBestClientReShadeAddonStatus::FAILED_INITIALIZATION;
	}

	return EBestClientReShadeAddonStatus::PENDING_INITIALIZATION;
}

EBestClientReShadeAddonStatus BestClientQueryReShadeAddonStatus(IStorage *pStorage)
{
	char aLogPath[IO_MAX_PATH_LENGTH];
	BestClientGetBinaryPath(pStorage, "ReShade.log", aLogPath, sizeof(aLogPath));
	if(aLogPath[0] == '\0')
	{
		BestClientInvalidateReShadeLogCache();
		return EBestClientReShadeAddonStatus::LOG_MISSING;
	}

	time_t ModifiedTime = 0;
	time_t CreatedTime = 0;
	if(fs_file_time(aLogPath, &CreatedTime, &ModifiedTime) != 0)
	{
		BestClientInvalidateReShadeLogCache();
		return EBestClientReShadeAddonStatus::LOG_MISSING;
	}

	if(gs_ReShadeLogCache.m_HasValue && str_comp(gs_ReShadeLogCache.m_aPath, aLogPath) == 0 && gs_ReShadeLogCache.m_ModifiedTime == ModifiedTime)
		return gs_ReShadeLogCache.m_Status;

	std::string ReShadeLogText;
	if(!BestClientReadAbsoluteTextFile(pStorage, aLogPath, ReShadeLogText))
	{
		BestClientInvalidateReShadeLogCache();
		return EBestClientReShadeAddonStatus::LOG_MISSING;
	}

	str_copy(gs_ReShadeLogCache.m_aPath, aLogPath, sizeof(gs_ReShadeLogCache.m_aPath));
	gs_ReShadeLogCache.m_ModifiedTime = ModifiedTime;
	gs_ReShadeLogCache.m_Status = BestClientParseReShadeAddonStatus(ReShadeLogText);
	gs_ReShadeLogCache.m_HasValue = true;
	return gs_ReShadeLogCache.m_Status;
}
}

bool BestClientReShadeRuntimeCommitPreset(IStorage *pStorage, char *pError, int ErrorSize)
{
	if(pError != nullptr && ErrorSize > 0)
		pError[0] = '\0';

	if(pStorage == nullptr)
	{
		str_copy(pError, "Storage interface is unavailable.", ErrorSize);
		return false;
	}

	switch(BestClientQueryReShadeAddonStatus(pStorage))
	{
	case EBestClientReShadeAddonStatus::READY:
		return true;
	case EBestClientReShadeAddonStatus::LOG_MISSING:
		str_copy(pError, "ReShade.log is not available yet.", ErrorSize);
		return false;
	case EBestClientReShadeAddonStatus::LIMITED_SUPPORT:
		str_copy(pError, "The current session is still running with the old limited vulkan-1.dll. Close DDNet so the new ReShade runtime can be copied in.", ErrorSize);
		return false;
	case EBestClientReShadeAddonStatus::FAILED_INITIALIZATION:
		str_copy(pError, "ReShade found the add-on, but it failed to initialize.", ErrorSize);
		return false;
	case EBestClientReShadeAddonStatus::PENDING_INITIALIZATION:
	default:
		str_copy(pError, "ReShade add-on has not initialized in this session yet.", ErrorSize);
		return false;
	}
}
