#pragma once

// Progress Journal (part of Snapshot since 1.1.0): a crash-safe running record of player progress.
namespace Journal
{
	void LoadSettings(const std::string& a_ini);
	void OnMessage(SKSE::MessagingInterface::Message* a_msg);
}
