#pragma once

#include <string>

namespace ServerRuntime
{
	std::string WideToUtf8(const std::wstring &value);
	std::wstring Utf8ToWide(const char *value);

	void LogStartupStep(const char *message);
	void LogWorldIO(const char *message);
	void LogWorldName(const char *prefix, const std::wstring &name);
}
