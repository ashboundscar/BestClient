#ifndef GAME_EDITOR_DUO_SESSION_H
#define GAME_EDITOR_DUO_SESSION_H

#include <game/editor/component.h>
#include <game/editor/duo/duo_protocol.h>
#include <game/client/ui.h>
#include <game/mapitems.h>
#include <base/net.h>
#include <base/system.h>
#include <set>
#include <utility>
#include <vector>

class CDuoSession : public CEditorComponent
{
public:
	void OnInit(CEditor *pEditor) override;
	void OnReset() override;
	void OnUpdate() override;
	void OnRender(CUIRect View) override;

	void NotifyTileEdit(int GroupIdx, int LayerIdx, int TileX, int TileY, uint8_t Index, uint8_t Flags);
	void NotifyStrokeEnd(); // call when mouse button released after drawing
	void NotifyFullSync();  // call after undo/redo — checks all tile layers
	void NotifyAddGroup(int InsertIdx = -1);
	void NotifyDelGroup(int GroupIdx);
	void NotifyAddLayer(int GroupIdx, int LayerIdx, int LayerType, const char *pName, int SubType = 0);
	void NotifyDelLayer(int GroupIdx, int LayerIdx);
	void SyncLayerContents(int GroupIdx, int LayerIdx); // send image + quads/tiles after layer restore
	void NotifySetImage(int GroupIdx, int LayerIdx, int ImageIdx);
	void NotifyRenameGroup(int GroupIdx, const char *pName);
	void NotifyRenameLayer(int GroupIdx, int LayerIdx, const char *pName);
	void NotifyLayerProp(int GroupIdx, int LayerIdx, int PropId, int Value);
	void NotifyAddImage(const char *pName, bool External, const uint8_t *pData, int DataSize);
	void NotifyDelImage(int ImageIdx);
	void NotifyEmbedImage(int ImageIdx, const uint8_t *pData, int DataSize);
	void NotifyExternImage(int ImageIdx);
	void NotifyAddQuad(int GroupIdx, int LayerIdx, int QuadIdx, const CQuad &Quad);
	void NotifyDelQuad(int GroupIdx, int LayerIdx, int QuadIdx);
	void NotifyQuadPoints(int GroupIdx, int LayerIdx, int QuadIdx, const CPoint *pPoints);
	void NotifyQuadColors(int GroupIdx, int LayerIdx, int QuadIdx, const CColor *pColors);
	void NotifyQuadProp(int GroupIdx, int LayerIdx, int QuadIdx, int Prop, int Value);
	void NotifyQuadPointProp(int GroupIdx, int LayerIdx, int QuadIdx, int PointIdx, int Prop, int Value);
	void NotifyLayerFlags(int GroupIdx, int LayerIdx, int Flags);
	void NotifyGroupProp(int GroupIdx, int PropId, int Value);
	void NotifySettingAdd(const char *pCmd);
	void NotifySettingDel(int CmdIdx);
	void NotifySettingEdit(int CmdIdx, const char *pCmd);
	void NotifySettingMove(int CmdIdx, int Direction);
	void NotifyEditorSettings();
	void StartMapTransfer(); // called when STATE_LIVE and we are creator
	bool IsLive() const { return m_State == STATE_LIVE; }

	static CUi::EPopupMenuFunctionResult PopupDuo(void *pContext, CUIRect View, bool Active);
	static CUi::EPopupMenuFunctionResult PopupDuoMain(void *pContext, CUIRect View, bool Active);
	static CUi::EPopupMenuFunctionResult PopupDuoCreate(void *pContext, CUIRect View, bool Active);
	static CUi::EPopupMenuFunctionResult PopupDuoJoin(void *pContext, CUIRect View, bool Active);

	enum EState
	{
		STATE_IDLE = 0,
		STATE_CONNECTING,
		STATE_WAITING,
		STATE_LIVE,
		STATE_ERROR,
	};

	EState m_State = STATE_IDLE;
	NETSOCKET m_Socket = nullptr;
	NETADDR m_ServerAddr = {};
	int64_t m_LastHeartbeatTime = 0;
	int64_t m_LastServerPacketTime = 0;
	int64_t m_LastCursorSendTime = 0;
	char m_aRoomCode[DuoProtocol::ROOM_CODE_LEN + 1] = {};
	bool m_IsCreator = false;

	float m_RemoteCursorX = 0.0f;
	float m_RemoteCursorY = 0.0f;
	bool m_HasRemoteCursor = false;

	int m_ParticipantCount = 0;
	char m_aJoinCodeInput[DuoProtocol::ROOM_CODE_LEN + 1] = {};
	int m_JoinCodeLen = 0;
	char m_aErrorMsg[128] = {};

	// TCP recv buffer for stream reassembly
	std::vector<uint8_t> m_vRecvBuf;
	int m_RecvBufLen = 0;

	// set while applying a remote packet — prevents re-broadcasting back
	bool m_ApplyingRemote = false;
	// set while owner is loading a new map to transfer — prevents OnReset from disconnecting
	bool m_OwnerLoadingMap = false;
	// set when MAP_NEW received — processed next frame
	bool m_PendingMapNew = false;
	// set when map transfer should happen after save completes
	bool m_PendingMapTransfer = false;
	// envelope sync: track undo stack size to detect changes
	int m_LastEnvUndoSize = 0;
	bool m_EnvDirty = false;

	// debug counters
	int m_DbgQuadSent = 0;
	int m_DbgQuadRecv = 0;

	// map transfer — receiver side
	bool m_MapTransferActive = false;
	int m_MapTransferTotal = 0;
	int m_MapTransferReceived = 0;
	char m_aMapTransferName[256] = {};
	std::vector<uint8_t> m_vMapTransferBuf;

	struct STileEditEntry
	{
		int m_GroupIdx;
		int m_LayerIdx;
		int m_TileX;
		int m_TileY;
		uint8_t m_Index;
		uint8_t m_Flags;
	};
	std::vector<STileEditEntry> m_vPendingTileEdits;

	// layers touched during current mouse stroke — flushed on NotifyStrokeEnd
	std::set<std::pair<int, int>> m_DirtyLayers;

	void Connect(const char *pRoomCode, bool Create);
	void Disconnect();
	void OpenSocket();
	void CloseSocket();
	void SendFrame(const std::vector<uint8_t> &vPayload);
	void SendHello();
	void SendHeartbeat();
	void SendCursor(float WorldX, float WorldY);
	void SendTileEdit(int GroupIdx, int LayerIdx, int TileX, int TileY, uint8_t Index, uint8_t Flags);
	void FlushTileEdits();
	void SendSyncCheck(int GroupIdx, int LayerIdx);
	void SendSyncRequest(int GroupIdx, int LayerIdx);
	void SendSyncData(int GroupIdx, int LayerIdx);
	void SendStructAddGroup(int InsertIdx);
	void SendStructDelGroup(int GroupIdx);
	void SendStructAddLayer(int GroupIdx, int LayerIdx, int LayerType, const char *pName, int SubType = 0);
	void SendStructDelLayer(int GroupIdx, int LayerIdx);
	void SendStructSetImage(int GroupIdx, int LayerIdx, int ImageIdx);
	void SendStructRenameGroup(int GroupIdx, const char *pName);
	void SendStructRenameLayer(int GroupIdx, int LayerIdx, const char *pName);
	void SendStructLayerProp(int GroupIdx, int LayerIdx, int PropId, int Value);
	void SendStructAddImage(const char *pName, bool External, const uint8_t *pData, int DataSize);
	void SendStructDelImage(int ImageIdx);
	void SendStructEmbedImage(int ImageIdx, const uint8_t *pData, int DataSize);
	void SendStructExternImage(int ImageIdx);
	void SendQuadAdd(int GroupIdx, int LayerIdx, int QuadIdx, const CQuad &Quad);
	void SendQuadDel(int GroupIdx, int LayerIdx, int QuadIdx);
	void SendQuadPoints(int GroupIdx, int LayerIdx, int QuadIdx, const CPoint *pPoints);
	void SendQuadColors(int GroupIdx, int LayerIdx, int QuadIdx, const CColor *pColors);
	void SendQuadProp(int GroupIdx, int LayerIdx, int QuadIdx, int Prop, int Value);
	void SendQuadPointProp(int GroupIdx, int LayerIdx, int QuadIdx, int PointIdx, int Prop, int Value);
	void SendLayerFlags(int GroupIdx, int LayerIdx, int Flags);
	void SendGroupProp(int GroupIdx, int PropId, int Value);
	void SendSettingAdd(const char *pCmd);
	void SendSettingDel(int CmdIdx);
	void SendSettingEdit(int CmdIdx, const char *pCmd);
	void SendSettingMove(int CmdIdx, int Direction);
	void SendGoodbye();
	void SendMapStart(const char *pName, int TotalSize);
	void SendMapChunk(int Offset, const uint8_t *pData, int DataLen);
	void SendMapEnd();
	void SendMapNew();
	void SendEditorSettings();
	void ProcessNetwork();
	void HandleMessage(const uint8_t *pData, int Size);
	void AppendAuth(std::vector<uint8_t> &vPacket) const;

private:
	static uint32_t CalcLayerCRC(const uint8_t *pTiles, int Count);
};

#endif // GAME_EDITOR_DUO_SESSION_H
