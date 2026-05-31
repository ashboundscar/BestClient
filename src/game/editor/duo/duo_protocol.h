#ifndef GAME_EDITOR_DUO_PROTOCOL_H
#define GAME_EDITOR_DUO_PROTOCOL_H

#include <cstdint>
#include <cstring>
#include <vector>

namespace DuoProtocol {

constexpr uint32_t DUO_MAGIC = 0x444D5031; // "DMP1"
constexpr uint8_t DUO_VERSION = 1;
constexpr int DUO_DEFAULT_PORT = 5555;
constexpr int DUO_HEADER_SIZE = 6;
constexpr int ROOM_CODE_LEN = 6;
constexpr int SESSION_TOKEN_SIZE = 16;
constexpr int HMAC_SIZE = 32;
constexpr int MAX_PACKET_SIZE = 65535;

enum EPacketType : uint8_t
{
	PACKET_HELLO = 1,
	PACKET_HELLO_ACK = 2,
	PACKET_ROOM_STATE = 3,
	PACKET_START = 4,
	PACKET_HEARTBEAT = 5,
	PACKET_CURSOR = 6,
	PACKET_CURSOR_RELAY = 7,
	PACKET_TILE_EDIT = 8,
	PACKET_TILE_RELAY = 9,
	PACKET_GOODBYE = 10,
	PACKET_ERROR = 11,
	PACKET_SYNC_CHECK = 12,         // sender → relay → receiver: GroupIdx(4)+LayerIdx(4)+CRC32(4)
	PACKET_SYNC_REQUEST = 13,       // receiver → relay → sender: GroupIdx(4)+LayerIdx(4)
	PACKET_SYNC_DATA = 14,          // sender → relay → receiver: GroupIdx(4)+LayerIdx(4)+W(4)+H(4)+Row(4)+tiles(W*4)
	PACKET_STRUCT_ADD_GROUP = 15,   // no payload
	PACKET_STRUCT_DEL_GROUP = 16,   // GroupIdx(4)
	PACKET_STRUCT_ADD_LAYER = 17,   // GroupIdx(4)+LayerIdx(4)+Type(1)+Name(2+N)
	PACKET_STRUCT_DEL_LAYER = 18,   // GroupIdx(4)+LayerIdx(4)
	PACKET_STRUCT_SET_IMAGE = 19,   // GroupIdx(4)+LayerIdx(4)+ImageIdx(4)
	PACKET_STRUCT_RENAME_GROUP = 20,// GroupIdx(4)+Name(2+N)
	PACKET_STRUCT_RENAME_LAYER = 21,// GroupIdx(4)+LayerIdx(4)+Name(2+N)
	PACKET_STRUCT_LAYER_PROP = 22,  // GroupIdx(4)+LayerIdx(4)+PropId(1)+Value(4)
	PACKET_STRUCT_ADD_IMAGE = 23,   // External(1)+Name(2+N)+RawData(4+N)
	PACKET_STRUCT_DEL_IMAGE = 24,   // ImageIdx(4)
	PACKET_STRUCT_EMBED_IMAGE = 25, // ImageIdx(4)+RawData(4+N)  (external→embedded)
	PACKET_STRUCT_EXTERN_IMAGE = 26,// ImageIdx(4)               (embedded→external)
};

enum EErrorCode : uint8_t
{
	ERROR_ROOM_FULL = 1,
	ERROR_ROOM_NOT_FOUND = 2,
	ERROR_AUTH_FAILED = 3,
	ERROR_RATE_LIMITED = 4,
	ERROR_REPLAY_DETECTED = 5,
};

inline void WriteU8(std::vector<uint8_t> &v, uint8_t val) { v.push_back(val); }

inline void WriteU16(std::vector<uint8_t> &v, uint16_t val)
{
	v.push_back((val >> 8) & 0xFF);
	v.push_back(val & 0xFF);
}

inline void WriteU32(std::vector<uint8_t> &v, uint32_t val)
{
	v.push_back((val >> 24) & 0xFF);
	v.push_back((val >> 16) & 0xFF);
	v.push_back((val >> 8) & 0xFF);
	v.push_back(val & 0xFF);
}

inline void WriteS32(std::vector<uint8_t> &v, int32_t val)
{
	WriteU32(v, static_cast<uint32_t>(val));
}

inline void WriteU64(std::vector<uint8_t> &v, uint64_t val)
{
	WriteU32(v, static_cast<uint32_t>(val >> 32));
	WriteU32(v, static_cast<uint32_t>(val & 0xFFFFFFFF));
}

inline void WriteBytes(std::vector<uint8_t> &v, const uint8_t *pData, int Size)
{
	v.insert(v.end(), pData, pData + Size);
}

inline void WriteString(std::vector<uint8_t> &v, const char *pStr, int Len)
{
	WriteU16(v, static_cast<uint16_t>(Len));
	v.insert(v.end(), reinterpret_cast<const uint8_t *>(pStr), reinterpret_cast<const uint8_t *>(pStr) + Len);
}

inline void WriteHeader(std::vector<uint8_t> &v, EPacketType Type)
{
	WriteU32(v, DUO_MAGIC);
	WriteU8(v, static_cast<uint8_t>(Type));
	WriteU8(v, DUO_VERSION);
}

class CPacketReader
{
	const uint8_t *m_pData;
	int m_Size;
	int m_Offset;

public:
	CPacketReader(const uint8_t *pData, int Size) :
		m_pData(pData), m_Size(Size), m_Offset(0) {}

	bool HasBytes(int n) const { return m_Offset + n <= m_Size; }
	int Remaining() const { return m_Size - m_Offset; }

	uint8_t ReadU8()
	{
		if(!HasBytes(1)) return 0;
		return m_pData[m_Offset++];
	}

	uint16_t ReadU16()
	{
		if(!HasBytes(2)) return 0;
		uint16_t val = (static_cast<uint16_t>(m_pData[m_Offset]) << 8) | m_pData[m_Offset + 1];
		m_Offset += 2;
		return val;
	}

	uint32_t ReadU32()
	{
		if(!HasBytes(4)) return 0;
		uint32_t val = (static_cast<uint32_t>(m_pData[m_Offset]) << 24) |
			(static_cast<uint32_t>(m_pData[m_Offset + 1]) << 16) |
			(static_cast<uint32_t>(m_pData[m_Offset + 2]) << 8) |
			m_pData[m_Offset + 3];
		m_Offset += 4;
		return val;
	}

	int32_t ReadS32() { return static_cast<int32_t>(ReadU32()); }

	uint64_t ReadU64()
	{
		uint64_t hi = ReadU32();
		uint64_t lo = ReadU32();
		return (hi << 32) | lo;
	}

	bool ReadBytes(uint8_t *pOut, int Size)
	{
		if(!HasBytes(Size)) return false;
		memcpy(pOut, m_pData + m_Offset, Size);
		m_Offset += Size;
		return true;
	}

	bool ValidateHeader(EPacketType *pType)
	{
		if(!HasBytes(DUO_HEADER_SIZE)) return false;
		uint32_t Magic = ReadU32();
		if(Magic != DUO_MAGIC) return false;
		*pType = static_cast<EPacketType>(ReadU8());
		uint8_t Version = ReadU8();
		return Version == DUO_VERSION;
	}
};

} // namespace DuoProtocol

#endif // GAME_EDITOR_DUO_PROTOCOL_H
