#include "stdafx.h"

#include "ServerProperties.h"

#include "ServerLogger.h"

#include <cctype>
#include <fstream>
#include <map>
#include <stdio.h>
#include <unordered_map>

namespace ServerRuntime
{
struct ServerPropertyDefault
{
	const char *key;
	const char *value;
};

static const char *kServerPropertiesPath = "server.properties";
static const size_t kMaxSaveIdLength = 31;

static const ServerPropertyDefault kServerPropertyDefaults[] =
{
	{ "level-name", "world" },
	{ "level-id", "world" },
	{ "level-type", "default" },
	{ "gamemode", "0" },
	{ "max-build-height", "256" },
	{ "spawn-animals", "true" },
	{ "spawn-npcs", "true" },
	{ "spawn-monsters", "true" },
	{ "pvp", "true" },
	{ "server-ip", "" },
	{ "motd", "A Minecraft Server" }
};

static std::string TrimAscii(const std::string &value)
{
	size_t start = 0;
	while (start < value.length() && std::isspace((unsigned char)value[start]))
	{
		++start;
	}

	size_t end = value.length();
	while (end > start && std::isspace((unsigned char)value[end - 1]))
	{
		--end;
	}

	return value.substr(start, end - start);
}

/**
 * 任意文字列を保存先IDとして安全な形式に正規化する
 *
 * 変換ルール:
 * - 英字は小文字化
 * - `[a-z0-9_.-]` のみ保持
 * - 空白/非対応文字は `_` に置換
 * - 空値は `world` に補正
 * - 最大長はストレージ制約に合わせて制限
 */
static std::string NormalizeSaveId(const std::string &source)
{
	std::string out;
	out.reserve(source.length());

	// Storage 側の保存先IDとして安全に扱える文字セットへ正規化する
	// 不正文字は '_' に落とし、大小は吸収して衝突を減らす
	for (size_t i = 0; i < source.length(); ++i)
	{
		unsigned char ch = (unsigned char)source[i];
		if (ch >= 'A' && ch <= 'Z')
		{
			ch = (unsigned char)(ch - 'A' + 'a');
		}

		const bool alnum = (ch >= 'a' && ch <= 'z') || (ch >= '0' && ch <= '9');
		const bool passthrough = (ch == '_') || (ch == '-') || (ch == '.');
		if (alnum || passthrough)
		{
			out.push_back((char)ch);
		}
		else if (std::isspace(ch))
		{
			out.push_back('_');
		}
		else if (ch < 0x80)
		{
			out.push_back('_');
		}
	}

	if (out.empty())
	{
		out = "world";
	}

	// 先頭文字が扱いづらいケースを避けるため、必要に応じて接頭辞を付与する
	if (!((out[0] >= 'a' && out[0] <= 'z') || (out[0] >= '0' && out[0] <= '9')))
	{
		out = std::string("w_") + out;
	}

	// 4J 側の filename バッファ制約に合わせて長さを制限する
	if (out.length() > kMaxSaveIdLength)
	{
		out.resize(kMaxSaveIdLength);
	}

	return out;
}

static void ApplyDefaultServerProperties(std::unordered_map<std::string, std::string> *properties)
{
	if (properties == NULL)
	{
		return;
	}

	const size_t defaultCount = sizeof(kServerPropertyDefaults) / sizeof(kServerPropertyDefaults[0]);
	for (size_t i = 0; i < defaultCount; ++i)
	{
		(*properties)[kServerPropertyDefaults[i].key] = kServerPropertyDefaults[i].value;
	}
}

/**
 * `server.properties` 形式のテキストをパースして key/value を抽出する
 *
 * - `#` / `!` 始まりはコメントとして無視
 * - `=` または `:` を区切りとして解釈
 * - 不正行はスキップして継続
 */
static bool ReadServerPropertiesFile(const char *filePath, std::unordered_map<std::string, std::string> *properties, int *outParsedCount)
{
	if (properties == NULL)
	{
		return false;
	}

	std::ifstream inFile(filePath, std::ios::in | std::ios::binary);
	if (!inFile.is_open())
	{
		return false;
	}

	int parsedCount = 0;
	std::string line;
	while (std::getline(inFile, line))
	{
		if (!line.empty() && line[line.length() - 1] == '\r')
		{
			line.erase(line.length() - 1);
		}

		std::string trimmedLine = TrimAscii(line);
		if (trimmedLine.empty())
		{
			continue;
		}

		if (trimmedLine[0] == '#' || trimmedLine[0] == '!')
		{
			continue;
		}

		size_t eqPos = trimmedLine.find('=');
		size_t colonPos = trimmedLine.find(':');
		size_t sepPos = std::string::npos;
		if (eqPos == std::string::npos)
		{
			sepPos = colonPos;
		}
		else if (colonPos == std::string::npos)
		{
			sepPos = eqPos;
		}
		else
		{
			sepPos = (eqPos < colonPos) ? eqPos : colonPos;
		}

		if (sepPos == std::string::npos)
		{
			continue;
		}

		std::string key = TrimAscii(trimmedLine.substr(0, sepPos));
		if (key.empty())
		{
			continue;
		}

		std::string value = TrimAscii(trimmedLine.substr(sepPos + 1));
		(*properties)[key] = value;
		++parsedCount;
	}

	if (outParsedCount != NULL)
	{
		*outParsedCount = parsedCount;
	}

	return true;
}

/**
 * key/value を `server.properties` として書き戻す
 *
 * 出力順を安定化するため、キーをソートして保存する
 */
static bool WriteServerPropertiesFile(const char *filePath, const std::unordered_map<std::string, std::string> &properties)
{
	FILE *outFile = fopen(filePath, "wb");
	if (outFile == NULL)
	{
		return false;
	}

	fprintf(outFile, "# Minecraft server properties\n");
	fprintf(outFile, "# Auto-generated when missing\n");

	std::map<std::string, std::string> sortedProperties(properties.begin(), properties.end());
	for (std::map<std::string, std::string>::const_iterator it = sortedProperties.begin(); it != sortedProperties.end(); ++it)
	{
		fprintf(outFile, "%s=%s\n", it->first.c_str(), it->second.c_str());
	}

	fclose(outFile);
	return true;
}

/**
 * 実効的なワールド設定を読み込み、欠損/不正を補正して返す
 *
 * - ファイルが無い場合はデフォルトで生成
 * - 必須キー欠損時は補完
 * - `level-id` を安全形式へ正規化
 * - 修正が発生した場合は自動で再保存
 */
ServerPropertiesConfig LoadServerPropertiesConfig()
{
	ServerPropertiesConfig config;

	std::unordered_map<std::string, std::string> defaults;
	std::unordered_map<std::string, std::string> loaded;
	ApplyDefaultServerProperties(&defaults);

	int parsedCount = 0;
	bool readSuccess = ReadServerPropertiesFile(kServerPropertiesPath, &loaded, &parsedCount);
	std::unordered_map<std::string, std::string> merged = defaults;
	bool shouldWrite = false;

	if (!readSuccess)
	{
		LogWorldIO("server.properties not found or unreadable; creating defaults");
		shouldWrite = true;
	}
	else
	{
		if (parsedCount == 0)
		{
			LogWorldIO("server.properties has no properties; applying defaults");
			shouldWrite = true;
		}

		const size_t defaultCount = sizeof(kServerPropertyDefaults) / sizeof(kServerPropertyDefaults[0]);
		for (size_t i = 0; i < defaultCount; ++i)
		{
			if (loaded.find(kServerPropertyDefaults[i].key) == loaded.end())
			{
				shouldWrite = true;
				break;
			}
		}
	}

	for (std::unordered_map<std::string, std::string>::const_iterator it = loaded.begin(); it != loaded.end(); ++it)
	{
		// 既存値をデフォルトへ上書きマージして、未知キーも可能な限り維持する
		merged[it->first] = it->second;
	}

	std::string worldName = TrimAscii(merged["level-name"]);
	if (worldName.empty())
	{
		worldName = "world";
		shouldWrite = true;
	}

	std::string worldSaveId = TrimAscii(merged["level-id"]);
	if (worldSaveId.empty())
	{
		// level-id が未設定なら level-name から自動生成して保存先を固定する
		worldSaveId = NormalizeSaveId(worldName);
		shouldWrite = true;
	}
	else
	{
		// 既存の level-id も正規化して、将来の不整合を防ぐ
		std::string normalized = NormalizeSaveId(worldSaveId);
		if (normalized != worldSaveId)
		{
			worldSaveId = normalized;
			shouldWrite = true;
		}
	}

	merged["level-name"] = worldName;
	merged["level-id"] = worldSaveId;

	if (shouldWrite)
	{
		if (WriteServerPropertiesFile(kServerPropertiesPath, merged))
		{
			LogWorldIO("wrote server.properties");
		}
		else
		{
			LogWorldIO("failed to write server.properties");
		}
	}

	config.worldName = Utf8ToWide(worldName.c_str());
	config.worldSaveId = worldSaveId;
	return config;
}

/**
 * ワールド識別情報を保存しつつ、他設定キーを可能な限り保持する
 *
 * - 既存ファイルを読み取り、未知キーも含めてマージ
 * - `level-name` / `level-id` のみ更新して書き戻す
 */
bool SaveServerPropertiesConfig(const ServerPropertiesConfig &config)
{
	std::unordered_map<std::string, std::string> merged;
	ApplyDefaultServerProperties(&merged);

	std::unordered_map<std::string, std::string> loaded;
	int parsedCount = 0;
	if (ReadServerPropertiesFile(kServerPropertiesPath, &loaded, &parsedCount))
	{
		for (std::unordered_map<std::string, std::string>::const_iterator it = loaded.begin(); it != loaded.end(); ++it)
		{
			// 呼び出し側が触っていないキーを落とさないように、既存内容を保持する
			merged[it->first] = it->second;
		}
	}

	std::string worldName = TrimAscii(WideToUtf8(config.worldName));
	if (worldName.empty())
	{
		worldName = "world"; // フォルト名
	}

	std::string worldSaveId = TrimAscii(config.worldSaveId);
	if (worldSaveId.empty())
	{
		worldSaveId = NormalizeSaveId(worldName);
	}
	else
	{
		worldSaveId = NormalizeSaveId(worldSaveId);
	}

	merged["level-name"] = worldName;
	merged["level-id"] = worldSaveId;

	return WriteServerPropertiesFile(kServerPropertiesPath, merged);
}
}
