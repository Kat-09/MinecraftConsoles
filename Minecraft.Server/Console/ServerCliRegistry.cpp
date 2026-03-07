#include "stdafx.h"

#include "ServerCliRegistry.h"

#include "commands\IServerCliCommand.h"

#include <ctype.h>

namespace ServerRuntime
{
	static bool StartsWithIgnoreCase(const std::string &value, const std::string &prefix)
	{
		if (prefix.size() > value.size())
		{
			return false;
		}
		for (size_t i = 0; i < prefix.size(); ++i)
		{
			char a = (char)tolower((unsigned char)value[i]);
			char b = (char)tolower((unsigned char)prefix[i]);
			if (a != b)
			{
				return false;
			}
		}
		return true;
	}

	std::string ServerCliRegistry::Normalize(const std::string &value)
	{
		std::string normalized = value;
		for (size_t i = 0; i < normalized.size(); ++i)
		{
			normalized[i] = (char)tolower((unsigned char)normalized[i]);
		}
		return normalized;
	}

	bool ServerCliRegistry::Register(std::unique_ptr<IServerCliCommand> command)
	{
		if (!command)
		{
			return false;
		}

		IServerCliCommand *raw = command.get();
		std::string baseName = Normalize(raw->Name());
		// Reject empty/duplicate primary command names.
		if (baseName.empty() || m_lookup.find(baseName) != m_lookup.end())
		{
			return false;
		}
		std::vector<std::string> aliases = raw->Aliases();
		std::vector<std::string> normalizedAliases;
		normalizedAliases.reserve(aliases.size());
		for (size_t i = 0; i < aliases.size(); ++i)
		{
			std::string alias = Normalize(aliases[i]);
			// Alias must also be unique across all names and aliases.
			if (alias.empty() || m_lookup.find(alias) != m_lookup.end())
			{
				return false;
			}
			normalizedAliases.push_back(alias);
		}

		m_lookup[baseName] = raw;
		for (size_t i = 0; i < normalizedAliases.size(); ++i)
		{
			m_lookup[normalizedAliases[i]] = raw;
		}

		// Command objects are owned here; lookup stores non-owning pointers.
		m_commands.push_back(std::move(command));
		return true;
	}

	const IServerCliCommand *ServerCliRegistry::Find(const std::string &name) const
	{
		std::string key = Normalize(name);
		auto it = m_lookup.find(key);
		if (it == m_lookup.end())
		{
			return NULL;
		}
		return it->second;
	}

	IServerCliCommand *ServerCliRegistry::FindMutable(const std::string &name)
	{
		std::string key = Normalize(name);
		auto it = m_lookup.find(key);
		if (it == m_lookup.end())
		{
			return NULL;
		}
		return it->second;
	}

	void ServerCliRegistry::SuggestCommandNames(const std::string &prefix, const std::string &linePrefix, std::vector<std::string> *out) const
	{
		for (size_t i = 0; i < m_commands.size(); ++i)
		{
			const IServerCliCommand *command = m_commands[i].get();
			std::string name = command->Name();
			if (StartsWithIgnoreCase(name, prefix))
			{
				out->push_back(linePrefix + name);
			}

			// Include aliases so users can discover shorthand commands.
			std::vector<std::string> aliases = command->Aliases();
			for (size_t aliasIndex = 0; aliasIndex < aliases.size(); ++aliasIndex)
			{
				if (StartsWithIgnoreCase(aliases[aliasIndex], prefix))
				{
					out->push_back(linePrefix + aliases[aliasIndex]);
				}
			}
		}
	}

	const std::vector<std::unique_ptr<IServerCliCommand>> &ServerCliRegistry::Commands() const
	{
		return m_commands;
	}
}
