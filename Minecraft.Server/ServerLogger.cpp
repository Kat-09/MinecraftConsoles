#include "stdafx.h"

#include "ServerLogger.h"

#include <stdio.h>

namespace ServerRuntime
{
std::string WideToUtf8(const std::wstring &value)
{
	if (value.empty())
	{
		return std::string();
	}

	int charCount = WideCharToMultiByte(CP_UTF8, 0, value.c_str(), (int)value.length(), NULL, 0, NULL, NULL);
	if (charCount <= 0)
	{
		return std::string();
	}

	std::string utf8;
	utf8.resize(charCount);
	WideCharToMultiByte(CP_UTF8, 0, value.c_str(), (int)value.length(), &utf8[0], charCount, NULL, NULL);
	return utf8;
}

std::wstring Utf8ToWide(const char *value)
{
	if (value == NULL || value[0] == 0)
	{
		return std::wstring();
	}

	int wideCount = MultiByteToWideChar(CP_UTF8, 0, value, -1, NULL, 0);
	if (wideCount <= 0)
	{
		wideCount = MultiByteToWideChar(CP_ACP, 0, value, -1, NULL, 0);
		if (wideCount <= 0)
		{
			return std::wstring();
		}

		std::wstring wide;
		wide.resize(wideCount - 1);
		MultiByteToWideChar(CP_ACP, 0, value, -1, &wide[0], wideCount);
		return wide;
	}

	std::wstring wide;
	wide.resize(wideCount - 1);
	MultiByteToWideChar(CP_UTF8, 0, value, -1, &wide[0], wideCount);
	return wide;
}

void LogStartupStep(const char *message)
{
	printf("[startup] %s\n", message);
	fflush(stdout);
}

void LogWorldIO(const char *message)
{
	printf("[world-io] %s\n", message);
	fflush(stdout);
}

void LogWorldName(const char *prefix, const std::wstring &name)
{
	std::string utf8 = WideToUtf8(name);
	printf("[world-io] %s: %s\n", prefix, utf8.c_str());
	fflush(stdout);
}
}
