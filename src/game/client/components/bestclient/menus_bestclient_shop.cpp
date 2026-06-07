/* Copyright © 2026 BestProject Team */
#include <base/fs.h>
#include <base/io.h>
#include <base/math.h>
#include <base/str.h>
#include <base/system.h>

#include <engine/font_icons.h>
#include <engine/shared/config.h>
#include <engine/shared/http.h>
#include <engine/shared/json.h>
#include <engine/storage.h>

#include <game/client/components/menus.h>
#include <game/client/gameclient.h>
#include <game/client/ui_listbox.h>
#include <game/localization.h>

#include <zlib.h>

#include <algorithm>
#include <array>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

using namespace FontIcon;

namespace
{
	static constexpr const char *BESTCLIENT_SHOP_HOST = "https://catdata.pages.dev";
	static constexpr const char *BESTCLIENT_SHOP_BROWSE_API_URL = "https://catdata.pages.dev/api/skins?page=%d&limit=10&type=%s";
	static constexpr const char *BESTCLIENT_SHOP_SEARCH_API_URL = "https://catdata.pages.dev/api/skins?page=%d&limit=10&type=%s&search=%s";
	static constexpr CTimeout BESTCLIENT_SHOP_TIMEOUT{8000, 0, 1024, 8};
	static constexpr int64_t BESTCLIENT_SHOP_PAGE_MAX_RESPONSE_SIZE = 2 * 1024 * 1024;
	static constexpr int64_t BESTCLIENT_SHOP_IMAGE_MAX_RESPONSE_SIZE = 32 * 1024 * 1024;
	static constexpr int64_t BESTCLIENT_SHOP_AUDIO_MAX_RESPONSE_SIZE = 128 * 1024 * 1024;

	static constexpr float BESTCLIENT_SHOP_MARGIN = 10.0f;
	static constexpr float BESTCLIENT_SHOP_MARGIN_SMALL = 5.0f;
	static constexpr float BESTCLIENT_SHOP_LINE_SIZE = 20.0f;
	static constexpr float BESTCLIENT_SHOP_HEADLINE_FONT_SIZE = 20.0f;
	static constexpr float BESTCLIENT_SHOP_FONT_SIZE = 14.0f;
	static constexpr float BESTCLIENT_SHOP_SMALL_FONT_SIZE = 12.0f;
	static constexpr float BESTCLIENT_SHOP_TAB_WIDTH = 120.0f;
	static constexpr float BESTCLIENT_SHOP_TAB_HEIGHT = 26.0f;
	static constexpr float BESTCLIENT_SHOP_SECTION_ROUNDING = 8.0f;
	static constexpr float BESTCLIENT_SHOP_ITEM_HEIGHT = 88.0f;

	enum
	{
		BESTCLIENT_SHOP_ENTITIES = 0,
		BESTCLIENT_SHOP_GAME,
		BESTCLIENT_SHOP_EMOTICONS,
		BESTCLIENT_SHOP_PARTICLES,
		BESTCLIENT_SHOP_HUD,
		BESTCLIENT_SHOP_ARROWS,
		BESTCLIENT_SHOP_CURSORS,
		BESTCLIENT_SHOP_AUDIO,
		NUM_BESTCLIENT_SHOP_TABS,
	};

	enum
	{
		BESTCLIENT_ASSETS_TAB_ENTITIES = 0,
		BESTCLIENT_ASSETS_TAB_GAME = 1,
		BESTCLIENT_ASSETS_TAB_EMOTICONS = 2,
		BESTCLIENT_ASSETS_TAB_PARTICLES = 3,
		BESTCLIENT_ASSETS_TAB_HUD = 4,
		BESTCLIENT_ASSETS_TAB_ARROW = 7,
		BESTCLIENT_ASSETS_TAB_CURSOR = 6,
		BESTCLIENT_ASSETS_TAB_AUDIO = 8,
	};

	struct SBestClientShopTypeInfo
	{
		const char *m_pLabel;
		const char *m_pApiType;
		const char *m_pAssetDirectory;
		int m_AssetsTab;
	};

	static const SBestClientShopTypeInfo gs_aBestClientShopTypeInfos[NUM_BESTCLIENT_SHOP_TABS] = {
		{"Entities", "entity", "assets/entities", BESTCLIENT_ASSETS_TAB_ENTITIES},
		{"Game", "gameskin", "assets/game", BESTCLIENT_ASSETS_TAB_GAME},
		{"Emoticons", "emoticon", "assets/emoticons", BESTCLIENT_ASSETS_TAB_EMOTICONS},
		{"Particles", "particle", "assets/particles", BESTCLIENT_ASSETS_TAB_PARTICLES},
		{"HUD", "hud", "assets/hud", BESTCLIENT_ASSETS_TAB_HUD},
		{"Arrows", "arrows", "assets/arrow", BESTCLIENT_ASSETS_TAB_ARROW},
		{"Cursors", "cursor", "assets/cursor", BESTCLIENT_ASSETS_TAB_CURSOR},
		{"Audio", "sounds", "assets/audio", BESTCLIENT_ASSETS_TAB_AUDIO},
	};

	struct SBestClientShopItem
	{
		char m_aId[64]{};
		char m_aName[128]{};
		char m_aFilename[128]{};
		char m_aUsername[64]{};
		char m_aImageUrl[256]{};
		char m_aDownloadUrl[256]{};
		bool m_PreviewFailed = false;
		int m_PreviewWidth = 0;
		int m_PreviewHeight = 0;
		IGraphics::CTextureHandle m_PreviewTexture;
		CButtonContainer m_PreviewButton;
		CButtonContainer m_ActionButton;
		CButtonContainer m_DeleteButton;
	};

	struct SBestClientShopState
	{
		bool m_Initialized = false;
		int m_Tab = BESTCLIENT_SHOP_ENTITIES;
		int m_SelectedIndex = -1;
		int m_TotalPages = 1;
		int m_TotalItems = 0;
		int m_LoadedTab = -1;
		int m_LoadedPage = 0;
		int m_FetchTab = -1;
		int m_FetchPage = 0;
		int m_PreviewTab = -1;
		int m_PreviewPage = 0;
		int m_InstallTab = -1;
		int m_InstallUrlIndex = 0;
		std::array<int, NUM_BESTCLIENT_SHOP_TABS> m_aPages{};
		std::shared_ptr<CHttpRequest> m_pFetchTask;
		std::shared_ptr<CHttpRequest> m_pInstallTask;
		std::shared_ptr<CHttpRequest> m_pPreviewTask;
		std::vector<SBestClientShopItem> m_vItems;
		std::vector<std::string> m_vInstallUrls;
		char m_aAppliedSearch[128]{};
		char m_aLoadedSearch[128]{};
		char m_aFetchSearch[128]{};
		char m_aPreviewSearch[128]{};
		char m_aInstallAssetName[128]{};
		char m_aInstallItemId[64]{};
		char m_aPreviewItemId[64]{};
		char m_aPreviewPath[IO_MAX_PATH_LENGTH]{};
		char m_aStatus[256]{};
		char m_aOpenPreviewItemId[64]{};
		bool m_PreviewOpen = false;
		bool m_Visible = false;
		CButtonContainer m_PreviewCloseButton;
	};

	static SBestClientShopState gs_BestClientShopState;
	static CLineInputBuffered<128> gs_BestClientShopSearchInput;

	static void BestClientShopSetStatus(const char *pText)
	{
		str_copy(gs_BestClientShopState.m_aStatus, pText, sizeof(gs_BestClientShopState.m_aStatus));
	}

	static void BestClientShopAbortTask(std::shared_ptr<CHttpRequest> &pTask)
	{
		if(pTask)
		{
			pTask->Abort();
			pTask = nullptr;
		}
	}

	static int BestClientShopDoButtonLogic(CMenus *pMenus, const void *pId, int Checked, const CUIRect *pRect, unsigned Flags)
	{
		return pMenus->MenuUi()->DoButtonLogic(pId, Checked, pRect, Flags, CUi::EButtonSoundType::BUTTON);
	}

	static int BestClientShopDoMenuButton(CMenus *pMenus, CButtonContainer *pButtonContainer, const char *pText, int Checked, const CUIRect *pRect, unsigned Flags, const char *pImageName, int Corners, float Rounding, float FontFactor, ColorRGBA Color)
	{
		return pMenus->DoButton_Menu(pButtonContainer, pText, Checked, pRect, Flags, pImageName, Corners, Rounding, FontFactor, Color);
	}

	static void BestClientShopResetInstallState()
	{
		gs_BestClientShopState.m_vInstallUrls.clear();
		gs_BestClientShopState.m_InstallUrlIndex = 0;
		gs_BestClientShopState.m_InstallTab = -1;
		gs_BestClientShopState.m_aInstallAssetName[0] = '\0';
		gs_BestClientShopState.m_aInstallItemId[0] = '\0';
	}

	static void BestClientShopCancelInstall()
	{
		BestClientShopAbortTask(gs_BestClientShopState.m_pInstallTask);
		BestClientShopResetInstallState();
		BestClientShopSetStatus(TCLocalize("Download canceled"));
	}

	static void BestClientShopInitState()
	{
		if(gs_BestClientShopState.m_Initialized)
		{
			return;
		}

		gs_BestClientShopState.m_Initialized = true;
		gs_BestClientShopState.m_aPages.fill(1);
		gs_BestClientShopState.m_TotalPages = 1;
	}

	static void BestClientShopClosePreview()
	{
		gs_BestClientShopState.m_PreviewOpen = false;
		gs_BestClientShopState.m_aOpenPreviewItemId[0] = '\0';
	}

	static bool BestClientShopHasPreviewOpen()
	{
		return gs_BestClientShopState.m_PreviewOpen && gs_BestClientShopState.m_aOpenPreviewItemId[0] != '\0';
	}

	static bool BestClientShopHasActiveSearch()
	{
		return gs_BestClientShopState.m_aAppliedSearch[0] != '\0';
	}

	static void BestClientShopClearItems(CMenus *pMenus)
	{
		for(SBestClientShopItem &Item : gs_BestClientShopState.m_vItems)
		{
			if(Item.m_PreviewTexture.IsValid())
			{
				pMenus->MenuGraphics()->UnloadTexture(&Item.m_PreviewTexture);
			}
		}
		gs_BestClientShopState.m_vItems.clear();
		gs_BestClientShopState.m_SelectedIndex = -1;
	}

	static void BestClientShopAbortPreviewTask()
	{
		BestClientShopAbortTask(gs_BestClientShopState.m_pPreviewTask);
		gs_BestClientShopState.m_PreviewTab = -1;
		gs_BestClientShopState.m_PreviewPage = 0;
		gs_BestClientShopState.m_aPreviewItemId[0] = '\0';
		gs_BestClientShopState.m_aPreviewPath[0] = '\0';
		gs_BestClientShopState.m_aPreviewSearch[0] = '\0';
	}

	static void BestClientShopStopBackgroundWork()
	{
		BestClientShopAbortTask(gs_BestClientShopState.m_pFetchTask);
		gs_BestClientShopState.m_FetchTab = -1;
		gs_BestClientShopState.m_FetchPage = 0;
		gs_BestClientShopState.m_aFetchSearch[0] = '\0';

		BestClientShopAbortPreviewTask();
		BestClientShopAbortTask(gs_BestClientShopState.m_pInstallTask);
		BestClientShopResetInstallState();
		BestClientShopClosePreview();
	}

	static void BestClientShopSetVisible(bool Visible)
	{
		BestClientShopInitState();
		if(gs_BestClientShopState.m_Visible == Visible)
		{
			return;
		}

		gs_BestClientShopState.m_Visible = Visible;
		if(!Visible)
		{
			BestClientShopStopBackgroundWork();
		}
	}

	static void BestClientShopInvalidatePage(CMenus *pMenus)
	{
		BestClientShopAbortTask(gs_BestClientShopState.m_pFetchTask);
		BestClientShopAbortPreviewTask();
		BestClientShopClosePreview();
		gs_BestClientShopState.m_LoadedTab = -1;
		gs_BestClientShopState.m_LoadedPage = 0;
		gs_BestClientShopState.m_aLoadedSearch[0] = '\0';
	}

	static void BestClientShopTrimQuery(const char *pInput, char *pOutput, size_t OutputSize)
	{
		str_copy(pOutput, pInput != nullptr ? pInput : "", OutputSize);

		int Start = 0;
		while(pOutput[Start] != '\0' && (unsigned char)pOutput[Start] <= 32)
		{
			++Start;
		}

		int End = str_length(pOutput);
		while(End > Start && (unsigned char)pOutput[End - 1] <= 32)
		{
			--End;
		}

		if(Start > 0 || End < str_length(pOutput))
		{
			int WritePos = 0;
			for(int ReadPos = Start; ReadPos < End && WritePos < (int)OutputSize - 1; ++ReadPos)
			{
				pOutput[WritePos++] = pOutput[ReadPos];
			}
			pOutput[WritePos] = '\0';
		}
	}

	static void BestClientShopSetTab(CMenus *pMenus, int Tab)
	{
		BestClientShopInitState();
		Tab = std::clamp(Tab, 0, NUM_BESTCLIENT_SHOP_TABS - 1);
		if(gs_BestClientShopState.m_Tab == Tab)
		{
			return;
		}

		gs_BestClientShopState.m_Tab = Tab;
		BestClientShopInvalidatePage(pMenus);
	}

	static void BestClientShopSetPage(CMenus *pMenus, int Page)
	{
		Page = maximum(1, Page);
		int &CurrentPage = gs_BestClientShopState.m_aPages[gs_BestClientShopState.m_Tab];
		if(CurrentPage == Page)
		{
			return;
		}

		CurrentPage = Page;
		BestClientShopInvalidatePage(pMenus);
	}

	static void BestClientShopSetSearch(CMenus *pMenus, const char *pSearch)
	{
		char aTrimmed[128];
		BestClientShopTrimQuery(pSearch, aTrimmed, sizeof(aTrimmed));
		if(str_comp(aTrimmed, gs_BestClientShopState.m_aAppliedSearch) == 0)
		{
			return;
		}

		str_copy(gs_BestClientShopState.m_aAppliedSearch, aTrimmed, sizeof(gs_BestClientShopState.m_aAppliedSearch));
		gs_BestClientShopState.m_aPages[gs_BestClientShopState.m_Tab] = 1;
		BestClientShopInvalidatePage(pMenus);
	}

	static void BestClientShopResolveUrl(const char *pInput, char *pOutput, size_t OutputSize)
	{
		pOutput[0] = '\0';
		if(pInput == nullptr || pInput[0] == '\0')
		{
			return;
		}

		if(str_startswith(pInput, "https://") != nullptr || str_startswith(pInput, "http://") != nullptr)
		{
			str_copy(pOutput, pInput, OutputSize);
		}
		else if(pInput[0] == '/')
		{
			str_format(pOutput, OutputSize, "%s%s", BESTCLIENT_SHOP_HOST, pInput);
		}
		else
		{
			str_format(pOutput, OutputSize, "%s/%s", BESTCLIENT_SHOP_HOST, pInput);
		}
	}

	static void BestClientShopNormalizeAssetName(const char *pName, const char *pFilename, char *pOutput, size_t OutputSize)
	{
		char aRawName[128];
		if(pFilename != nullptr && pFilename[0] != '\0')
		{
			IStorage::StripPathAndExtension(pFilename, aRawName, sizeof(aRawName));
		}
		else if(pName != nullptr && pName[0] != '\0')
		{
			str_copy(aRawName, pName, sizeof(aRawName));
		}
		else
		{
			str_copy(aRawName, "asset", sizeof(aRawName));
		}

		str_sanitize_filename(aRawName);

		char aSanitized[128];
		int WritePos = 0;
		bool LastWasSeparator = true;
		for(int ReadPos = 0; aRawName[ReadPos] != '\0' && WritePos < (int)sizeof(aSanitized) - 1; ++ReadPos)
		{
			unsigned char Character = (unsigned char)aRawName[ReadPos];
			if(Character <= 32)
			{
				if(!LastWasSeparator)
				{
					aSanitized[WritePos++] = '_';
					LastWasSeparator = true;
				}
				continue;
			}

			aSanitized[WritePos++] = Character;
			LastWasSeparator = false;
		}
		aSanitized[WritePos] = '\0';

		while(WritePos > 0 && aSanitized[WritePos - 1] == '_')
		{
			aSanitized[--WritePos] = '\0';
		}

		if(aSanitized[0] == '\0')
		{
			str_copy(aSanitized, "asset", sizeof(aSanitized));
		}

		str_copy(pOutput, aSanitized, OutputSize);
	}

	static void BestClientShopBuildPreviewPath(int Tab, const char *pItemId, char *pOutput, size_t OutputSize)
	{
		str_format(pOutput, OutputSize, "bestclient/shop_previews/%s/%s.png", gs_aBestClientShopTypeInfos[Tab].m_pApiType, pItemId);
	}

	static void BestClientShopBuildAssetPath(int Tab, const char *pAssetName, char *pOutput, size_t OutputSize)
	{
		if(Tab == BESTCLIENT_SHOP_AUDIO)
		{
			str_format(pOutput, OutputSize, "%s/%s", gs_aBestClientShopTypeInfos[Tab].m_pAssetDirectory, pAssetName);
		}
		else
		{
			str_format(pOutput, OutputSize, "%s/%s.png", gs_aBestClientShopTypeInfos[Tab].m_pAssetDirectory, pAssetName);
		}
	}

	static void BestClientShopBuildAssetDirectoryPath(int Tab, const char *pAssetName, char *pOutput, size_t OutputSize)
	{
		str_format(pOutput, OutputSize, "%s/%s", gs_aBestClientShopTypeInfos[Tab].m_pAssetDirectory, pAssetName);
	}

	static bool BestClientShopWriteFile(CMenus *pMenus, const char *pRelativePath, const void *pData, size_t DataSize)
	{
		char aAbsolutePath[IO_MAX_PATH_LENGTH];
		pMenus->MenuStorage()->GetCompletePath(IStorage::TYPE_SAVE, pRelativePath, aAbsolutePath, sizeof(aAbsolutePath));
		if(fs_makedir_rec_for(aAbsolutePath) != 0)
		{
			return false;
		}

		IOHANDLE File = pMenus->MenuStorage()->OpenFile(pRelativePath, IOFLAG_WRITE, IStorage::TYPE_SAVE);
		if(File == nullptr)
		{
			return false;
		}

		const bool Success = io_write(File, pData, DataSize) == DataSize && io_error(File) == 0 && io_close(File) == 0;
		return Success;
	}

	static bool BestClientShopIsPngBuffer(const unsigned char *pData, size_t DataSize)
	{
		static const unsigned char s_aSignature[8] = {0x89, 'P', 'N', 'G', '\r', '\n', 0x1a, '\n'};
		return DataSize >= sizeof(s_aSignature) && mem_comp(pData, s_aSignature, sizeof(s_aSignature)) == 0;
	}

	static bool BestClientShopIsZipBuffer(const unsigned char *pData, size_t DataSize)
	{
		static const unsigned char s_aSignature[4] = {'P', 'K', 0x03, 0x04};
		return DataSize >= sizeof(s_aSignature) && mem_comp(pData, s_aSignature, sizeof(s_aSignature)) == 0;
	}

	static bool BestClientShopDirectoryExists(CMenus *pMenus, const char *pRelativePath)
	{
		char aAbsolutePath[IO_MAX_PATH_LENGTH];
		pMenus->MenuStorage()->GetCompletePath(IStorage::TYPE_SAVE, pRelativePath, aAbsolutePath, sizeof(aAbsolutePath));
		return fs_is_dir(aAbsolutePath) == 1;
	}

	static int BestClientShopCollectDirectoryEntries(const char *pName, int IsDir, int DirType, void *pUser)
	{
		(void)IsDir;
		(void)DirType;
		auto *pEntries = static_cast<std::vector<std::string> *>(pUser);
		if(str_comp(pName, ".") == 0 || str_comp(pName, "..") == 0)
		{
			return 0;
		}
		pEntries->emplace_back(pName);
		return 0;
	}

	static bool BestClientShopRemoveAbsoluteDirectoryRecursive(const char *pAbsolutePath)
	{
		std::vector<std::string> vEntries;
		fs_listdir(pAbsolutePath, BestClientShopCollectDirectoryEntries, 0, &vEntries);

		bool Success = true;
		for(const std::string &Entry : vEntries)
		{
			char aChildPath[IO_MAX_PATH_LENGTH];
			str_format(aChildPath, sizeof(aChildPath), "%s/%s", pAbsolutePath, Entry.c_str());
			if(fs_is_dir(aChildPath) == 1)
			{
				Success &= BestClientShopRemoveAbsoluteDirectoryRecursive(aChildPath);
			}
			else
			{
				Success &= fs_remove(aChildPath) == 0;
			}
		}

		Success &= fs_removedir(pAbsolutePath) == 0;
		return Success;
	}

	static uint16_t BestClientShopReadLe16(const unsigned char *pData)
	{
		return (uint16_t)pData[0] | ((uint16_t)pData[1] << 8);
	}

	static uint32_t BestClientShopReadLe32(const unsigned char *pData)
	{
		return (uint32_t)pData[0] | ((uint32_t)pData[1] << 8) | ((uint32_t)pData[2] << 16) | ((uint32_t)pData[3] << 24);
	}

	static bool BestClientShopSanitizeArchivePath(const std::string &Path, std::string &OutPath)
	{
		OutPath.clear();
		if(Path.empty())
		{
			return false;
		}

		OutPath.reserve(Path.size());
		for(char Ch : Path)
		{
			OutPath.push_back(Ch == '\\' ? '/' : Ch);
		}

		while(!OutPath.empty() && OutPath[0] == '/')
		{
			OutPath.erase(OutPath.begin());
		}

		if(OutPath.empty())
		{
			return false;
		}

		if((OutPath.size() >= 2 && OutPath[1] == ':') || str_startswith(OutPath.c_str(), "../") || str_find(OutPath.c_str(), "/../") != nullptr || str_endswith(OutPath.c_str(), "/.."))
		{
			return false;
		}

		return true;
	}

	static bool BestClientShopExtractZipToDirectory(CMenus *pMenus, const unsigned char *pData, size_t DataSize, const char *pOutputBasePath)
	{
		if(!BestClientShopIsZipBuffer(pData, DataSize) || DataSize < 22)
		{
			return false;
		}

		struct SZipEntry
		{
			std::string m_Path;
			uint32_t m_LocalHeaderOffset = 0;
			uint32_t m_CompressedSize = 0;
			uint32_t m_UncompressedSize = 0;
			uint16_t m_CompressionMethod = 0;
		};

		size_t EocdOffset = (size_t)-1;
		const size_t SearchStart = DataSize > 65557 ? DataSize - 65557 : 0;
		for(size_t Pos = DataSize - 22 + 1; Pos-- > SearchStart;)
		{
			if(Pos + 4 <= DataSize && BestClientShopReadLe32(pData + Pos) == 0x06054b50U)
			{
				EocdOffset = Pos;
				break;
			}
		}

		if(EocdOffset == (size_t)-1 || EocdOffset + 22 > DataSize)
		{
			return false;
		}

		const uint16_t NumEntries = BestClientShopReadLe16(pData + EocdOffset + 10);
		const uint32_t CentralDirSize = BestClientShopReadLe32(pData + EocdOffset + 12);
		const uint32_t CentralDirOffset = BestClientShopReadLe32(pData + EocdOffset + 16);
		if((size_t)CentralDirOffset + (size_t)CentralDirSize > DataSize)
		{
			return false;
		}

		std::vector<SZipEntry> vEntries;
		vEntries.reserve(NumEntries);

		size_t CentralPos = CentralDirOffset;
		for(uint16_t EntryIndex = 0; EntryIndex < NumEntries; ++EntryIndex)
		{
			if(CentralPos + 46 > DataSize || BestClientShopReadLe32(pData + CentralPos) != 0x02014b50U)
			{
				return false;
			}

			const uint16_t CompressionMethod = BestClientShopReadLe16(pData + CentralPos + 10);
			const uint32_t CompressedSize = BestClientShopReadLe32(pData + CentralPos + 20);
			const uint32_t UncompressedSize = BestClientShopReadLe32(pData + CentralPos + 24);
			const uint16_t NameLength = BestClientShopReadLe16(pData + CentralPos + 28);
			const uint16_t ExtraLength = BestClientShopReadLe16(pData + CentralPos + 30);
			const uint16_t CommentLength = BestClientShopReadLe16(pData + CentralPos + 32);
			const uint32_t LocalHeaderOffset = BestClientShopReadLe32(pData + CentralPos + 42);

			const size_t NameOffset = CentralPos + 46;
			const size_t NextEntryPos = NameOffset + NameLength + ExtraLength + CommentLength;
			if(NameOffset + NameLength > DataSize || NextEntryPos > DataSize)
			{
				return false;
			}

			std::string RawPath((const char *)(pData + NameOffset), NameLength);
			std::string SanitizedPath;
			if(!RawPath.empty() && RawPath.back() != '/')
			{
				if(!BestClientShopSanitizeArchivePath(RawPath, SanitizedPath))
				{
					return false;
				}
				vEntries.push_back({SanitizedPath, LocalHeaderOffset, CompressedSize, UncompressedSize, CompressionMethod});
			}

			CentralPos = NextEntryPos;
		}

		if(vEntries.empty())
		{
			return false;
		}

		std::string CommonPrefix;
		{
			std::string Candidate;
			bool CandidateReady = false;
			bool PrefixMismatch = false;
			for(const SZipEntry &Entry : vEntries)
			{
				const size_t SlashPos = Entry.m_Path.find('/');
				if(SlashPos == std::string::npos || SlashPos == 0 || SlashPos + 1 >= Entry.m_Path.size())
				{
					PrefixMismatch = true;
					break;
				}

				const std::string Prefix = Entry.m_Path.substr(0, SlashPos);
				if(!CandidateReady)
				{
					Candidate = Prefix;
					CandidateReady = true;
				}
				else if(Candidate != Prefix)
				{
					PrefixMismatch = true;
					break;
				}
			}

			if(CandidateReady && !PrefixMismatch)
			{
				CommonPrefix = Candidate + "/";
			}
		}

		for(const SZipEntry &Entry : vEntries)
		{
			std::string RelativePath = Entry.m_Path;
			if(!CommonPrefix.empty() && str_startswith(RelativePath.c_str(), CommonPrefix.c_str()) != nullptr)
			{
				RelativePath = RelativePath.substr(CommonPrefix.size());
			}
			if(RelativePath.empty())
			{
				continue;
			}

			const size_t LocalPos = Entry.m_LocalHeaderOffset;
			if(LocalPos + 30 > DataSize || BestClientShopReadLe32(pData + LocalPos) != 0x04034b50U)
			{
				return false;
			}

			const uint16_t LocalNameLength = BestClientShopReadLe16(pData + LocalPos + 26);
			const uint16_t LocalExtraLength = BestClientShopReadLe16(pData + LocalPos + 28);
			const size_t CompressedOffset = LocalPos + 30 + LocalNameLength + LocalExtraLength;
			if(CompressedOffset + (size_t)Entry.m_CompressedSize > DataSize)
			{
				return false;
			}

			const unsigned char *pCompressedData = pData + CompressedOffset;
			std::vector<unsigned char> vFileData;
			if(Entry.m_CompressionMethod == 0)
			{
				vFileData.assign(pCompressedData, pCompressedData + Entry.m_CompressedSize);
			}
			else if(Entry.m_CompressionMethod == 8)
			{
				if(Entry.m_UncompressedSize == 0)
				{
					vFileData.clear();
				}
				else
				{
					vFileData.resize(Entry.m_UncompressedSize);
					z_stream Stream = {};
					Stream.next_in = const_cast<Bytef *>(reinterpret_cast<const Bytef *>(pCompressedData));
					Stream.avail_in = Entry.m_CompressedSize;
					Stream.next_out = reinterpret_cast<Bytef *>(vFileData.data());
					Stream.avail_out = Entry.m_UncompressedSize;

					if(inflateInit2(&Stream, -MAX_WBITS) != Z_OK)
					{
						return false;
					}
					const int InflateResult = inflate(&Stream, Z_FINISH);
					inflateEnd(&Stream);
					if(InflateResult != Z_STREAM_END || Stream.total_out != Entry.m_UncompressedSize)
					{
						return false;
					}
				}
			}
			else
			{
				return false;
			}

			char aOutputPath[IO_MAX_PATH_LENGTH];
			str_format(aOutputPath, sizeof(aOutputPath), "%s/%s", pOutputBasePath, RelativePath.c_str());
			if(!BestClientShopWriteFile(pMenus, aOutputPath, vFileData.data(), vFileData.size()))
			{
				return false;
			}
		}

		return true;
	}

	static bool BestClientShopAssetExists(CMenus *pMenus, int Tab, const char *pAssetName)
	{
		if(Tab == BESTCLIENT_SHOP_AUDIO)
		{
			char aDirectoryPath[IO_MAX_PATH_LENGTH];
			BestClientShopBuildAssetDirectoryPath(Tab, pAssetName, aDirectoryPath, sizeof(aDirectoryPath));
			if(BestClientShopDirectoryExists(pMenus, aDirectoryPath))
			{
				return true;
			}
			str_format(aDirectoryPath, sizeof(aDirectoryPath), "audio/%s", pAssetName);
			return BestClientShopDirectoryExists(pMenus, aDirectoryPath);
		}

		if(Tab == BESTCLIENT_SHOP_ARROWS)
		{
			char aPath[IO_MAX_PATH_LENGTH];
			str_format(aPath, sizeof(aPath), "assets/arrow/%s.png", pAssetName);
			if(pMenus->MenuStorage()->FileExists(aPath, IStorage::TYPE_ALL))
			{
				return true;
			}
			str_format(aPath, sizeof(aPath), "assets/arrow/%s/arrow.png", pAssetName);
			if(pMenus->MenuStorage()->FileExists(aPath, IStorage::TYPE_ALL))
			{
				return true;
			}
			str_format(aPath, sizeof(aPath), "assets/arrows/%s.png", pAssetName);
			if(pMenus->MenuStorage()->FileExists(aPath, IStorage::TYPE_ALL))
			{
				return true;
			}
			str_format(aPath, sizeof(aPath), "assets/arrows/%s/arrow.png", pAssetName);
			return pMenus->MenuStorage()->FileExists(aPath, IStorage::TYPE_ALL);
		}

		char aPath[IO_MAX_PATH_LENGTH];
		BestClientShopBuildAssetPath(Tab, pAssetName, aPath, sizeof(aPath));
		if(pMenus->MenuStorage()->FileExists(aPath, IStorage::TYPE_ALL))
		{
			return true;
		}

		char aDirectoryPath[IO_MAX_PATH_LENGTH];
		BestClientShopBuildAssetDirectoryPath(Tab, pAssetName, aDirectoryPath, sizeof(aDirectoryPath));
		return BestClientShopDirectoryExists(pMenus, aDirectoryPath);
	}

	static bool BestClientShopAssetSelected(int Tab, const char *pAssetName)
	{
		switch(Tab)
		{
		case BESTCLIENT_SHOP_ENTITIES:
			return str_comp(g_Config.m_ClAssetsEntities, pAssetName) == 0;
		case BESTCLIENT_SHOP_GAME:
			return str_comp(g_Config.m_ClAssetGame, pAssetName) == 0;
		case BESTCLIENT_SHOP_EMOTICONS:
			return str_comp(g_Config.m_ClAssetEmoticons, pAssetName) == 0;
		case BESTCLIENT_SHOP_PARTICLES:
			return str_comp(g_Config.m_ClAssetParticles, pAssetName) == 0;
		case BESTCLIENT_SHOP_HUD:
			return str_comp(g_Config.m_ClAssetHud, pAssetName) == 0;
		case BESTCLIENT_SHOP_ARROWS:
			return str_comp(g_Config.m_ClAssetArrow, pAssetName) == 0;
		case BESTCLIENT_SHOP_CURSORS:
			return str_comp(g_Config.m_ClAssetCursor, pAssetName) == 0;
		case BESTCLIENT_SHOP_AUDIO:
			return str_comp(g_Config.m_SndPack, pAssetName) == 0;
		default:
			return false;
		}
	}

	static void BestClientShopReloadAsset(CMenus *pMenus, int Tab, const char *pAssetName)
	{
		switch(Tab)
		{
		case BESTCLIENT_SHOP_ENTITIES:
			pMenus->MenuGameClient()->m_MapImages.ChangeEntitiesPath(pAssetName);
			break;
		case BESTCLIENT_SHOP_GAME:
			pMenus->MenuGameClient()->LoadGameSkin(pAssetName);
			break;
		case BESTCLIENT_SHOP_EMOTICONS:
			pMenus->MenuGameClient()->LoadEmoticonsSkin(pAssetName);
			break;
		case BESTCLIENT_SHOP_PARTICLES:
			pMenus->MenuGameClient()->LoadParticlesSkin(pAssetName);
			break;
		case BESTCLIENT_SHOP_HUD:
			pMenus->MenuGameClient()->LoadHudSkin(pAssetName);
			break;
		case BESTCLIENT_SHOP_ARROWS:
			pMenus->MenuGameClient()->LoadArrowAsset(pAssetName);
			break;
		case BESTCLIENT_SHOP_CURSORS:
			pMenus->MenuGameClient()->LoadCursorAsset(pAssetName);
			break;
		case BESTCLIENT_SHOP_AUDIO:
			pMenus->MenuGameClient()->m_Sounds.Clear();
			break;
		}
	}

	static void BestClientShopApplyAsset(CMenus *pMenus, int Tab, const char *pAssetName, bool RefreshAssetList)
	{
		switch(Tab)
		{
		case BESTCLIENT_SHOP_ENTITIES:
			str_copy(g_Config.m_ClAssetsEntities, pAssetName);
			break;
		case BESTCLIENT_SHOP_GAME:
			str_copy(g_Config.m_ClAssetGame, pAssetName);
			break;
		case BESTCLIENT_SHOP_EMOTICONS:
			str_copy(g_Config.m_ClAssetEmoticons, pAssetName);
			break;
		case BESTCLIENT_SHOP_PARTICLES:
			str_copy(g_Config.m_ClAssetParticles, pAssetName);
			break;
		case BESTCLIENT_SHOP_HUD:
			str_copy(g_Config.m_ClAssetHud, pAssetName);
			break;
		case BESTCLIENT_SHOP_ARROWS:
			str_copy(g_Config.m_ClAssetArrow, pAssetName);
			break;
		case BESTCLIENT_SHOP_CURSORS:
			str_copy(g_Config.m_ClAssetCursor, pAssetName);
			break;
		case BESTCLIENT_SHOP_AUDIO:
			str_copy(g_Config.m_SndPack, pAssetName);
			break;
		default:
			return;
		}

		if(RefreshAssetList)
		{
			pMenus->RefreshCustomAssetsTab(gs_aBestClientShopTypeInfos[Tab].m_AssetsTab);
		}
		else
		{
			BestClientShopReloadAsset(pMenus, Tab, pAssetName);
		}
	}

	static bool BestClientShopDeleteAsset(CMenus *pMenus, int Tab, const char *pAssetName)
	{
		bool AnyDeleted = false;
		bool DeleteSuccess = true;

		if(Tab == BESTCLIENT_SHOP_AUDIO)
		{
			const char *apAudioDirectories[] = {
				"assets/audio/%s",
				"audio/%s",
			};
			for(int i = 0; i < (int)std::size(apAudioDirectories); ++i)
			{
				char aDirectoryPath[IO_MAX_PATH_LENGTH];
				if(i == 0)
					str_format(aDirectoryPath, sizeof(aDirectoryPath), "assets/audio/%s", pAssetName);
				else
					str_format(aDirectoryPath, sizeof(aDirectoryPath), "audio/%s", pAssetName);
				char aAbsolutePath[IO_MAX_PATH_LENGTH];
				pMenus->MenuStorage()->GetCompletePath(IStorage::TYPE_SAVE, aDirectoryPath, aAbsolutePath, sizeof(aAbsolutePath));
				if(fs_is_dir(aAbsolutePath) == 1)
				{
					AnyDeleted = true;
					DeleteSuccess &= BestClientShopRemoveAbsoluteDirectoryRecursive(aAbsolutePath);
				}
			}
		}
		else if(Tab == BESTCLIENT_SHOP_ARROWS)
		{
			char aPath[IO_MAX_PATH_LENGTH];
			const char *apArrowPaths[] = {
				"assets/arrow/%s.png",
				"assets/arrow/%s/arrow.png",
				"assets/arrows/%s.png",
				"assets/arrows/%s/arrow.png",
			};
			for(int i = 0; i < (int)std::size(apArrowPaths); ++i)
			{
				if(i == 0)
					str_format(aPath, sizeof(aPath), "assets/arrow/%s.png", pAssetName);
				else if(i == 1)
					str_format(aPath, sizeof(aPath), "assets/arrow/%s/arrow.png", pAssetName);
				else if(i == 2)
					str_format(aPath, sizeof(aPath), "assets/arrows/%s.png", pAssetName);
				else
					str_format(aPath, sizeof(aPath), "assets/arrows/%s/arrow.png", pAssetName);
				if(pMenus->MenuStorage()->FileExists(aPath, IStorage::TYPE_SAVE))
				{
					AnyDeleted = true;
					DeleteSuccess &= pMenus->MenuStorage()->RemoveFile(aPath, IStorage::TYPE_SAVE);
				}
			}

			const char *apArrowDirectories[] = {
				"assets/arrow/%s",
				"assets/arrows/%s",
			};
			for(int i = 0; i < (int)std::size(apArrowDirectories); ++i)
			{
				char aDirectoryPath[IO_MAX_PATH_LENGTH];
				if(i == 0)
					str_format(aDirectoryPath, sizeof(aDirectoryPath), "assets/arrow/%s", pAssetName);
				else
					str_format(aDirectoryPath, sizeof(aDirectoryPath), "assets/arrows/%s", pAssetName);
				char aAbsolutePath[IO_MAX_PATH_LENGTH];
				pMenus->MenuStorage()->GetCompletePath(IStorage::TYPE_SAVE, aDirectoryPath, aAbsolutePath, sizeof(aAbsolutePath));
				if(fs_is_dir(aAbsolutePath) == 1)
				{
					AnyDeleted = true;
					DeleteSuccess &= BestClientShopRemoveAbsoluteDirectoryRecursive(aAbsolutePath);
				}
			}
		}
		else
		{
			char aPath[IO_MAX_PATH_LENGTH];
			BestClientShopBuildAssetPath(Tab, pAssetName, aPath, sizeof(aPath));
			if(pMenus->MenuStorage()->FileExists(aPath, IStorage::TYPE_SAVE))
			{
				AnyDeleted = true;
				DeleteSuccess = pMenus->MenuStorage()->RemoveFile(aPath, IStorage::TYPE_SAVE);
			}
			else
			{
				char aDirectoryPath[IO_MAX_PATH_LENGTH];
				BestClientShopBuildAssetDirectoryPath(Tab, pAssetName, aDirectoryPath, sizeof(aDirectoryPath));
				char aAbsolutePath[IO_MAX_PATH_LENGTH];
				pMenus->MenuStorage()->GetCompletePath(IStorage::TYPE_SAVE, aDirectoryPath, aAbsolutePath, sizeof(aAbsolutePath));
				if(fs_is_dir(aAbsolutePath) == 1)
				{
					AnyDeleted = true;
					DeleteSuccess = BestClientShopRemoveAbsoluteDirectoryRecursive(aAbsolutePath);
				}
				else
				{
					return false;
				}
			}
		}

		if(!AnyDeleted)
		{
			return false;
		}

		if(BestClientShopAssetSelected(Tab, pAssetName))
		{
			BestClientShopApplyAsset(pMenus, Tab, "default", true);
		}
		else
		{
			pMenus->RefreshCustomAssetsTab(gs_aBestClientShopTypeInfos[Tab].m_AssetsTab);
		}

		return DeleteSuccess;
	}

	static void BestClientShopOpenAssetDirectory(CMenus *pMenus, int Tab)
	{
		const char *pRelativePath = gs_aBestClientShopTypeInfos[Tab].m_pAssetDirectory;
		pMenus->MenuStorage()->CreateFolder("assets", IStorage::TYPE_SAVE);
		pMenus->MenuStorage()->CreateFolder(pRelativePath, IStorage::TYPE_SAVE);

		char aAbsolutePath[IO_MAX_PATH_LENGTH];
		pMenus->MenuStorage()->GetCompletePath(IStorage::TYPE_SAVE, pRelativePath, aAbsolutePath, sizeof(aAbsolutePath));
		pMenus->MenuClient()->ViewFile(aAbsolutePath);
	}

	static SBestClientShopItem *BestClientShopFindItem(const char *pItemId)
	{
		for(SBestClientShopItem &Item : gs_BestClientShopState.m_vItems)
		{
			if(str_comp(Item.m_aId, pItemId) == 0)
			{
				return &Item;
			}
		}
		return nullptr;
	}

	static bool BestClientShopLoadPreviewTexture(CMenus *pMenus, SBestClientShopItem &Item, int Tab)
	{
		if(Tab == BESTCLIENT_SHOP_AUDIO)
		{
			return false;
		}

		if(Item.m_PreviewTexture.IsValid() || Item.m_aId[0] == '\0')
		{
			return Item.m_PreviewTexture.IsValid();
		}

		char aPreviewPath[IO_MAX_PATH_LENGTH];
		BestClientShopBuildPreviewPath(Tab, Item.m_aId, aPreviewPath, sizeof(aPreviewPath));
		if(!pMenus->MenuStorage()->FileExists(aPreviewPath, IStorage::TYPE_SAVE))
		{
			return false;
		}

		Item.m_PreviewTexture = pMenus->MenuGraphics()->LoadTexture(aPreviewPath, IStorage::TYPE_SAVE);
		CImageInfo PreviewInfo;
		if(pMenus->MenuGraphics()->LoadPng(PreviewInfo, aPreviewPath, IStorage::TYPE_SAVE))
		{
			Item.m_PreviewWidth = PreviewInfo.m_Width;
			Item.m_PreviewHeight = PreviewInfo.m_Height;
			PreviewInfo.Free();
		}
		return Item.m_PreviewTexture.IsValid() && !Item.m_PreviewTexture.IsNullTexture();
	}

	static bool BestClientShopCalcFittedRect(const CUIRect &Rect, int SourceWidth, int SourceHeight, CUIRect &OutRect)
	{
		if(SourceWidth <= 0 || SourceHeight <= 0 || Rect.w <= 0.0f || Rect.h <= 0.0f)
		{
			return false;
		}

		const float SourceAspect = (float)SourceWidth / (float)SourceHeight;
		const float RectAspect = Rect.w / Rect.h;

		OutRect = Rect;
		if(SourceAspect > RectAspect)
		{
			OutRect.h = Rect.w / SourceAspect;
			OutRect.y = Rect.y + (Rect.h - OutRect.h) * 0.5f;
		}
		else
		{
			OutRect.w = Rect.h * SourceAspect;
			OutRect.x = Rect.x + (Rect.w - OutRect.w) * 0.5f;
		}
		return true;
	}

	static void BestClientShopDrawFittedTexture(IGraphics *pGraphics, IGraphics::CTextureHandle Texture, const CUIRect &Rect, int SourceWidth, int SourceHeight)
	{
		if(!Texture.IsValid() || Texture.IsNullTexture())
		{
			return;
		}

		CUIRect FittedRect;
		if(!BestClientShopCalcFittedRect(Rect, SourceWidth, SourceHeight, FittedRect))
		{
			FittedRect = Rect;
		}

		pGraphics->WrapClamp();
		pGraphics->TextureSet(Texture);
		pGraphics->QuadsBegin();
		pGraphics->SetColor(1.0f, 1.0f, 1.0f, 1.0f);
		const IGraphics::CQuadItem QuadItem(FittedRect.x, FittedRect.y, FittedRect.w, FittedRect.h);
		pGraphics->QuadsDrawTL(&QuadItem, 1);
		pGraphics->QuadsEnd();
		pGraphics->WrapNormal();
	}

	static float BestClientShopListPreviewWidth(const SBestClientShopItem &Item, int Tab)
	{
		float Width = Tab == BESTCLIENT_SHOP_GAME ? 112.0f : 64.0f;
		if(Item.m_PreviewWidth > 0 && Item.m_PreviewHeight > 0)
		{
			const float Aspect = (float)Item.m_PreviewWidth / (float)Item.m_PreviewHeight;
			if(Aspect > 1.0f)
			{
				Width = std::clamp((BESTCLIENT_SHOP_ITEM_HEIGHT - 12.0f) * Aspect, Width, 132.0f);
			}
		}
		return Width;
	}

	static void BestClientShopBuildInstallUrls(int Tab, const SBestClientShopItem &Item, std::vector<std::string> &vUrls)
	{
		vUrls.clear();

		auto AddUrl = [&vUrls](const char *pUrl) {
			if(pUrl == nullptr || pUrl[0] == '\0')
			{
				return;
			}

			char aResolvedUrl[256];
			BestClientShopResolveUrl(pUrl, aResolvedUrl, sizeof(aResolvedUrl));
			if(aResolvedUrl[0] == '\0')
			{
				return;
			}

			const std::string Url = aResolvedUrl;
			if(std::find(vUrls.begin(), vUrls.end(), Url) == vUrls.end())
			{
				vUrls.push_back(Url);
			}
		};

		char aFallbackUrl[256];
		if(Tab == BESTCLIENT_SHOP_AUDIO)
		{
			AddUrl(Item.m_aDownloadUrl);

			str_format(aFallbackUrl, sizeof(aFallbackUrl), "%s/api/skins/%s?download=true", BESTCLIENT_SHOP_HOST, Item.m_aId);
			AddUrl(aFallbackUrl);

			str_format(aFallbackUrl, sizeof(aFallbackUrl), "%s/api/skins/%s?file=true", BESTCLIENT_SHOP_HOST, Item.m_aId);
			AddUrl(aFallbackUrl);
		}

		AddUrl(Item.m_aImageUrl);

		str_format(aFallbackUrl, sizeof(aFallbackUrl), "%s/api/skins/%s?image=true", BESTCLIENT_SHOP_HOST, Item.m_aId);
		AddUrl(aFallbackUrl);
	}

	static void BestClientShopStartFetch(CMenus *pMenus)
	{
		const char *pApiType = gs_aBestClientShopTypeInfos[gs_BestClientShopState.m_Tab].m_pApiType;
		const int CurrentPage = gs_BestClientShopState.m_aPages[gs_BestClientShopState.m_Tab];
		char aUrl[512];
		if(BestClientShopHasActiveSearch())
		{
			char aEscapedQuery[384];
			EscapeUrl(aEscapedQuery, sizeof(aEscapedQuery), gs_BestClientShopState.m_aAppliedSearch);
			str_format(aUrl, sizeof(aUrl), BESTCLIENT_SHOP_SEARCH_API_URL, CurrentPage, pApiType, aEscapedQuery);
		}
		else
		{
			str_format(aUrl, sizeof(aUrl), BESTCLIENT_SHOP_BROWSE_API_URL, CurrentPage, pApiType);
		}

		gs_BestClientShopState.m_pFetchTask = HttpGet(aUrl);
		gs_BestClientShopState.m_pFetchTask->Timeout(BESTCLIENT_SHOP_TIMEOUT);
		gs_BestClientShopState.m_pFetchTask->IpResolve(IPRESOLVE::V4);
		gs_BestClientShopState.m_pFetchTask->MaxResponseSize(BESTCLIENT_SHOP_PAGE_MAX_RESPONSE_SIZE);
		gs_BestClientShopState.m_pFetchTask->LogProgress(HTTPLOG::NONE);
		gs_BestClientShopState.m_pFetchTask->FailOnErrorStatus(false);
		gs_BestClientShopState.m_FetchTab = gs_BestClientShopState.m_Tab;
		gs_BestClientShopState.m_FetchPage = CurrentPage;
		str_copy(gs_BestClientShopState.m_aFetchSearch, gs_BestClientShopState.m_aAppliedSearch, sizeof(gs_BestClientShopState.m_aFetchSearch));
		BestClientShopSetStatus(TCLocalize("Loading shop..."));
		pMenus->MenuHttp()->Run(gs_BestClientShopState.m_pFetchTask);
	}

	static void BestClientShopEnsureFetch(CMenus *pMenus)
	{
		if(gs_BestClientShopState.m_pFetchTask)
		{
			return;
		}

		const int CurrentPage = gs_BestClientShopState.m_aPages[gs_BestClientShopState.m_Tab];
		if(gs_BestClientShopState.m_LoadedTab != gs_BestClientShopState.m_Tab ||
			gs_BestClientShopState.m_LoadedPage != CurrentPage ||
			str_comp(gs_BestClientShopState.m_aLoadedSearch, gs_BestClientShopState.m_aAppliedSearch) != 0)
		{
			BestClientShopStartFetch(pMenus);
		}
	}

	static void BestClientShopStartPreviewFetch(CMenus *pMenus)
	{
		if(gs_BestClientShopState.m_Tab == BESTCLIENT_SHOP_AUDIO)
		{
			return;
		}

		if(gs_BestClientShopState.m_pPreviewTask != nullptr)
		{
			return;
		}

		for(SBestClientShopItem &Item : gs_BestClientShopState.m_vItems)
		{
			if(Item.m_PreviewFailed)
			{
				continue;
			}
			if(BestClientShopLoadPreviewTexture(pMenus, Item, gs_BestClientShopState.m_Tab))
			{
				continue;
			}

			char aPreviewUrl[256];
			if(Item.m_aImageUrl[0] != '\0')
			{
				BestClientShopResolveUrl(Item.m_aImageUrl, aPreviewUrl, sizeof(aPreviewUrl));
			}
			else
			{
				str_format(aPreviewUrl, sizeof(aPreviewUrl), "%s/api/skins/%s?image=true", BESTCLIENT_SHOP_HOST, Item.m_aId);
			}

			if(aPreviewUrl[0] == '\0')
			{
				Item.m_PreviewFailed = true;
				continue;
			}

			BestClientShopBuildPreviewPath(gs_BestClientShopState.m_Tab, Item.m_aId, gs_BestClientShopState.m_aPreviewPath, sizeof(gs_BestClientShopState.m_aPreviewPath));
			str_copy(gs_BestClientShopState.m_aPreviewItemId, Item.m_aId, sizeof(gs_BestClientShopState.m_aPreviewItemId));
			gs_BestClientShopState.m_PreviewTab = gs_BestClientShopState.m_Tab;
			gs_BestClientShopState.m_PreviewPage = gs_BestClientShopState.m_aPages[gs_BestClientShopState.m_Tab];
			str_copy(gs_BestClientShopState.m_aPreviewSearch, gs_BestClientShopState.m_aAppliedSearch, sizeof(gs_BestClientShopState.m_aPreviewSearch));

			gs_BestClientShopState.m_pPreviewTask = HttpGet(aPreviewUrl);
			gs_BestClientShopState.m_pPreviewTask->Timeout(BESTCLIENT_SHOP_TIMEOUT);
			gs_BestClientShopState.m_pPreviewTask->IpResolve(IPRESOLVE::V4);
			gs_BestClientShopState.m_pPreviewTask->MaxResponseSize(BESTCLIENT_SHOP_IMAGE_MAX_RESPONSE_SIZE);
			gs_BestClientShopState.m_pPreviewTask->LogProgress(HTTPLOG::NONE);
			gs_BestClientShopState.m_pPreviewTask->FailOnErrorStatus(false);
			pMenus->MenuHttp()->Run(gs_BestClientShopState.m_pPreviewTask);
			return;
		}
	}

	static void BestClientShopFinishPreviewFetch(CMenus *pMenus)
	{
		if(!gs_BestClientShopState.m_pPreviewTask || !gs_BestClientShopState.m_pPreviewTask->Done())
		{
			return;
		}

		SBestClientShopItem *pItem = BestClientShopFindItem(gs_BestClientShopState.m_aPreviewItemId);
		const bool SamePage = gs_BestClientShopState.m_PreviewTab == gs_BestClientShopState.m_Tab &&
				      gs_BestClientShopState.m_PreviewPage == gs_BestClientShopState.m_aPages[gs_BestClientShopState.m_Tab] &&
				      str_comp(gs_BestClientShopState.m_aPreviewSearch, gs_BestClientShopState.m_aAppliedSearch) == 0;

		if(gs_BestClientShopState.m_pPreviewTask->State() != EHttpState::DONE)
		{
			if(pItem != nullptr)
			{
				pItem->m_PreviewFailed = true;
			}
			BestClientShopAbortPreviewTask();
			return;
		}

		if(pItem != nullptr)
		{
			if(!SamePage || gs_BestClientShopState.m_pPreviewTask->StatusCode() >= 400)
			{
				pItem->m_PreviewFailed = true;
			}
			else
			{
				unsigned char *pResult = nullptr;
				size_t ResultLength = 0;
				gs_BestClientShopState.m_pPreviewTask->Result(&pResult, &ResultLength);
				if(pResult != nullptr && ResultLength > 0 && BestClientShopIsPngBuffer(pResult, ResultLength) &&
					BestClientShopWriteFile(pMenus, gs_BestClientShopState.m_aPreviewPath, pResult, ResultLength) &&
					BestClientShopLoadPreviewTexture(pMenus, *pItem, gs_BestClientShopState.m_Tab))
				{
					pItem->m_PreviewFailed = false;
				}
				else
				{
					pItem->m_PreviewFailed = true;
				}
			}
		}

		BestClientShopAbortPreviewTask();
	}

	static void BestClientShopFinishFetch(CMenus *pMenus)
	{
		if(!gs_BestClientShopState.m_pFetchTask || !gs_BestClientShopState.m_pFetchTask->Done())
		{
			return;
		}

		gs_BestClientShopState.m_LoadedTab = gs_BestClientShopState.m_FetchTab;
		gs_BestClientShopState.m_LoadedPage = gs_BestClientShopState.m_FetchPage;
		str_copy(gs_BestClientShopState.m_aLoadedSearch, gs_BestClientShopState.m_aFetchSearch, sizeof(gs_BestClientShopState.m_aLoadedSearch));

		if(gs_BestClientShopState.m_pFetchTask->State() != EHttpState::DONE)
		{
			BestClientShopClearItems(pMenus);
			gs_BestClientShopState.m_TotalPages = 1;
			gs_BestClientShopState.m_TotalItems = 0;
			BestClientShopSetStatus(TCLocalize("Shop request failed"));
			return;
		}

		const int FetchStatusCode = gs_BestClientShopState.m_pFetchTask->StatusCode();
		if(FetchStatusCode >= 400)
		{
			str_format(gs_BestClientShopState.m_aStatus, sizeof(gs_BestClientShopState.m_aStatus), "%s (%d)", TCLocalize("Shop request failed"), FetchStatusCode);
			return;
		}

		json_value *pJson = gs_BestClientShopState.m_pFetchTask->ResultJson();
		if(pJson == nullptr || pJson->type != json_object)
		{
			if(pJson != nullptr)
			{
				json_value_free(pJson);
			}
			BestClientShopSetStatus(TCLocalize("Shop response is invalid"));
			return;
		}

		std::vector<SBestClientShopItem> vItems;
		const json_value *pSkins = json_object_get(pJson, "skins");
		if(pSkins != &json_value_none && pSkins->type == json_array)
		{
			for(int Index = 0; Index < json_array_length(pSkins); ++Index)
			{
				const json_value *pSkin = json_array_get(pSkins, Index);
				if(pSkin == &json_value_none || pSkin->type != json_object)
				{
					continue;
				}

				const char *pStatus = json_string_get(json_object_get(pSkin, "status"));
				if(pStatus != nullptr && pStatus[0] != '\0' && str_comp(pStatus, "approved") != 0)
				{
					continue;
				}

				SBestClientShopItem Item;
				if(const char *pId = json_string_get(json_object_get(pSkin, "id")); pId != nullptr)
				{
					str_copy(Item.m_aId, pId, sizeof(Item.m_aId));
				}
				if(const char *pName = json_string_get(json_object_get(pSkin, "name")); pName != nullptr)
				{
					str_copy(Item.m_aName, pName, sizeof(Item.m_aName));
				}
				if(const char *pFilename = json_string_get(json_object_get(pSkin, "filename")); pFilename != nullptr)
				{
					str_copy(Item.m_aFilename, pFilename, sizeof(Item.m_aFilename));
				}
				if(const char *pUsername = json_string_get(json_object_get(pSkin, "username")); pUsername != nullptr)
				{
					str_copy(Item.m_aUsername, pUsername, sizeof(Item.m_aUsername));
				}
				if(const char *pImageUrl = json_string_get(json_object_get(pSkin, "imageUrl")); pImageUrl != nullptr)
				{
					str_copy(Item.m_aImageUrl, pImageUrl, sizeof(Item.m_aImageUrl));
				}
				if(const char *pDownloadUrl = json_string_get(json_object_get(pSkin, "downloadUrl")); pDownloadUrl != nullptr)
				{
					str_copy(Item.m_aDownloadUrl, pDownloadUrl, sizeof(Item.m_aDownloadUrl));
				}
				else if(const char *pFileUrl = json_string_get(json_object_get(pSkin, "fileUrl")); pFileUrl != nullptr)
				{
					str_copy(Item.m_aDownloadUrl, pFileUrl, sizeof(Item.m_aDownloadUrl));
				}
				if(Item.m_aId[0] == '\0')
				{
					continue;
				}
				if(Item.m_aName[0] == '\0')
				{
					str_copy(Item.m_aName, Item.m_aFilename, sizeof(Item.m_aName));
				}

				vItems.push_back(Item);
			}
		}

		int TotalPages = 1;
		const json_value *pTotalPages = json_object_get(pJson, "totalPages");
		if(pTotalPages != &json_value_none && pTotalPages->type == json_integer)
		{
			TotalPages = maximum(1, json_int_get(pTotalPages));
		}

		int TotalItems = (int)vItems.size();
		const json_value *pTotal = json_object_get(pJson, "total");
		if(pTotal != &json_value_none && pTotal->type == json_integer)
		{
			TotalItems = maximum(0, json_int_get(pTotal));
		}

		BestClientShopClearItems(pMenus);
		gs_BestClientShopState.m_vItems = std::move(vItems);
		gs_BestClientShopState.m_SelectedIndex = gs_BestClientShopState.m_vItems.empty() ? -1 : 0;
		gs_BestClientShopState.m_TotalPages = TotalPages;
		gs_BestClientShopState.m_TotalItems = TotalItems;

		if(gs_BestClientShopState.m_aPages[gs_BestClientShopState.m_Tab] > gs_BestClientShopState.m_TotalPages)
		{
			gs_BestClientShopState.m_aPages[gs_BestClientShopState.m_Tab] = gs_BestClientShopState.m_TotalPages;
		}

		if(gs_BestClientShopState.m_vItems.empty())
		{
			BestClientShopSetStatus(TCLocalize("No assets found on this page"));
		}
		else
		{
			gs_BestClientShopState.m_aStatus[0] = '\0';
		}

		json_value_free(pJson);
	}

	static bool BestClientShopInstallDownloadedData(CMenus *pMenus, const unsigned char *pData, size_t DataSize)
	{
		if(gs_BestClientShopState.m_InstallTab < 0 || gs_BestClientShopState.m_InstallTab >= NUM_BESTCLIENT_SHOP_TABS)
		{
			return false;
		}

		const int InstallTab = gs_BestClientShopState.m_InstallTab;
		if(BestClientShopIsPngBuffer(pData, DataSize))
		{
			if(InstallTab == BESTCLIENT_SHOP_AUDIO)
			{
				return false;
			}

			char aAssetPath[IO_MAX_PATH_LENGTH];
			BestClientShopBuildAssetPath(InstallTab, gs_BestClientShopState.m_aInstallAssetName, aAssetPath, sizeof(aAssetPath));
			return BestClientShopWriteFile(pMenus, aAssetPath, pData, DataSize);
		}

		if(BestClientShopIsZipBuffer(pData, DataSize))
		{
			char aAssetDirectoryPath[IO_MAX_PATH_LENGTH];
			BestClientShopBuildAssetDirectoryPath(InstallTab, gs_BestClientShopState.m_aInstallAssetName, aAssetDirectoryPath, sizeof(aAssetDirectoryPath));
			char aAbsolutePath[IO_MAX_PATH_LENGTH];
			pMenus->MenuStorage()->GetCompletePath(IStorage::TYPE_SAVE, aAssetDirectoryPath, aAbsolutePath, sizeof(aAbsolutePath));
			if(fs_is_dir(aAbsolutePath) == 1 && !BestClientShopRemoveAbsoluteDirectoryRecursive(aAbsolutePath))
			{
				return false;
			}
			return BestClientShopExtractZipToDirectory(pMenus, pData, DataSize, aAssetDirectoryPath);
		}

		return false;
	}

	static void BestClientShopStartInstallRequest(CMenus *pMenus)
	{
		if(gs_BestClientShopState.m_InstallUrlIndex < 0 || gs_BestClientShopState.m_InstallUrlIndex >= (int)gs_BestClientShopState.m_vInstallUrls.size())
		{
			BestClientShopSetStatus(TCLocalize("Failed to build a download URL"));
			return;
		}

		const std::string &Url = gs_BestClientShopState.m_vInstallUrls[gs_BestClientShopState.m_InstallUrlIndex];
		gs_BestClientShopState.m_pInstallTask = HttpGet(Url.c_str());
		gs_BestClientShopState.m_pInstallTask->Timeout(BESTCLIENT_SHOP_TIMEOUT);
		gs_BestClientShopState.m_pInstallTask->IpResolve(IPRESOLVE::V4);
		const int64_t MaxResponseSize = gs_BestClientShopState.m_InstallTab == BESTCLIENT_SHOP_AUDIO ?
							BESTCLIENT_SHOP_AUDIO_MAX_RESPONSE_SIZE :
							BESTCLIENT_SHOP_IMAGE_MAX_RESPONSE_SIZE;
		gs_BestClientShopState.m_pInstallTask->MaxResponseSize(MaxResponseSize);
		gs_BestClientShopState.m_pInstallTask->LogProgress(HTTPLOG::NONE);
		gs_BestClientShopState.m_pInstallTask->FailOnErrorStatus(false);

		char aMessage[256];
		str_format(aMessage, sizeof(aMessage), "%s: %s", TCLocalize("Downloading"), gs_BestClientShopState.m_aInstallAssetName);
		BestClientShopSetStatus(aMessage);
		pMenus->MenuHttp()->Run(gs_BestClientShopState.m_pInstallTask);
	}

	static void BestClientShopStartInstall(CMenus *pMenus, int Tab, const SBestClientShopItem &Item)
	{
		BestClientShopAbortTask(gs_BestClientShopState.m_pInstallTask);
		BestClientShopResetInstallState();
		gs_BestClientShopState.m_InstallTab = Tab;
		str_copy(gs_BestClientShopState.m_aInstallItemId, Item.m_aId, sizeof(gs_BestClientShopState.m_aInstallItemId));
		BestClientShopNormalizeAssetName(Item.m_aName, Item.m_aFilename, gs_BestClientShopState.m_aInstallAssetName, sizeof(gs_BestClientShopState.m_aInstallAssetName));
		BestClientShopBuildInstallUrls(Tab, Item, gs_BestClientShopState.m_vInstallUrls);
		BestClientShopStartInstallRequest(pMenus);
	}

	static void BestClientShopRetryInstall(CMenus *pMenus)
	{
		BestClientShopAbortTask(gs_BestClientShopState.m_pInstallTask);
		++gs_BestClientShopState.m_InstallUrlIndex;
		if(gs_BestClientShopState.m_InstallUrlIndex >= (int)gs_BestClientShopState.m_vInstallUrls.size())
		{
			char aMessage[256];
			str_format(aMessage, sizeof(aMessage), "%s: %s", TCLocalize("Unable to install asset"), gs_BestClientShopState.m_aInstallAssetName);
			BestClientShopSetStatus(aMessage);
			BestClientShopResetInstallState();
			return;
		}

		BestClientShopStartInstallRequest(pMenus);
	}

	static void BestClientShopFinishInstall(CMenus *pMenus)
	{
		if(!gs_BestClientShopState.m_pInstallTask || !gs_BestClientShopState.m_pInstallTask->Done())
		{
			return;
		}

		if(gs_BestClientShopState.m_pInstallTask->State() != EHttpState::DONE)
		{
			BestClientShopRetryInstall(pMenus);
			return;
		}

		if(gs_BestClientShopState.m_pInstallTask->StatusCode() >= 400)
		{
			BestClientShopRetryInstall(pMenus);
			return;
		}

		unsigned char *pResult = nullptr;
		size_t ResultLength = 0;
		gs_BestClientShopState.m_pInstallTask->Result(&pResult, &ResultLength);
		if(pResult == nullptr || ResultLength == 0 || !BestClientShopInstallDownloadedData(pMenus, pResult, ResultLength))
		{
			BestClientShopRetryInstall(pMenus);
			return;
		}

		char aMessage[256];
		if(g_Config.m_BcShopAutoSet)
		{
			BestClientShopApplyAsset(pMenus, gs_BestClientShopState.m_InstallTab, gs_BestClientShopState.m_aInstallAssetName, true);
			str_format(aMessage, sizeof(aMessage), "%s: %s", TCLocalize("Installed and applied"), gs_BestClientShopState.m_aInstallAssetName);
		}
		else
		{
			pMenus->RefreshCustomAssetsTab(gs_aBestClientShopTypeInfos[gs_BestClientShopState.m_InstallTab].m_AssetsTab);
			str_format(aMessage, sizeof(aMessage), "%s: %s", TCLocalize("Installed"), gs_BestClientShopState.m_aInstallAssetName);
		}
		BestClientShopSetStatus(aMessage);

		BestClientShopAbortTask(gs_BestClientShopState.m_pInstallTask);
		BestClientShopResetInstallState();
	}

	static void BestClientShopRenderPreview(CMenus *pMenus, const CUIRect &MainView)
	{
		SBestClientShopItem *pItem = BestClientShopFindItem(gs_BestClientShopState.m_aOpenPreviewItemId);
		if(pItem == nullptr || !pItem->m_PreviewTexture.IsValid() || pItem->m_PreviewTexture.IsNullTexture())
		{
			BestClientShopClosePreview();
			return;
		}

		CUIRect Overlay = MainView;
		Overlay.Draw(ColorRGBA(0.0f, 0.0f, 0.0f, 0.82f), IGraphics::CORNER_ALL, 0.0f);

		CUIRect Panel = Overlay;
		Panel.VMargin(maximum(32.0f, (Overlay.w - 860.0f) / 2.0f), &Panel);
		Panel.HMargin(maximum(24.0f, (Overlay.h - 600.0f) / 2.0f), &Panel);
		Panel.Draw(ColorRGBA(0.06f, 0.07f, 0.08f, 0.96f), IGraphics::CORNER_ALL, BESTCLIENT_SHOP_SECTION_ROUNDING + 2.0f);

		CUIRect Content;
		Panel.Margin(BESTCLIENT_SHOP_MARGIN + 4.0f, &Content);

		CUIRect Header, PreviewArea;
		Content.HSplitTop(28.0f, &Header, &Content);
		Content.HSplitTop(BESTCLIENT_SHOP_MARGIN_SMALL, nullptr, &Content);
		PreviewArea = Content;

		CUIRect Title, CloseButton;
		Header.VSplitRight(100.0f, &Title, &CloseButton);
		pMenus->MenuUi()->DoLabel(&Title, pItem->m_aName, BESTCLIENT_SHOP_HEADLINE_FONT_SIZE, TEXTALIGN_ML);
		if(pMenus->DoButton_Menu(&gs_BestClientShopState.m_PreviewCloseButton, TCLocalize("Close"), 0, &CloseButton, BUTTONFLAG_LEFT, nullptr, IGraphics::CORNER_ALL, 6.0f, 0.0f, ColorRGBA(0.0f, 0.0f, 0.0f, 0.30f)))
		{
			BestClientShopClosePreview();
			return;
		}

		PreviewArea.Draw(ColorRGBA(1.0f, 1.0f, 1.0f, 0.04f), IGraphics::CORNER_ALL, BESTCLIENT_SHOP_SECTION_ROUNDING);
		PreviewArea.Margin(BESTCLIENT_SHOP_MARGIN, &PreviewArea);
		PreviewArea.Draw(ColorRGBA(0.12f, 0.12f, 0.12f, 1.0f), IGraphics::CORNER_ALL, BESTCLIENT_SHOP_SECTION_ROUNDING - 2.0f);

		CUIRect TextureRect;
		PreviewArea.Margin(BESTCLIENT_SHOP_MARGIN, &TextureRect);
		BestClientShopDrawFittedTexture(pMenus->MenuGraphics(), pItem->m_PreviewTexture, TextureRect, pItem->m_PreviewWidth, pItem->m_PreviewHeight);
	}

} // namespace

void CMenus::SetBestClientShopVisible(bool Visible)
{
	BestClientShopSetVisible(Visible);
}

void CMenus::RenderSettingsBestClientShop(CUIRect MainView)
{
	SetBestClientShopVisible(true);

	const bool IsRussian = str_find_nocase(g_Config.m_ClLanguagefile, "russian") != nullptr;
	const char *pLine1 = IsRussian ? "\xd0\xa8\xd0\xbe\xd0\xbf \xd0\xb2\xd1\x80\xd0\xb5\xd0\xbc\xd0\xb5\xd0\xbd\xd0\xbd\xd0\xbe \xd0\xbd\xd0\xb5\xd0\xb4\xd0\xbe\xd1\x81\xd1\x82\xd1\x83\xd0\xbf\xd0\xb5\xd0\xbd" : "Shop is temporarily unavailable";
	const char *pLine2 = IsRussian ? "\xd0\xa8\xd0\xbe\xd0\xbf \xd1\x80\xd0\xb0\xd0\xb1\xd0\xbe\xd1\x82\xd0\xb0\xd0\xbb \xd0\xbd\xd0\xb0 \xd1\x81\xd0\xb0\xd0\xb9\xd1\x82\xd0\xb5 catdata.dev, \xd0\xba\xd0\xbe\xd1\x82\xd0\xbe\xd1\x80\xd1\x8b\xd0\xb9 \xd1\x82\xd0\xb5\xd0\xbf\xd0\xb5\xd1\x80\xd1\x8c \xd0\xb1\xd0\xbe\xd0\xbb\xd1\x8c\xd1\x88\xd0\xb5 \xd0\xbd\xd0\xb5 \xd0\xb4\xd0\xbe\xd1\x81\xd1\x82\xd1\x83\xd0\xbf\xd0\xb5\xd0\xbd" : "Shop was powered by catdata.dev, which is no longer available";
	const char *pLine3 = IsRussian ? "\xd0\x9e\xd0\xb6\xd0\xb8\xd0\xb4\xd0\xb0\xd0\xb9\xd1\x82\xd0\xb5 \xd0\xbe\xd0\xb1\xd0\xbd\xd0\xbe\xd0\xb2\xd0\xbb\xd0\xb5\xd0\xbd\xd0\xb8\xd1\x8f" : "Please await an update";

	CUIRect Block;
	MainView.HSplitTop(MainView.h * 0.35f, nullptr, &Block);

	CUIRect Line1, Line2, Line3;
	Block.HSplitTop(30.0f, &Line1, &Block);
	Block.HSplitTop(8.0f, nullptr, &Block);
	Block.HSplitTop(24.0f, &Line2, &Block);
	Block.HSplitTop(5.0f, nullptr, &Block);
	Block.HSplitTop(24.0f, &Line3, &Block);

	Ui()->DoLabel(&Line1, pLine1, 20.0f, TEXTALIGN_MC);
	Ui()->DoLabel(&Line2, pLine2, 15.0f, TEXTALIGN_MC);
	Ui()->DoLabel(&Line3, pLine3, 15.0f, TEXTALIGN_MC);
}
