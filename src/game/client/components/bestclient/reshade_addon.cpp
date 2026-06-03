#if defined(_WIN32)

#include <algorithm>
#include <cctype>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <functional>
#include <optional>
#include <reshade.hpp>
#include <sstream>
#include <string>
#include <system_error>
#include <unordered_map>
#include <unordered_set>
#include <vector>

extern "C" __declspec(dllexport) const char *NAME = "BestClient ReShade Live Sync";
extern "C" __declspec(dllexport) const char *DESCRIPTION = "Applies BestClient ReShade state changes to the current runtime live.";
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

	struct SPresetState
	{
		std::unordered_set<std::string> m_EnabledTokens;
		std::unordered_set<std::string> m_EnabledEffectIdentifiers;
		std::unordered_map<std::string, std::unordered_map<std::string, SUniformValue>> m_SectionValues;
	};

	struct SRuntimeState
	{
		std::filesystem::path m_PresetPath;
		std::filesystem::file_time_type m_LastWriteTime = {};
		bool m_HasWriteTime = false;
		size_t m_LastPresetHash = 0;
		bool m_HasPresetHash = false;
		size_t m_LastZeroUniformDiagnosticsHash = 0;
		bool m_HasZeroUniformDiagnosticsHash = false;
		std::chrono::steady_clock::time_point m_LastFingerprintCheck = {};
		bool m_PresetReadWarningShown = false;
		bool m_HasAppliedPreset = false;
		bool m_EffectsReady = false;
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

	bool QueryWriteTime(const std::filesystem::path &Path, std::filesystem::file_time_type &WriteTime)
	{
		std::error_code Error;
		WriteTime = std::filesystem::last_write_time(Path, Error);
		return !Error;
	}

	bool QueryPresetFingerprint(const std::filesystem::path &Path, std::filesystem::file_time_type &WriteTime, size_t &Hash)
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

	bool HasBridgeCompanionFile(const std::filesystem::path &PresetPath)
	{
		if(PresetPath.empty())
			return false;

		const std::filesystem::path BridgePath = PresetPath.parent_path() / gs_pBridgeFilename;
		return std::filesystem::exists(BridgePath);
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

	std::optional<std::string> ReadTextFile(const std::filesystem::path &Path)
	{
		std::ifstream File(Path, std::ios::in | std::ios::binary);
		if(!File.is_open())
			return std::nullopt;

		std::ostringstream Buffer;
		Buffer << File.rdbuf();
		return Buffer.str();
	}

	std::vector<std::string> SplitCommaSeparated(const std::string &Text);
	std::string NormalizeEffectIdentifier(const std::string &EffectName);
	std::string EffectIdentifierStem(const std::string &EffectName);
	std::string NormalizeUniformIdentifier(const std::string &UniformName);

	void AddEnabledEffectIdentifiers(SPresetState &State, const std::string &Token)
	{
		const size_t SeparatorPos = Token.find('@');
		if(SeparatorPos == std::string::npos || SeparatorPos + 1 >= Token.size())
			return;

		const std::string EffectName = Token.substr(SeparatorPos + 1);
		const std::string Normalized = NormalizeEffectIdentifier(EffectName);
		if(!Normalized.empty())
		{
			State.m_EnabledEffectIdentifiers.insert(Normalized);
			const std::string Stem = EffectIdentifierStem(Normalized);
			if(!Stem.empty())
				State.m_EnabledEffectIdentifiers.insert(Stem);
		}
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

		for(char &c : Normalized)
			c = (char)std::tolower((unsigned char)c);
		return Normalized;
	}

	const SUniformValue *FindUniformValueForRuntimeVariable(const std::unordered_map<std::string, SUniformValue> &SectionValues, const char *pUniformName)
	{
		if(pUniformName == nullptr || pUniformName[0] == '\0')
			return nullptr;

		const auto ExactIt = SectionValues.find(pUniformName);
		if(ExactIt != SectionValues.end())
			return &ExactIt->second;

		const std::string RuntimeNormalized = NormalizeUniformIdentifier(pUniformName);
		for(const auto &[UniformName, UniformValue] : SectionValues)
		{
			if(NormalizeUniformIdentifier(UniformName) == RuntimeNormalized)
				return &UniformValue;
		}

		return nullptr;
	}

	bool ApplyUniformValueToVariable(reshade::api::effect_runtime *pRuntime, reshade::api::effect_uniform_variable Variable, const SUniformValue &UniformValue)
	{
		reshade::api::format BaseType = reshade::api::format::unknown;
		uint32_t Rows = 0;
		uint32_t Columns = 0;
		uint32_t ArrayLength = 0;
		pRuntime->get_uniform_variable_type(Variable, &BaseType, &Rows, &Columns, &ArrayLength);
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
			else if(UniformValue.m_Type == EUniformValueType::UINT && ComponentCount == 1)
			{
				const int32_t SignedValue = (int32_t)UniformValue.m_UintValue;
				pRuntime->set_uniform_value_int(Variable, &SignedValue, 1);
				return true;
			}
			else if(UniformValue.m_Type == EUniformValueType::BOOL && ComponentCount == 1)
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
			else if(UniformValue.m_Type == EUniformValueType::INT && UniformValue.m_IntValue >= 0 && ComponentCount == 1)
			{
				const uint32_t UnsignedValue = (uint32_t)UniformValue.m_IntValue;
				pRuntime->set_uniform_value_uint(Variable, &UnsignedValue, 1);
				return true;
			}
			else if(UniformValue.m_Type == EUniformValueType::BOOL && ComponentCount == 1)
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
			else if(UniformValue.m_Type == EUniformValueType::FLOAT && ComponentCount == 1)
			{
				pRuntime->set_uniform_value_float(Variable, &UniformValue.m_FloatValue, 1);
				return true;
			}
			else if(UniformValue.m_Type == EUniformValueType::INT && ComponentCount == 1)
			{
				const float FloatValue = (float)UniformValue.m_IntValue;
				pRuntime->set_uniform_value_float(Variable, &FloatValue, 1);
				return true;
			}
			else if(UniformValue.m_Type == EUniformValueType::UINT && ComponentCount == 1)
			{
				const float FloatValue = (float)UniformValue.m_UintValue;
				pRuntime->set_uniform_value_float(Variable, &FloatValue, 1);
				return true;
			}
			else if(UniformValue.m_Type == EUniformValueType::BOOL && ComponentCount == 1)
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

	bool TryParseFloat(const std::string &Text, float &Value)
	{
		char *pEnd = nullptr;
		Value = std::strtof(Text.c_str(), &pEnd);
		return pEnd != Text.c_str() && pEnd != nullptr && *pEnd == '\0';
	}

	bool TryParseFloatVector(const std::string &Text, std::vector<float> &vValues)
	{
		vValues.clear();
		for(const std::string &Token : SplitCommaSeparated(Text))
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

	bool TryParseBool(const std::string &Text, bool &Value)
	{
		if(_stricmp(Text.c_str(), "true") == 0 || Text == "1")
		{
			Value = true;
			return true;
		}
		if(_stricmp(Text.c_str(), "false") == 0 || Text == "0")
		{
			Value = false;
			return true;
		}
		return false;
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

	bool ParsePresetState(const std::filesystem::path &PresetPath, SPresetState &State)
	{
		const std::optional<std::string> PresetText = ReadTextFile(PresetPath);
		if(!PresetText.has_value())
			return false;

		State = SPresetState{};

		std::istringstream Stream(*PresetText);
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
				if(Key == "Techniques")
				{
					for(const std::string &Token : SplitCommaSeparated(Value))
					{
						State.m_EnabledTokens.insert(Token);
						AddEnabledEffectIdentifiers(State, Token);
					}
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

	std::string BuildTechniqueToken(const char *pTechniqueName, const char *pEffectName)
	{
		if(pTechniqueName == nullptr || pTechniqueName[0] == '\0')
			return "";
		if(pEffectName == nullptr || pEffectName[0] == '\0')
			return pTechniqueName;
		return std::string(pTechniqueName) + "@" + pEffectName;
	}

	std::string NormalizeEffectIdentifier(const std::string &EffectName)
	{
		std::string Normalized = EffectName;
		std::replace(Normalized.begin(), Normalized.end(), '\\', '/');
		const size_t SlashPos = Normalized.find_last_of('/');
		if(SlashPos != std::string::npos)
			Normalized = Normalized.substr(SlashPos + 1);
		for(char &c : Normalized)
			c = (char)std::tolower((unsigned char)c);
		return Normalized;
	}

	std::string EffectIdentifierStem(const std::string &EffectName)
	{
		const std::filesystem::path Path = std::filesystem::u8path(EffectName);
		std::string Stem = Path.stem().string();
		for(char &c : Stem)
			c = (char)std::tolower((unsigned char)c);
		return Stem;
	}

	const std::unordered_map<std::string, SUniformValue> *FindPresetSectionForRuntimeEffect(const SPresetState &PresetState, const char *pEffectName)
	{
		if(pEffectName == nullptr || pEffectName[0] == '\0')
			return nullptr;

		const std::string RuntimeNormalized = NormalizeEffectIdentifier(pEffectName);
		const std::string RuntimeStem = EffectIdentifierStem(RuntimeNormalized);
		for(const auto &[SectionName, SectionValues] : PresetState.m_SectionValues)
		{
			if(SectionValues.empty())
				continue;

			const std::string SectionNormalized = NormalizeEffectIdentifier(SectionName);
			if(SectionNormalized == RuntimeNormalized)
				return &SectionValues;

			if(!RuntimeStem.empty() && EffectIdentifierStem(SectionNormalized) == RuntimeStem)
				return &SectionValues;
		}

		return nullptr;
	}

	void ApplyTechniqueStates(reshade::api::effect_runtime *pRuntime, const SPresetState &PresetState, int &NumAppliedTechniques)
	{
		NumAppliedTechniques = 0;
		pRuntime->enumerate_techniques(nullptr, [&](reshade::api::effect_runtime *pCurrentRuntime, reshade::api::effect_technique Technique) {
			char aTechniqueName[512] = {0};
			char aEffectName[512] = {0};
			pCurrentRuntime->get_technique_name(Technique, aTechniqueName);
			pCurrentRuntime->get_technique_effect_name(Technique, aEffectName);
			const std::string Token = BuildTechniqueToken(aTechniqueName, aEffectName);
			const bool Enabled = PresetState.m_EnabledTokens.find(Token) != PresetState.m_EnabledTokens.end() ||
					     PresetState.m_EnabledTokens.find(aTechniqueName) != PresetState.m_EnabledTokens.end();
			pCurrentRuntime->set_technique_state(Technique, Enabled);
			++NumAppliedTechniques;
		});
	}

	int ApplyUniformStates(reshade::api::effect_runtime *pRuntime, const SPresetState &PresetState)
	{
		std::unordered_map<std::string, std::vector<std::string>> RuntimeEffectsByIdentifier;
		auto AddRuntimeEffectName = [&](const char *pEffectName) {
			if(pEffectName == nullptr || pEffectName[0] == '\0')
				return;

			const std::string ActualName = pEffectName;
			const std::string Normalized = NormalizeEffectIdentifier(ActualName);
			const std::string Stem = EffectIdentifierStem(Normalized);
			auto AddIdentifier = [&](const std::string &Identifier) {
				if(Identifier.empty())
					return;
				auto &vEffectNames = RuntimeEffectsByIdentifier[Identifier];
				if(std::find(vEffectNames.begin(), vEffectNames.end(), ActualName) == vEffectNames.end())
					vEffectNames.push_back(ActualName);
			};
			AddIdentifier(Normalized);
			AddIdentifier(Stem);
		};

		pRuntime->enumerate_techniques(nullptr, [&](reshade::api::effect_runtime *pCurrentRuntime, reshade::api::effect_technique Technique) {
			char aEffectName[512] = {0};
			pCurrentRuntime->get_technique_effect_name(Technique, aEffectName);
			AddRuntimeEffectName(aEffectName);
		});
		pRuntime->enumerate_uniform_variables(nullptr, [&](reshade::api::effect_runtime *pCurrentRuntime, reshade::api::effect_uniform_variable Variable) {
			char aEffectName[512] = {0};
			pCurrentRuntime->get_uniform_variable_effect_name(Variable, aEffectName);
			AddRuntimeEffectName(aEffectName);
		});

		int NumAppliedUniforms = 0;

		for(const auto &[SectionName, SectionValues] : PresetState.m_SectionValues)
		{
			if(SectionValues.empty())
				continue;

			const std::string SectionNormalized = NormalizeEffectIdentifier(SectionName);
			const std::string SectionStem = EffectIdentifierStem(SectionNormalized);
			std::vector<std::string> CandidateEffectNames;
			auto AppendCandidates = [&](const std::string &Identifier) {
				const auto It = RuntimeEffectsByIdentifier.find(Identifier);
				if(It == RuntimeEffectsByIdentifier.end())
					return;
				for(const std::string &EffectName : It->second)
				{
					if(std::find(CandidateEffectNames.begin(), CandidateEffectNames.end(), EffectName) == CandidateEffectNames.end())
						CandidateEffectNames.push_back(EffectName);
				}
			};
			AppendCandidates(SectionNormalized);
			AppendCandidates(SectionStem);
			if(CandidateEffectNames.empty())
				continue;

			for(const auto &[UniformName, UniformValue] : SectionValues)
			{
				bool Applied = false;
				for(const std::string &EffectName : CandidateEffectNames)
				{
					reshade::api::effect_uniform_variable Variable = pRuntime->find_uniform_variable(EffectName.c_str(), UniformName.c_str());
					if(Variable.handle == 0)
					{
						pRuntime->enumerate_uniform_variables(EffectName.c_str(), [&](reshade::api::effect_runtime *pCurrentRuntime, reshade::api::effect_uniform_variable CandidateVariable) {
							if(Applied || Variable.handle != 0)
								return;

							char aCandidateUniformName[512] = {0};
							pCurrentRuntime->get_uniform_variable_name(CandidateVariable, aCandidateUniformName);
							const SUniformValue *pMatchedValue = FindUniformValueForRuntimeVariable(SectionValues, aCandidateUniformName);
							if(pMatchedValue == nullptr)
								return;
							if(NormalizeUniformIdentifier(aCandidateUniformName) != NormalizeUniformIdentifier(UniformName))
								return;
							Variable = CandidateVariable;
						});
					}

					if(Variable.handle != 0 && ApplyUniformValueToVariable(pRuntime, Variable, UniformValue))
					{
						++NumAppliedUniforms;
						Applied = true;
						break;
					}
				}
			}
		}

		return NumAppliedUniforms;
	}

	bool ApplyPresetState(reshade::api::effect_runtime *pRuntime, const std::filesystem::path &PresetPath, SRuntimeState &RuntimeState)
	{
		SPresetState PresetState;
		if(!ParsePresetState(PresetPath, PresetState))
		{
			if(!RuntimeState.m_PresetReadWarningShown)
			{
				char aMessage[4608];
				std::snprintf(aMessage, sizeof(aMessage), "[BestClient/ReShadeAddon] Failed to read preset state from: %s", PresetPath.string().c_str());
				LogWarning(aMessage);
				RuntimeState.m_PresetReadWarningShown = true;
			}
			return false;
		}
		RuntimeState.m_PresetReadWarningShown = false;

		int NumAppliedTechniques = 0;
		ApplyTechniqueStates(pRuntime, PresetState, NumAppliedTechniques);
		const int NumAppliedUniforms = ApplyUniformStates(pRuntime, PresetState);

		if(NumAppliedTechniques == 0)
		{
			char aMessage[4608];
			std::snprintf(aMessage, sizeof(aMessage), "[BestClient/ReShadeAddon] Delaying live apply until ReShade finishes loading effects: %s", PresetPath.string().c_str());
			LogInfo(aMessage);
			RuntimeState.m_HasAppliedPreset = false;
			return false;
		}

		RuntimeState.m_HasAppliedPreset = true;

		char aMessage[4608];
		std::snprintf(
			aMessage, sizeof(aMessage),
			"[BestClient/ReShadeAddon] Applied preset live: techniques=%d sections=%zu uniforms=%d",
			NumAppliedTechniques,
			PresetState.m_SectionValues.size(),
			NumAppliedUniforms);
		LogInfo(aMessage);

		if(NumAppliedUniforms == 0 && !PresetState.m_SectionValues.empty() &&
			(!RuntimeState.m_HasZeroUniformDiagnosticsHash || RuntimeState.m_LastZeroUniformDiagnosticsHash != RuntimeState.m_LastPresetHash))
		{
			std::string SectionsSummary;
			int NumSectionsLogged = 0;
			for(const auto &[SectionName, SectionValues] : PresetState.m_SectionValues)
			{
				(void)SectionValues;
				if(!SectionsSummary.empty())
					SectionsSummary += ", ";
				SectionsSummary += SectionName;
				if(++NumSectionsLogged >= 6)
					break;
			}

			std::string UniformSummary;
			int NumUniformsLogged = 0;
			pRuntime->enumerate_uniform_variables(nullptr, [&](reshade::api::effect_runtime *pCurrentRuntime, reshade::api::effect_uniform_variable Variable) {
				if(NumUniformsLogged >= 10)
					return;

				char aEffectName[512] = {0};
				char aUniformName[512] = {0};
				pCurrentRuntime->get_uniform_variable_effect_name(Variable, aEffectName);
				pCurrentRuntime->get_uniform_variable_name(Variable, aUniformName);
				if(aEffectName[0] == '\0' || aUniformName[0] == '\0')
					return;

				if(!UniformSummary.empty())
					UniformSummary += ", ";
				UniformSummary += std::string(aEffectName) + "::" + aUniformName;
				++NumUniformsLogged;
			});

			char aDiagnostics[4608];
			std::snprintf(
				aDiagnostics, sizeof(aDiagnostics),
				"[BestClient/ReShadeAddon] Live diagnostics: no uniforms matched. preset_sections=[%s] runtime_uniforms_sample=[%s]",
				SectionsSummary.c_str(),
				UniformSummary.c_str());
			LogWarning(aDiagnostics);

			if(PresetState.m_SectionValues.size() == 1)
			{
				const auto &OnlySection = *PresetState.m_SectionValues.begin();
				const std::string SectionNormalized = NormalizeEffectIdentifier(OnlySection.first);
				const std::string SectionStem = EffectIdentifierStem(SectionNormalized);

				std::string MatchingEffectNames;
				int NumMatchingEffects = 0;
				pRuntime->enumerate_techniques(nullptr, [&](reshade::api::effect_runtime *pCurrentRuntime, reshade::api::effect_technique Technique) {
					char aTechniqueName[512] = {0};
					char aEffectName[512] = {0};
					pCurrentRuntime->get_technique_name(Technique, aTechniqueName);
					pCurrentRuntime->get_technique_effect_name(Technique, aEffectName);
					const std::string RuntimeNormalized = NormalizeEffectIdentifier(aEffectName);
					const std::string RuntimeStem = EffectIdentifierStem(RuntimeNormalized);
					if(RuntimeNormalized != SectionNormalized && RuntimeStem != SectionStem)
						return;

					if(!MatchingEffectNames.empty())
						MatchingEffectNames += ", ";
					MatchingEffectNames += std::string(aEffectName) + "::" + aTechniqueName;
					++NumMatchingEffects;
				});

				std::string MatchingUniformNames;
				int NumMatchingUniforms = 0;
				pRuntime->enumerate_uniform_variables(nullptr, [&](reshade::api::effect_runtime *pCurrentRuntime, reshade::api::effect_uniform_variable Variable) {
					char aEffectName[512] = {0};
					char aUniformName[512] = {0};
					pCurrentRuntime->get_uniform_variable_effect_name(Variable, aEffectName);
					pCurrentRuntime->get_uniform_variable_name(Variable, aUniformName);
					const std::string RuntimeNormalized = NormalizeEffectIdentifier(aEffectName);
					const std::string RuntimeStem = EffectIdentifierStem(RuntimeNormalized);
					if(RuntimeNormalized != SectionNormalized && RuntimeStem != SectionStem)
						return;

					if(!MatchingUniformNames.empty())
						MatchingUniformNames += ", ";
					MatchingUniformNames += std::string(aEffectName) + "::" + aUniformName;
					++NumMatchingUniforms;
					if(NumMatchingUniforms >= 20)
						return;
				});

				std::string PresetUniformNames;
				for(const auto &[UniformName, UniformValue] : OnlySection.second)
				{
					(void)UniformValue;
					if(!PresetUniformNames.empty())
						PresetUniformNames += ", ";
					PresetUniformNames += UniformName;
				}

				char aSectionDiagnostics[4608];
				std::snprintf(
					aSectionDiagnostics, sizeof(aSectionDiagnostics),
					"[BestClient/ReShadeAddon] Section diagnostics: section=%s normalized=%s stem=%s preset_uniforms=[%s] runtime_matching_effects=[%s] runtime_matching_uniforms=[%s]",
					OnlySection.first.c_str(),
					SectionNormalized.c_str(),
					SectionStem.c_str(),
					PresetUniformNames.c_str(),
					MatchingEffectNames.c_str(),
					MatchingUniformNames.c_str());
				LogWarning(aSectionDiagnostics);
			}

			RuntimeState.m_LastZeroUniformDiagnosticsHash = RuntimeState.m_LastPresetHash;
			RuntimeState.m_HasZeroUniformDiagnosticsHash = true;
		}

		return true;
	}

	void RefreshPresetPath(reshade::api::effect_runtime *pRuntime, SRuntimeState &State)
	{
		char aPresetPath[4096] = {0};
		pRuntime->get_current_preset_path(aPresetPath);
		State.m_PresetPath = std::filesystem::u8path(aPresetPath);
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
		RefreshPresetPath(pRuntime, State);
		State.m_HasWriteTime = QueryPresetFingerprint(State.m_PresetPath, State.m_LastWriteTime, State.m_LastPresetHash);
		State.m_HasPresetHash = State.m_HasWriteTime;
		State.m_LastFingerprintCheck = std::chrono::steady_clock::now();
		State.m_EffectsReady = false;
		const std::string PresetPathString = State.m_PresetPath.string();

		char aMessage[4608];
		std::snprintf(aMessage, sizeof(aMessage), "[BestClient/ReShadeAddon] Runtime initialized. Watching preset: %s", PresetPathString.c_str());
		LogInfo(aMessage);
	}

	void OnDestroyEffectRuntime(reshade::api::effect_runtime *pRuntime)
	{
		gs_RuntimeStates.erase(pRuntime);
		LogInfo("[BestClient/ReShadeAddon] Runtime destroyed.");
	}

	void OnPresent(reshade::api::effect_runtime *pRuntime)
	{
		auto It = gs_RuntimeStates.find(pRuntime);
		if(It == gs_RuntimeStates.end())
			return;

		SRuntimeState &State = It->second;

		char aPresetPath[4096] = {0};
		pRuntime->get_current_preset_path(aPresetPath);
		const std::filesystem::path CurrentPresetPath = std::filesystem::u8path(aPresetPath);
		if(CurrentPresetPath != State.m_PresetPath)
		{
			State.m_PresetPath = CurrentPresetPath;
			State.m_HasWriteTime = QueryPresetFingerprint(State.m_PresetPath, State.m_LastWriteTime, State.m_LastPresetHash);
			State.m_HasPresetHash = State.m_HasWriteTime;
			State.m_LastFingerprintCheck = {};
			State.m_HasAppliedPreset = false;
			State.m_EffectsReady = false;
		}

		if(State.m_PresetPath.empty())
			return;
		if(HasBridgeCompanionFile(State.m_PresetPath))
			return;
		if(!State.m_EffectsReady)
		{
			if(CountLoadedTechniques(pRuntime) <= 0)
				return;
			State.m_EffectsReady = true;
			State.m_HasAppliedPreset = false;
			State.m_LastFingerprintCheck = {};
		}

		const auto Now = std::chrono::steady_clock::now();
		if(State.m_HasAppliedPreset && State.m_LastFingerprintCheck.time_since_epoch().count() != 0 &&
			Now - State.m_LastFingerprintCheck < std::chrono::milliseconds(200))
		{
			return;
		}
		State.m_LastFingerprintCheck = Now;

		std::filesystem::file_time_type WriteTime;
		if(!QueryWriteTime(State.m_PresetPath, WriteTime))
			return;

		// mtime unchanged since the applied preset: skip the full file read + hash.
		if(State.m_HasAppliedPreset && State.m_HasWriteTime && WriteTime == State.m_LastWriteTime)
			return;

		size_t PresetHash = 0;
		if(!QueryPresetFingerprint(State.m_PresetPath, WriteTime, PresetHash))
			return;

		if(State.m_HasAppliedPreset && State.m_HasWriteTime && State.m_HasPresetHash &&
			WriteTime == State.m_LastWriteTime && PresetHash == State.m_LastPresetHash)
		{
			return;
		}

		State.m_LastWriteTime = WriteTime;
		State.m_HasWriteTime = true;
		State.m_LastPresetHash = PresetHash;
		State.m_HasPresetHash = true;

		ApplyPresetState(pRuntime, State.m_PresetPath, State);
	}

	void OnReloadedEffects(reshade::api::effect_runtime *pRuntime)
	{
		auto It = gs_RuntimeStates.find(pRuntime);
		if(It == gs_RuntimeStates.end())
			return;

		SRuntimeState &State = It->second;
		State.m_EffectsReady = false;
		State.m_HasAppliedPreset = false;
		State.m_LastFingerprintCheck = {};
		RefreshPresetPath(pRuntime, State);

		std::filesystem::file_time_type WriteTime;
		size_t PresetHash = 0;
		if(QueryPresetFingerprint(State.m_PresetPath, WriteTime, PresetHash))
		{
			State.m_LastWriteTime = WriteTime;
			State.m_HasWriteTime = true;
			State.m_LastPresetHash = PresetHash;
			State.m_HasPresetHash = true;
		}
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
	LogInfo("[BestClient/ReShadeAddon] Add-on initialized.");
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
