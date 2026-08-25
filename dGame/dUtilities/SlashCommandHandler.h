/*
 * Darkflame Universe
 * Copyright 2018
 */

#ifndef SLASHCOMMANDHANDLER_H
#define SLASHCOMMANDHANDLER_H

#include "RakNetTypes.h"
#include "eGameMasterLevel.h"
#include <string>

class Entity;

struct Command {
	std::string help;
	std::string info;
	std::vector<std::string> aliases;
	std::function<void(Entity*, const SystemAddress&,const std::string)> handle;
	eGameMasterLevel requiredLevel = eGameMasterLevel::OPERATOR;
	// Optional dynamic owner. Core commands leave this empty; mod commands use
	// their stable mod id so all aliases can be removed before a Lua state dies.
	std::string owner;
};

namespace SlashCommandHandler {
	void HandleChatCommand(const std::u16string& command, Entity* entity, const SystemAddress& sysAddr);
	void SendAnnouncement(const std::string& title, const std::string& message);
	// Registration is atomic across all aliases: either every alias is available
	// and the command is published, or nothing is inserted.
	bool RegisterCommand(Command info);
	// Removes every command whose Command::owner exactly matches owner and returns
	// the number of primary commands removed.
	size_t UnregisterCommandsByOwner(const std::string& owner);
	void Startup();
};

namespace GMZeroCommands {
	void Help(Entity* entity, const SystemAddress& sysAddr, const std::string args);
}

#endif // SLASHCOMMANDHANDLER_H
