#pragma once

#include <string>

namespace ServerRuntime
{
	enum EServerLogLevel
	{
		eServerLogLevel_Debug = 0,
		eServerLogLevel_Info = 1,
		eServerLogLevel_Warn = 2,
		eServerLogLevel_Error = 3
	};

	/**
	 * 文字列をログレベルへ変換する（`debug`/`info`/`warn`/`error`）
	 *
	 * @param value 変換元文字列
	 * @param outLevel 変換結果の出力先
	 * @return 変換成功時 `true`
	 */
	bool TryParseServerLogLevel(const char *value, EServerLogLevel *outLevel);

	void SetServerLogLevel(EServerLogLevel level);
	EServerLogLevel GetServerLogLevel();

	std::string WideToUtf8(const std::wstring &value);
	std::wstring Utf8ToWide(const char *value);

	void LogDebug(const char *category, const char *message);
	void LogInfo(const char *category, const char *message);
	void LogWarn(const char *category, const char *message);
	void LogError(const char *category, const char *message);

	/** 指定レベル・カテゴリでフォーマットログを出力する */
	void LogDebugf(const char *category, const char *format, ...);
	void LogInfof(const char *category, const char *format, ...);
	void LogWarnf(const char *category, const char *format, ...);
	void LogErrorf(const char *category, const char *format, ...);

	void LogStartupStep(const char *message);
	void LogWorldIO(const char *message);
	void LogWorldName(const char *prefix, const std::wstring &name);
}
