#if defined(_WIN32)

#include <reshade.hpp>

#include <algorithm>
#include <chrono>
#include <cctype>
#include <cstdlib>
#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <optional>
#include <sstream>
#include <string>
#include <system_error>
#include <unordered_map>
#include <unordered_set>
#include <vector>

extern "C" __declspec(dllexport) const char *NAME = "BestClient ReShade Live Bridge";
extern "C" __declspec(dllexport) const char *DESCRIPTION = "Applies BestClient ReShade UI changes directly to the active ReShade runtime.";
extern "C" __declspec(dllexport) const char *AUTHOR = "BestClient";

namespace
{
constexpr const char *gs_pBridgeFilename = "BestClientReShadeBridge.ini";

enum class EUniformValueType
{
	BOOL = 0,
	INT,
	UINT,
	FLOAT,
	FLOAT_VECTOR,
};

struct SUniformValue
{
	EUniformValueType m_Type = EUniformValueType::FLOAT;
	bool m_BoolValue = false;
	int32_t m_IntValue = 0;
	uint32_t m_UintValue = 0;
	float m_FloatValue = 0.0f;
	std::vector<float> m_vFloatValues;
};

struct SBridgeState
{
	uint64_t m_Revision = 0;
	std::vector<std::string> m_vTechniqueSorting;
	std::unordered_set<std::string> m_EnabledTokens;
	std::unordered_map<std::string, std::unordered_map<std::string, SUniformValue>> m_SectionValues;
};

struct STechniqueTokenMeta
{
	std::string m_Token;
	std::string m_TechniqueName;
	std::string m_EffectName;
	std::string m_NormalizedEffect;
	std::string m_EffectStem;
};

struct SRuntimeTechniqueInfo
{
	reshade::api::effect_technique m_Handle = {0};
	std::string m_Token;
	std::string m_TechniqueName;
	std::string m_EffectName;
	std::string m_NormalizedEffect;
	std::string m_EffectStem;
};

struct SRuntimeUniformInfo
{
	reshade::api::effect_uniform_variable m_Handle = {0};
	std::string m_Name;
	std::string m_NormalizedName;
	std::string m_EffectName;
	std::string m_NormalizedEffect;
	std::string m_EffectStem;
};

struct SRuntimeState
{
	std::filesystem::path m_CommandPath;
	std::filesystem::file_time_type m_LastWriteTime = {};
	bool m_HasWriteTime = false;
	size_t m_LastHash = 0;
	bool m_HasHash = false;
	bool m_EffectsReady = false;
	bool m_HasAppliedCommand = false;
	std::chrono::steady_clock::time_point m_LastFingerprintCheck = {};
};

std::unordered_map<reshade::api::effect_runtime *, SRuntimeState> gs_RuntimeStates;

void LogInfo(const char *pMessage)
{
	reshade::log::message(reshade::log::level::info, pMessage);
}

void LogWarning(const char *pMessage)
{
	reshade::log::message(reshade::log::level::warning, pMessage);
}

std::string Trim(std::string Text)
{
	const auto IsWhitespace = [](unsigned char c) { return c == ' ' || c == '\t' || c == '\r' || c == '\n'; };
	while(!Text.empty() && IsWhitespace((unsigned char)Text.front()))
		Text.erase(Text.begin());
	while(!Text.empty() && IsWhitespace((unsigned char)Text.back()))
		Text.pop_back();
	return Text;
}

std::string ToLower(std::string Text)
{
	for(char &c : Text)
		c = (char)std::tolower((unsigned char)c);
	return Text;
}

std::vector<std::string> SplitCommaSeparated(const std::string &Text)
{
	std::vector<std::string> vTokens;
	size_t TokenStart = 0;
	while(TokenStart <= Text.size())
	{
		const size_t TokenEnd = Text.find(',', TokenStart);
		const std::string Token = Trim(Text.substr(TokenStart, TokenEnd == std::string::npos ? std::string::npos : TokenEnd - TokenStart));
		if(!Token.empty())
			vTokens.push_back(Token);
		if(TokenEnd == std::string::npos)
			break;
		TokenStart = TokenEnd + 1;
	}
	return vTokens;
}

std::string NormalizeEffectIdentifier(const std::string &EffectName)
{
	std::string Normalized = EffectName;
	std::replace(Normalized.begin(), Normalized.end(), '\\', '/');
	const size_t SlashPos = Normalized.find_last_of('/');
	if(SlashPos != std::string::npos)
		Normalized = Normalized.substr(SlashPos + 1);
	return ToLower(Normalized);
}

std::string EffectIdentifierStem(const std::string &EffectName)
{
	const std::filesystem::path Path = std::filesystem::u8path(EffectName);
	return ToLower(Path.stem().string());
}

std::string NormalizeUniformIdentifier(const std::string &UniformName)
{
	std::string Normalized = UniformName;
	const size_t ScopePos = Normalized.rfind("::");
	if(ScopePos != std::string::npos)
		Normalized = Normalized.substr(ScopePos + 2);

	const size_t BracketPos = Normalized.find('[');
	if(BracketPos != std::string::npos)
		Normalized = Normalized.substr(0, BracketPos);

	return ToLower(Normalized);
}

bool QueryWriteTime(const std::filesystem::path &Path, std::filesystem::file_time_type &WriteTime)
{
	std::error_code Error;
	WriteTime = std::filesystem::last_write_time(Path, Error);
	return !Error;
}

bool QueryCommandFingerprint(const std::filesystem::path &Path, std::filesystem::file_time_type &WriteTime, size_t &Hash)
{
	if(!QueryWriteTime(Path, WriteTime))
		return false;

	std::ifstream File(Path, std::ios::binary);
	if(!File.is_open())
		return false;

	std::string Contents((std::istreambuf_iterator<char>(File)), std::istreambuf_iterator<char>());
	Hash = std::hash<std::string>{}(Contents);
	return true;
}

std::optional<std::string> ReadTextFile(const std::filesystem::path &Path)
{
	std::ifstream File(Path, std::ios::in | std::ios::binary);
	if(!File.is_open())
		return std::nullopt;

	std::ostringstream Buffer;
	Buffer << File.rdbuf();
	return Buffer.str();
}

bool TryParseFloat(const std::string &Text, float &Value)
{
	char *pEnd = nullptr;
	Value = std::strtof(Text.c_str(), &pEnd);
	return pEnd != Text.c_str() && pEnd != nullptr && *pEnd == '\0';
}

bool TryParseFloatVector(const std::string &Text, std::vector<float> &vValues)
{
	vValues.clear();
	std::string Normalized = Trim(Text);
	const size_t OpenParenPos = Normalized.find('(');
	const size_t CloseParenPos = Normalized.rfind(')');
	if(OpenParenPos != std::string::npos && CloseParenPos != std::string::npos && OpenParenPos < CloseParenPos)
		Normalized = Normalized.substr(OpenParenPos + 1, CloseParenPos - OpenParenPos - 1);

	for(const std::string &Token : SplitCommaSeparated(Normalized))
	{
		float Value = 0.0f;
		if(!TryParseFloat(Token, Value))
			return false;
		vValues.push_back(Value);
	}
	return !vValues.empty();
}

bool TryParseInt(const std::string &Text, int32_t &Value)
{
	char *pEnd = nullptr;
	const long ParsedValue = std::strtol(Text.c_str(), &pEnd, 10);
	if(pEnd == Text.c_str() || pEnd == nullptr || *pEnd != '\0')
		return false;
	Value = (int32_t)ParsedValue;
	return true;
}

bool TryParseUint(const std::string &Text, uint32_t &Value)
{
	char *pEnd = nullptr;
	const unsigned long ParsedValue = std::strtoul(Text.c_str(), &pEnd, 10);
	if(pEnd == Text.c_str() || pEnd == nullptr || *pEnd != '\0')
		return false;
	Value = (uint32_t)ParsedValue;
	return true;
}

bool TryParseUint64(const std::string &Text, uint64_t &Value)
{
	char *pEnd = nullptr;
	const unsigned long long ParsedValue = std::strtoull(Text.c_str(), &pEnd, 10);
	if(pEnd == Text.c_str() || pEnd == nullptr || *pEnd != '\0')
		return false;
	Value = (uint64_t)ParsedValue;
	return true;
}

bool TryParseBool(const std::string &Text, bool &Value)
{
	const std::string Lower = ToLower(Text);
	if(Lower == "true" || Lower == "1")
	{
		Value = true;
		return true;
	}
	if(Lower == "false" || Lower == "0")
	{
		Value = false;
		return true;
	}
	return false;
}

std::string BuildTechniqueToken(const char *pTechniqueName, const char *pEffectName)
{
	if(pTechniqueName == nullptr || pTechniqueName[0] == '\0')
		return "";
	if(pEffectName == nullptr || pEffectName[0] == '\0')
		return pTechniqueName;
	return std::string(pTechniqueName) + "@" + pEffectName;
}

STechniqueTokenMeta BuildTechniqueTokenMeta(const std::string &Token)
{
	STechniqueTokenMeta Meta;
	Meta.m_Token = Token;
	const size_t SeparatorPos = Token.find('@');
	if(SeparatorPos == std::string::npos)
	{
		Meta.m_TechniqueName = Token;
	}
	else
	{
		Meta.m_TechniqueName = Token.substr(0, SeparatorPos);
		Meta.m_EffectName = Token.substr(SeparatorPos + 1);
	}
	Meta.m_NormalizedEffect = NormalizeEffectIdentifier(Meta.m_EffectName);
	Meta.m_EffectStem = EffectIdentifierStem(Meta.m_NormalizedEffect);
	return Meta;
}

bool ParseBridgeState(const std::filesystem::path &BridgePath, SBridgeState &State)
{
	const std::optional<std::string> BridgeText = ReadTextFile(BridgePath);
	if(!BridgeText.has_value())
		return false;

	State = SBridgeState{};

	std::istringstream Stream(*BridgeText);
	std::string Line;
	std::string CurrentSection;

	while(std::getline(Stream, Line))
	{
		const std::string TrimmedLine = Trim(Line);
		if(TrimmedLine.empty() || TrimmedLine[0] == ';' || TrimmedLine[0] == '#')
			continue;

		if(TrimmedLine.front() == '[' && TrimmedLine.back() == ']')
		{
			CurrentSection = Trim(TrimmedLine.substr(1, TrimmedLine.size() - 2));
			continue;
		}

		const size_t EqualsPos = TrimmedLine.find('=');
		if(EqualsPos == std::string::npos)
			continue;

		const std::string Key = Trim(TrimmedLine.substr(0, EqualsPos));
		const std::string Value = Trim(TrimmedLine.substr(EqualsPos + 1));

		if(CurrentSection.empty())
		{
			if(Key == "Revision")
			{
				TryParseUint64(Value, State.m_Revision);
			}
			else if(Key == "Techniques")
			{
				for(const std::string &Token : SplitCommaSeparated(Value))
					State.m_EnabledTokens.insert(Token);
			}
			else if(Key == "TechniqueSorting")
			{
				State.m_vTechniqueSorting = SplitCommaSeparated(Value);
			}
			continue;
		}

		SUniformValue ParsedValue;
		bool BoolValue = false;
		int32_t IntValue = 0;
		uint32_t UintValue = 0;
		float FloatValue = 0.0f;
		std::vector<float> vFloatValues;
		if(TryParseBool(Value, BoolValue))
		{
			ParsedValue.m_Type = EUniformValueType::BOOL;
			ParsedValue.m_BoolValue = BoolValue;
		}
		else if(TryParseInt(Value, IntValue))
		{
			ParsedValue.m_Type = EUniformValueType::INT;
			ParsedValue.m_IntValue = IntValue;
		}
		else if(TryParseUint(Value, UintValue))
		{
			ParsedValue.m_Type = EUniformValueType::UINT;
			ParsedValue.m_UintValue = UintValue;
		}
		else if(TryParseFloat(Value, FloatValue))
		{
			ParsedValue.m_Type = EUniformValueType::FLOAT;
			ParsedValue.m_FloatValue = FloatValue;
		}
		else if(TryParseFloatVector(Value, vFloatValues))
		{
			ParsedValue.m_Type = EUniformValueType::FLOAT_VECTOR;
			ParsedValue.m_vFloatValues = std::move(vFloatValues);
		}
		else
		{
			continue;
		}

		State.m_SectionValues[CurrentSection][Key] = ParsedValue;
	}

	return true;
}

bool ApplyUniformValueToVariable(reshade::api::effect_runtime *pRuntime, reshade::api::effect_uniform_variable Variable, const SUniformValue &UniformValue)
{
	reshade::api::format BaseType = reshade::api::format::unknown;
	uint32_t Rows = 0;
	uint32_t Columns = 0;
	uint32_t ArrayLength = 0;
	pRuntime->get_uniform_variable_type(Variable, &BaseType, &Rows, &Columns, &ArrayLength);
	if(ArrayLength == 0)
		ArrayLength = 1;
	if(Rows == 0)
		Rows = 1;
	if(Columns == 0)
		Columns = 1;
	if(ArrayLength != 1)
		return false;

	const uint32_t ComponentCount = Rows * Columns;
	if(ComponentCount == 0)
		return false;

	switch(BaseType)
	{
	case reshade::api::format::r32_typeless:
		if(UniformValue.m_Type == EUniformValueType::BOOL && ComponentCount == 1)
		{
			pRuntime->set_uniform_value_bool(Variable, &UniformValue.m_BoolValue, 1);
			return true;
		}
		break;
	case reshade::api::format::r32_sint:
		if(UniformValue.m_Type == EUniformValueType::INT && ComponentCount == 1)
		{
			pRuntime->set_uniform_value_int(Variable, &UniformValue.m_IntValue, 1);
			return true;
		}
		if(UniformValue.m_Type == EUniformValueType::UINT && ComponentCount == 1)
		{
			const int32_t SignedValue = (int32_t)UniformValue.m_UintValue;
			pRuntime->set_uniform_value_int(Variable, &SignedValue, 1);
			return true;
		}
		if(UniformValue.m_Type == EUniformValueType::BOOL && ComponentCount == 1)
		{
			const int32_t SignedValue = UniformValue.m_BoolValue ? 1 : 0;
			pRuntime->set_uniform_value_int(Variable, &SignedValue, 1);
			return true;
		}
		break;
	case reshade::api::format::r32_uint:
		if(UniformValue.m_Type == EUniformValueType::UINT && ComponentCount == 1)
		{
			pRuntime->set_uniform_value_uint(Variable, &UniformValue.m_UintValue, 1);
			return true;
		}
		if(UniformValue.m_Type == EUniformValueType::INT && UniformValue.m_IntValue >= 0 && ComponentCount == 1)
		{
			const uint32_t UnsignedValue = (uint32_t)UniformValue.m_IntValue;
			pRuntime->set_uniform_value_uint(Variable, &UnsignedValue, 1);
			return true;
		}
		if(UniformValue.m_Type == EUniformValueType::BOOL && ComponentCount == 1)
		{
			const uint32_t UnsignedValue = UniformValue.m_BoolValue ? 1u : 0u;
			pRuntime->set_uniform_value_uint(Variable, &UnsignedValue, 1);
			return true;
		}
		break;
	case reshade::api::format::r32_float:
		if(UniformValue.m_Type == EUniformValueType::FLOAT_VECTOR && ComponentCount <= UniformValue.m_vFloatValues.size())
		{
			pRuntime->set_uniform_value_float(Variable, UniformValue.m_vFloatValues.data(), ComponentCount);
			return true;
		}
		if(UniformValue.m_Type == EUniformValueType::FLOAT && ComponentCount == 1)
		{
			pRuntime->set_uniform_value_float(Variable, &UniformValue.m_FloatValue, 1);
			return true;
		}
		if(UniformValue.m_Type == EUniformValueType::INT && ComponentCount == 1)
		{
			const float FloatValue = (float)UniformValue.m_IntValue;
			pRuntime->set_uniform_value_float(Variable, &FloatValue, 1);
			return true;
		}
		if(UniformValue.m_Type == EUniformValueType::UINT && ComponentCount == 1)
		{
			const float FloatValue = (float)UniformValue.m_UintValue;
			pRuntime->set_uniform_value_float(Variable, &FloatValue, 1);
			return true;
		}
		if(UniformValue.m_Type == EUniformValueType::BOOL && ComponentCount == 1)
		{
			const float FloatValue = UniformValue.m_BoolValue ? 1.0f : 0.0f;
			pRuntime->set_uniform_value_float(Variable, &FloatValue, 1);
			return true;
		}
		break;
	default:
		break;
	}

	return false;
}

std::vector<SRuntimeTechniqueInfo> BuildRuntimeTechniqueInfos(reshade::api::effect_runtime *pRuntime)
{
	std::vector<SRuntimeTechniqueInfo> vTechniques;
	pRuntime->enumerate_techniques(nullptr, [&](reshade::api::effect_runtime *pCurrentRuntime, reshade::api::effect_technique Technique) {
		char aTechniqueName[512] = {0};
		char aEffectName[512] = {0};
		pCurrentRuntime->get_technique_name(Technique, aTechniqueName);
		pCurrentRuntime->get_technique_effect_name(Technique, aEffectName);

		SRuntimeTechniqueInfo Info;
		Info.m_Handle = Technique;
		Info.m_TechniqueName = aTechniqueName;
		Info.m_EffectName = aEffectName;
		Info.m_Token = BuildTechniqueToken(aTechniqueName, aEffectName);
		Info.m_NormalizedEffect = NormalizeEffectIdentifier(Info.m_EffectName);
		Info.m_EffectStem = EffectIdentifierStem(Info.m_NormalizedEffect);
		vTechniques.push_back(std::move(Info));
	});
	return vTechniques;
}

std::vector<SRuntimeUniformInfo> BuildRuntimeUniformInfos(reshade::api::effect_runtime *pRuntime)
{
	std::vector<SRuntimeUniformInfo> vUniforms;
	pRuntime->enumerate_uniform_variables(nullptr, [&](reshade::api::effect_runtime *pCurrentRuntime, reshade::api::effect_uniform_variable Variable) {
		char aUniformName[512] = {0};
		char aEffectName[512] = {0};
		pCurrentRuntime->get_uniform_variable_name(Variable, aUniformName);
		pCurrentRuntime->get_uniform_variable_effect_name(Variable, aEffectName);

		SRuntimeUniformInfo Info;
		Info.m_Handle = Variable;
		Info.m_Name = aUniformName;
		Info.m_NormalizedName = NormalizeUniformIdentifier(Info.m_Name);
		Info.m_EffectName = aEffectName;
		Info.m_NormalizedEffect = NormalizeEffectIdentifier(Info.m_EffectName);
		Info.m_EffectStem = EffectIdentifierStem(Info.m_NormalizedEffect);
		vUniforms.push_back(std::move(Info));
	});
	return vUniforms;
}

bool RuntimeTechniqueMatchesToken(const SRuntimeTechniqueInfo &RuntimeTechnique, const STechniqueTokenMeta &TokenMeta)
{
	if(RuntimeTechnique.m_TechniqueName != TokenMeta.m_TechniqueName)
		return false;

	if(TokenMeta.m_NormalizedEffect.empty())
		return true;

	return RuntimeTechnique.m_NormalizedEffect == TokenMeta.m_NormalizedEffect ||
		(!TokenMeta.m_EffectStem.empty() && RuntimeTechnique.m_EffectStem == TokenMeta.m_EffectStem);
}

void AddUniqueString(std::vector<std::string> &vValues, const std::string &Value)
{
	if(std::find(vValues.begin(), vValues.end(), Value) == vValues.end())
		vValues.push_back(Value);
}

void ApplyTechniqueStates(
	reshade::api::effect_runtime *pRuntime,
	const SBridgeState &BridgeState,
	const std::vector<SRuntimeTechniqueInfo> &vRuntimeTechniques,
	std::unordered_map<std::string, std::vector<std::string>> &RuntimeEffectsBySection,
	int &NumAppliedTechniques)
{
	RuntimeEffectsBySection.clear();
	NumAppliedTechniques = 0;

	std::vector<STechniqueTokenMeta> vTokens;
	vTokens.reserve(BridgeState.m_vTechniqueSorting.size());
	for(const std::string &Token : BridgeState.m_vTechniqueSorting)
		vTokens.push_back(BuildTechniqueTokenMeta(Token));

	std::vector<reshade::api::effect_technique> vOrderedTechniques;
	std::unordered_set<uint64_t> OrderedTechniqueHandles;
	std::unordered_set<uint64_t> MatchedTechniqueHandles;
	vOrderedTechniques.reserve(vRuntimeTechniques.size());

	for(const STechniqueTokenMeta &TokenMeta : vTokens)
	{
		for(const SRuntimeTechniqueInfo &RuntimeTechnique : vRuntimeTechniques)
		{
			if(!RuntimeTechniqueMatchesToken(RuntimeTechnique, TokenMeta))
				continue;

			AddUniqueString(RuntimeEffectsBySection[TokenMeta.m_EffectName], RuntimeTechnique.m_EffectName);
			const bool Enabled = BridgeState.m_EnabledTokens.find(TokenMeta.m_Token) != BridgeState.m_EnabledTokens.end();
			pRuntime->set_technique_state(RuntimeTechnique.m_Handle, Enabled);
			MatchedTechniqueHandles.insert(RuntimeTechnique.m_Handle.handle);
			++NumAppliedTechniques;

			if(OrderedTechniqueHandles.insert(RuntimeTechnique.m_Handle.handle).second)
				vOrderedTechniques.push_back(RuntimeTechnique.m_Handle);
		}
	}

	for(const SRuntimeTechniqueInfo &RuntimeTechnique : vRuntimeTechniques)
	{
		if(MatchedTechniqueHandles.find(RuntimeTechnique.m_Handle.handle) == MatchedTechniqueHandles.end())
			pRuntime->set_technique_state(RuntimeTechnique.m_Handle, false);

		if(OrderedTechniqueHandles.insert(RuntimeTechnique.m_Handle.handle).second)
			vOrderedTechniques.push_back(RuntimeTechnique.m_Handle);
	}

	if(!vOrderedTechniques.empty())
		pRuntime->reorder_techniques(vOrderedTechniques.size(), vOrderedTechniques.data());
}

std::vector<std::string> BuildFallbackRuntimeEffectsForSection(const std::vector<SRuntimeTechniqueInfo> &vRuntimeTechniques, const std::vector<SRuntimeUniformInfo> &vRuntimeUniforms, const std::string &SectionName)
{
	std::vector<std::string> vEffectNames;
	const std::string SectionNormalized = NormalizeEffectIdentifier(SectionName);
	const std::string SectionStem = EffectIdentifierStem(SectionNormalized);

	for(const SRuntimeTechniqueInfo &RuntimeTechnique : vRuntimeTechniques)
	{
		if(RuntimeTechnique.m_NormalizedEffect != SectionNormalized &&
			(SectionStem.empty() || RuntimeTechnique.m_EffectStem != SectionStem))
		{
			continue;
		}

		AddUniqueString(vEffectNames, RuntimeTechnique.m_EffectName);
	}

	for(const SRuntimeUniformInfo &RuntimeUniform : vRuntimeUniforms)
	{
		if(RuntimeUniform.m_NormalizedEffect != SectionNormalized &&
			(SectionStem.empty() || RuntimeUniform.m_EffectStem != SectionStem))
		{
			continue;
		}

		AddUniqueString(vEffectNames, RuntimeUniform.m_EffectName);
	}

	return vEffectNames;
}

int ApplyUniformStates(
	reshade::api::effect_runtime *pRuntime,
	const SBridgeState &BridgeState,
	const std::vector<SRuntimeTechniqueInfo> &vRuntimeTechniques,
	const std::vector<SRuntimeUniformInfo> &vRuntimeUniforms,
	const std::unordered_map<std::string, std::vector<std::string>> &RuntimeEffectsBySection)
{
	int NumAppliedUniforms = 0;
	bool LoggedApplyFailure = false;

	for(const auto &[SectionName, SectionValues] : BridgeState.m_SectionValues)
	{
		if(SectionValues.empty())
			continue;

		std::vector<std::string> CandidateEffectNames;
		const auto RuntimeEffectsIt = RuntimeEffectsBySection.find(SectionName);
		if(RuntimeEffectsIt != RuntimeEffectsBySection.end())
			CandidateEffectNames = RuntimeEffectsIt->second;
		if(CandidateEffectNames.empty())
			CandidateEffectNames = BuildFallbackRuntimeEffectsForSection(vRuntimeTechniques, vRuntimeUniforms, SectionName);
		if(CandidateEffectNames.empty())
			continue;

		for(const auto &[UniformName, UniformValue] : SectionValues)
		{
			bool Applied = false;
			const std::string NormalizedUniformName = NormalizeUniformIdentifier(UniformName);
			for(const std::string &EffectName : CandidateEffectNames)
			{
				for(const SRuntimeUniformInfo &RuntimeUniform : vRuntimeUniforms)
				{
					if(RuntimeUniform.m_EffectName != EffectName || RuntimeUniform.m_NormalizedName != NormalizedUniformName)
						continue;

					if(ApplyUniformValueToVariable(pRuntime, RuntimeUniform.m_Handle, UniformValue))
					{
						++NumAppliedUniforms;
						Applied = true;
						break;
					}
					if(!LoggedApplyFailure)
					{
						reshade::api::format BaseType = reshade::api::format::unknown;
						uint32_t Rows = 0;
						uint32_t Columns = 0;
						uint32_t ArrayLength = 0;
						pRuntime->get_uniform_variable_type(RuntimeUniform.m_Handle, &BaseType, &Rows, &Columns, &ArrayLength);
						char aMessage[1024];
						std::snprintf(
							aMessage, sizeof(aMessage),
							"[BestClient/ReShadeBridge] Apply failed: effect=%s uniform=%s base_type=%u rows=%u columns=%u array_length=%u value_type=%u",
							RuntimeUniform.m_EffectName.c_str(),
							RuntimeUniform.m_Name.c_str(),
							static_cast<unsigned>(BaseType),
							Rows,
							Columns,
							ArrayLength,
							static_cast<unsigned>(UniformValue.m_Type));
						LogWarning(aMessage);
						LoggedApplyFailure = true;
					}
				}

				if(Applied)
					break;
			}
		}
	}

	return NumAppliedUniforms;
}

bool ApplyBridgeState(reshade::api::effect_runtime *pRuntime, const std::filesystem::path &BridgePath, SRuntimeState &RuntimeState)
{
	SBridgeState BridgeState;
	if(!ParseBridgeState(BridgePath, BridgeState))
		return false;

	const std::vector<SRuntimeTechniqueInfo> vRuntimeTechniques = BuildRuntimeTechniqueInfos(pRuntime);
	const std::vector<SRuntimeUniformInfo> vRuntimeUniforms = BuildRuntimeUniformInfos(pRuntime);
	if(vRuntimeTechniques.empty())
	{
		RuntimeState.m_HasAppliedCommand = false;
		return false;
	}

	std::unordered_map<std::string, std::vector<std::string>> RuntimeEffectsBySection;
	int NumAppliedTechniques = 0;
	ApplyTechniqueStates(pRuntime, BridgeState, vRuntimeTechniques, RuntimeEffectsBySection, NumAppliedTechniques);
	const int NumAppliedUniforms = ApplyUniformStates(pRuntime, BridgeState, vRuntimeTechniques, vRuntimeUniforms, RuntimeEffectsBySection);

	RuntimeState.m_HasAppliedCommand = true;

	char aMessage[4096];
	std::snprintf(
		aMessage, sizeof(aMessage),
		"[BestClient/ReShadeBridge] Applied live bridge state: revision=%llu techniques=%d sections=%zu uniforms=%d",
		static_cast<unsigned long long>(BridgeState.m_Revision),
		NumAppliedTechniques,
		BridgeState.m_SectionValues.size(),
		NumAppliedUniforms);
	LogInfo(aMessage);

	if(NumAppliedUniforms == 0 && !BridgeState.m_SectionValues.empty())
	{
		for(const auto &[SectionName, SectionValues] : BridgeState.m_SectionValues)
		{
			if(SectionValues.empty())
				continue;

			std::vector<std::string> CandidateEffectNames;
			const auto RuntimeEffectsIt = RuntimeEffectsBySection.find(SectionName);
			if(RuntimeEffectsIt != RuntimeEffectsBySection.end())
				CandidateEffectNames = RuntimeEffectsIt->second;
			if(CandidateEffectNames.empty())
				CandidateEffectNames = BuildFallbackRuntimeEffectsForSection(vRuntimeTechniques, vRuntimeUniforms, SectionName);

			std::vector<std::string> vUniformSamples;
			for(const std::string &EffectName : CandidateEffectNames)
			{
				for(const SRuntimeUniformInfo &RuntimeUniform : vRuntimeUniforms)
				{
					if(RuntimeUniform.m_EffectName != EffectName)
						continue;
					if(vUniformSamples.size() >= 8)
						break;
					AddUniqueString(vUniformSamples, RuntimeUniform.m_Name);
				}
				if(vUniformSamples.size() >= 8)
					break;
			}

			std::string SectionUniforms;
			for(const auto &[UniformName, UniformValue] : SectionValues)
			{
				(void)UniformValue;
				if(!SectionUniforms.empty())
					SectionUniforms += ", ";
				SectionUniforms += UniformName;
				if(SectionUniforms.size() > 256)
					break;
			}

			std::string CandidateEffects;
			for(const std::string &EffectName : CandidateEffectNames)
			{
				if(!CandidateEffects.empty())
					CandidateEffects += ", ";
				CandidateEffects += EffectName;
				if(CandidateEffects.size() > 256)
					break;
			}

			std::string RuntimeUniforms;
			for(const std::string &UniformName : vUniformSamples)
			{
				if(!RuntimeUniforms.empty())
					RuntimeUniforms += ", ";
				RuntimeUniforms += UniformName;
			}

			std::snprintf(
				aMessage, sizeof(aMessage),
				"[BestClient/ReShadeBridge] Bridge diagnostics: section=%s preset_uniforms=[%s] runtime_effects=[%s] runtime_uniforms=[%s]",
				SectionName.c_str(),
				SectionUniforms.c_str(),
				CandidateEffects.c_str(),
				RuntimeUniforms.c_str());
			LogWarning(aMessage);
			break;
		}
	}

	return true;
}

void RefreshCommandPath(reshade::api::effect_runtime *pRuntime, SRuntimeState &State)
{
	char aPresetPath[4096] = {0};
	pRuntime->get_current_preset_path(aPresetPath);
	const std::filesystem::path PresetPath = std::filesystem::u8path(aPresetPath);
	State.m_CommandPath = PresetPath.empty() ? std::filesystem::u8path(gs_pBridgeFilename) : (PresetPath.parent_path() / gs_pBridgeFilename);
}

int CountLoadedTechniques(reshade::api::effect_runtime *pRuntime)
{
	int NumTechniques = 0;
	pRuntime->enumerate_techniques(nullptr, [&](reshade::api::effect_runtime *pCurrentRuntime, reshade::api::effect_technique Technique) {
		(void)pCurrentRuntime;
		(void)Technique;
		++NumTechniques;
	});
	return NumTechniques;
}

void OnInitEffectRuntime(reshade::api::effect_runtime *pRuntime)
{
	SRuntimeState &State = gs_RuntimeStates[pRuntime];
	RefreshCommandPath(pRuntime, State);
	State.m_HasWriteTime = QueryCommandFingerprint(State.m_CommandPath, State.m_LastWriteTime, State.m_LastHash);
	State.m_HasHash = State.m_HasWriteTime;
	State.m_LastFingerprintCheck = {};
	State.m_EffectsReady = false;
	State.m_HasAppliedCommand = false;

	char aMessage[4096];
	std::snprintf(aMessage, sizeof(aMessage), "[BestClient/ReShadeBridge] Runtime initialized. Watching bridge state: %s", State.m_CommandPath.string().c_str());
	LogInfo(aMessage);
}

void OnDestroyEffectRuntime(reshade::api::effect_runtime *pRuntime)
{
	gs_RuntimeStates.erase(pRuntime);
	LogInfo("[BestClient/ReShadeBridge] Runtime destroyed.");
}

void OnPresent(reshade::api::effect_runtime *pRuntime)
{
	auto It = gs_RuntimeStates.find(pRuntime);
	if(It == gs_RuntimeStates.end())
		return;

	SRuntimeState &State = It->second;

	const std::filesystem::path PreviousCommandPath = State.m_CommandPath;
	RefreshCommandPath(pRuntime, State);
	if(State.m_CommandPath != PreviousCommandPath)
	{
		State.m_HasWriteTime = QueryCommandFingerprint(State.m_CommandPath, State.m_LastWriteTime, State.m_LastHash);
		State.m_HasHash = State.m_HasWriteTime;
		State.m_LastFingerprintCheck = {};
		State.m_HasAppliedCommand = false;
		State.m_EffectsReady = false;
	}

	if(State.m_CommandPath.empty() || !std::filesystem::exists(State.m_CommandPath))
		return;

	if(!State.m_EffectsReady)
	{
		if(CountLoadedTechniques(pRuntime) <= 0)
			return;
		State.m_EffectsReady = true;
		State.m_HasAppliedCommand = false;
		State.m_LastFingerprintCheck = {};
	}

	const auto Now = std::chrono::steady_clock::now();
	if(State.m_LastFingerprintCheck.time_since_epoch().count() != 0 &&
		Now - State.m_LastFingerprintCheck < std::chrono::milliseconds(40))
	{
		return;
	}
	State.m_LastFingerprintCheck = Now;

	std::filesystem::file_time_type WriteTime;
	size_t Hash = 0;
	if(!QueryCommandFingerprint(State.m_CommandPath, WriteTime, Hash))
		return;

	if(State.m_HasAppliedCommand && State.m_HasWriteTime && State.m_HasHash &&
		WriteTime == State.m_LastWriteTime && Hash == State.m_LastHash)
	{
		return;
	}

	State.m_LastWriteTime = WriteTime;
	State.m_HasWriteTime = true;
	State.m_LastHash = Hash;
	State.m_HasHash = true;

	if(!ApplyBridgeState(pRuntime, State.m_CommandPath, State))
	{
		char aMessage[4096];
		std::snprintf(aMessage, sizeof(aMessage), "[BestClient/ReShadeBridge] Failed to apply bridge state from: %s", State.m_CommandPath.string().c_str());
		LogWarning(aMessage);
	}
}

void OnReloadedEffects(reshade::api::effect_runtime *pRuntime)
{
	auto It = gs_RuntimeStates.find(pRuntime);
	if(It == gs_RuntimeStates.end())
		return;

	SRuntimeState &State = It->second;
	State.m_EffectsReady = false;
	State.m_HasAppliedCommand = false;
	State.m_LastFingerprintCheck = {};
	RefreshCommandPath(pRuntime, State);
}
}

extern "C" __declspec(dllexport) bool AddonInit(HMODULE AddonModule, HMODULE ReShadeModule)
{
	if(!reshade::register_addon(AddonModule, ReShadeModule))
		return false;

	reshade::register_event<reshade::addon_event::init_effect_runtime>(&OnInitEffectRuntime);
	reshade::register_event<reshade::addon_event::destroy_effect_runtime>(&OnDestroyEffectRuntime);
	reshade::register_event<reshade::addon_event::reshade_reloaded_effects>(&OnReloadedEffects);
	reshade::register_event<reshade::addon_event::reshade_present>(&OnPresent);
	LogInfo("[BestClient/ReShadeBridge] Add-on initialized.");
	return true;
}

extern "C" __declspec(dllexport) void AddonUninit(HMODULE AddonModule, HMODULE ReShadeModule)
{
	(void)ReShadeModule;
	reshade::unregister_event<reshade::addon_event::reshade_present>(&OnPresent);
	reshade::unregister_event<reshade::addon_event::reshade_reloaded_effects>(&OnReloadedEffects);
	reshade::unregister_event<reshade::addon_event::destroy_effect_runtime>(&OnDestroyEffectRuntime);
	reshade::unregister_event<reshade::addon_event::init_effect_runtime>(&OnInitEffectRuntime);
	reshade::unregister_addon(AddonModule, ReShadeModule);
	gs_RuntimeStates.clear();
}

#endif
