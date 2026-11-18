// Progress Journal - part of Snapshot since 1.1.0.
//
// A crash-safe running record of the player's progress, written to disk line by line as things happen (each
// line is flushed at once, so a crash cannot lose it). Every save and every load is a checkpoint in the
// journal. When a save is loaded and the journal holds progress made after that save that was never saved
// (the game crashed, or you quit without saving), a message box says so and F1 > Snapshot > Progress Journal
// offers to put it back:
//   - items and gold gained or lost         (added / removed again)
//   - quest stages reached                  (replayed one by one, in the order they happened, through the quest's own
//                                             SetStage, so every stage's script runs: quests it starts, items it gives,
//                                             world changes it makes. Stages already done are skipped.)
//   - skills, skill XP, level, level XP, perk points, health/magicka/stamina   (raised to the recorded values)
//   - perks taken, spells learned           (added again)
//   - where you stood, and where each follower stood   (moved back there)
// Shown for information only (not restored): enemies killed, books read, locations discovered.
//
// Journal file: Documents\My Games\Skyrim Special Edition\SKSE\Snapshot\ProgressJournal.txt (tab separated,
// one event per line). Settings: [Journal] in Data\SKSE\Plugins\Snapshot.ini.

#include "Journal.h"

#include <Windows.h>

#include <filesystem>
#include <map>
#include <set>
#include <sstream>

#include "SKSEMenuFramework.h"

namespace fs = std::filesystem;

namespace Journal
{
	namespace
	{
		// ---------------------------------------------------------------- settings
		bool  g_enabled = true;
		float g_posSeconds = 5.0f;
		float g_posMinMove = 256.0f;

		// ---------------------------------------------------------------- state
		std::mutex        g_fileLock;
		std::ofstream     g_out;
		fs::path          g_path;
		std::atomic<bool> g_inGame{ false };
		std::string       g_loadingSave;  // name from kPreLoadGame

		std::set<RE::FormID> g_knownPerks;   // baseline for perk polling (main thread only)
		std::set<RE::FormID> g_knownSpells;  // baseline for spell polling (main thread only; this CommonLib's SpellsLearned event carries no spell)
		std::string          g_lastSkills;  // last SKILLS line written (main thread only)
		struct LastPos
		{
			RE::FormID cell = 0;
			float      x = 0, y = 0, z = 0;
		};
		std::map<RE::FormID, LastPos> g_lastPos;  // player and followers (main thread only)

		// ---------------------------------------------------------------- helpers
		std::string Clean(std::string a_text)
		{
			for (auto& c : a_text) {
				if (c == '\t' || c == '\n' || c == '\r') {
					c = ' ';
				}
			}
			return a_text;
		}

		std::string NameOf(const RE::TESForm* a_form)
		{
			if (!a_form) {
				return "";
			}
			const char* n = a_form->GetName();
			return Clean(n ? n : "");
		}

		// stable reference to a form: "Plugin.esp|00ABCD" (local id), or "#FF001234" for forms made in game
		std::string Ref(const RE::TESForm* a_form)
		{
			if (!a_form) {
				return "-";
			}
			if (!a_form->IsDynamicForm()) {
				if (const auto file = a_form->GetFile(0)) {
					return std::format("{}|{:06X}", file->GetFilename(), a_form->GetLocalFormID());
				}
			}
			return std::format("#{:08X}", a_form->GetFormID());
		}

		RE::TESForm* Resolve(const std::string& a_ref)
		{
			if (a_ref.empty() || a_ref == "-") {
				return nullptr;
			}
			if (a_ref[0] == '#') {
				return RE::TESForm::LookupByID(static_cast<RE::FormID>(std::strtoul(a_ref.c_str() + 1, nullptr, 16)));
			}
			const auto bar = a_ref.rfind('|');
			if (bar == std::string::npos) {
				return nullptr;
			}
			const auto dh = RE::TESDataHandler::GetSingleton();
			const auto local = static_cast<RE::FormID>(std::strtoul(a_ref.c_str() + bar + 1, nullptr, 16));
			return dh ? dh->LookupForm(local, a_ref.substr(0, bar)) : nullptr;
		}

		std::string SaveName(const void* a_data, std::uint32_t a_len)
		{
			if (!a_data || a_len == 0) {
				return {};
			}
			std::string s(static_cast<const char*>(a_data), strnlen(static_cast<const char*>(a_data), a_len));
			if (const auto slash = s.find_last_of("\\/"); slash != std::string::npos) {
				s.erase(0, slash + 1);
			}
			if (s.size() > 4 && _stricmp(s.c_str() + s.size() - 4, ".ess") == 0) {
				s.resize(s.size() - 4);
			}
			return Clean(s);
		}

		std::string Clock()
		{
			SYSTEMTIME t{};
			GetLocalTime(&t);
			return std::format("{:04}-{:02}-{:02} {:02}:{:02}:{:02}", t.wYear, t.wMonth, t.wDay, t.wHour, t.wMinute, t.wSecond);
		}

		void Write(const std::string& a_type, const std::string& a_fields = {})
		{
			std::scoped_lock l{ g_fileLock };
			if (!g_out.is_open()) {
				return;
			}
			g_out << Clock() << '\t' << a_type;
			if (!a_fields.empty()) {
				g_out << '\t' << a_fields;
			}
			g_out << '\n';
			g_out.flush();
		}

		void OpenFile()
		{
			const auto dir = SKSE::log::log_directory();
			if (!dir) {
				return;
			}
			std::error_code ec;
			fs::create_directories(*dir / "Snapshot", ec);
			g_path = *dir / "Snapshot" / "ProgressJournal.txt";
			if (fs::exists(g_path, ec) && fs::file_size(g_path, ec) > 64ull * 1024 * 1024) {
				fs::rename(g_path, *dir / "Snapshot" / "ProgressJournal.old.txt", ec);
			}
			std::scoped_lock l{ g_fileLock };
			g_out.open(g_path, std::ios::out | std::ios::app | std::ios::binary);
			logger::info("Progress Journal: {}", g_out.is_open() ? g_path.string() : std::string("could not open the journal file"));
		}

		std::vector<std::string> Split(const std::string& a_line)
		{
			std::vector<std::string> out;
			std::size_t              start = 0;
			while (true) {
				const auto tab = a_line.find('\t', start);
				out.push_back(a_line.substr(start, tab == std::string::npos ? std::string::npos : tab - start));
				if (tab == std::string::npos) {
					break;
				}
				start = tab + 1;
			}
			return out;
		}

		// ---------------------------------------------------------------- player data
		std::vector<RE::Actor*> Followers()
		{
			std::vector<RE::Actor*> out;
			static const auto followerFaction = RE::TESForm::LookupByID<RE::TESFaction>(0x0005C84E);  // CurrentFollowerFaction
			const auto        player = RE::PlayerCharacter::GetSingleton();
			const auto        pl = RE::ProcessLists::GetSingleton();
			if (!pl || !player) {
				return out;
			}
			for (auto& h : pl->highActorHandles) {
				const auto a = h.get();
				if (!a || a.get() == player || a->IsDead(false) || a->IsSummoned()) {
					continue;
				}
				if (a->IsPlayerTeammate() || (followerFaction && a->IsInFaction(followerFaction))) {
					out.push_back(a.get());
				}
			}
			return out;
		}

		std::string SkillsLine()
		{
			const auto player = RE::PlayerCharacter::GetSingleton();
			const auto skills = player ? player->GetPlayerRuntimeData().skills : nullptr;
			const auto av = player ? player->AsActorValueOwner() : nullptr;
			if (!skills || !skills->data || !av) {
				return {};
			}
			const auto d = skills->data;
			std::string s = std::format("{}\t{:.3f}\t{:.3f}\t{}\t{:.2f}\t{:.2f}\t{:.2f}", player->GetLevel(), d->xp, d->levelThreshold,
				static_cast<int>(player->GetGameStatsData().perkCount), av->GetBaseActorValue(RE::ActorValue::kHealth),
				av->GetBaseActorValue(RE::ActorValue::kMagicka), av->GetBaseActorValue(RE::ActorValue::kStamina));
			for (std::uint32_t i = 0; i < RE::PlayerCharacter::PlayerSkills::Data::Skill::kTotal; ++i) {
				const auto value = static_cast<RE::ActorValue>(static_cast<std::uint32_t>(RE::ActorValue::kOneHanded) + i);
				s += std::format("\t{:.2f}:{:.3f}:{:.3f}", av->GetBaseActorValue(value), d->skills[i].xp, d->skills[i].levelThreshold);
			}
			return s;
		}

		std::set<RE::FormID> CurrentPerks()
		{
			std::set<RE::FormID> out;
			const auto           player = RE::PlayerCharacter::GetSingleton();
			const auto           dh = RE::TESDataHandler::GetSingleton();
			if (!player || !dh) {
				return out;
			}
			for (auto perk : dh->GetFormArray<RE::BGSPerk>()) {
				if (perk && player->HasPerk(perk)) {
					out.insert(perk->GetFormID());
				}
			}
			return out;
		}

		std::set<RE::FormID> CurrentSpells()
		{
			std::set<RE::FormID> out;
			if (const auto player = RE::PlayerCharacter::GetSingleton()) {
				for (auto spell : player->GetActorRuntimeData().addedSpells) {
					if (spell) {
						out.insert(spell->GetFormID());
					}
				}
			}
			return out;
		}

		void RecordPosition(RE::Actor* a_actor, bool a_force)
		{
			const auto cell = a_actor->GetParentCell();
			if (!cell) {
				return;
			}
			const auto p = a_actor->GetPosition();
			auto&      last = g_lastPos[a_actor->GetFormID()];
			const float dx = p.x - last.x, dy = p.y - last.y, dz = p.z - last.z;
			if (!a_force && last.cell == cell->GetFormID() && dx * dx + dy * dy + dz * dz < g_posMinMove * g_posMinMove) {
				return;
			}
			last = { cell->GetFormID(), p.x, p.y, p.z };
			const bool isPlayer = a_actor == RE::PlayerCharacter::GetSingleton();
			Write("POS", std::format("{}\t{}\t{}\t{}\t{:.1f}\t{:.1f}\t{:.1f}\t{:.4f}", isPlayer ? std::string("PLAYER") : Ref(a_actor), NameOf(a_actor), Ref(cell),
							 Ref(a_actor->GetWorldspace()), p.x, p.y, p.z, a_actor->GetAngleZ()));
		}

		// main thread, every few seconds while in game
		int g_tick = 0;
		void Poll()
		{
			if (!g_inGame) {
				return;
			}
			const auto ui = RE::UI::GetSingleton();
			if (ui && ui->IsMenuOpen(RE::LoadingMenu::MENU_NAME)) {
				return;
			}
			const auto player = RE::PlayerCharacter::GetSingleton();
			if (!player) {
				return;
			}
			RecordPosition(player, false);
			for (auto f : Followers()) {
				RecordPosition(f, false);
			}
			if (++g_tick % 3 == 0) {
				if (auto line = SkillsLine(); !line.empty() && line != g_lastSkills) {
					g_lastSkills = line;
					Write("SKILLS", line);
				}
				auto now = CurrentPerks();
				for (const auto id : now) {
					if (!g_knownPerks.contains(id)) {
						const auto perk = RE::TESForm::LookupByID(id);
						Write("PERK", std::format("{}\t{}", Ref(perk), NameOf(perk)));
					}
				}
				g_knownPerks = std::move(now);
				auto spells = CurrentSpells();
				for (const auto id : spells) {
					if (!g_knownSpells.contains(id)) {
						const auto spell = RE::TESForm::LookupByID(id);
						Write("SPELL", std::format("{}\t{}", Ref(spell), NameOf(spell)));
					}
				}
				g_knownSpells = std::move(spells);
			}
		}

		void StartTimer()
		{
			static std::atomic<bool> started{ false };
			if (started.exchange(true)) {
				return;
			}
			std::thread([]() {
				while (true) {
					std::this_thread::sleep_for(std::chrono::milliseconds(static_cast<int>(g_posSeconds * 1000.0f)));
					if (g_inGame) {
						if (const auto tasks = SKSE::GetTaskInterface()) {
							tasks->AddTask([]() { Poll(); });
						}
					}
				}
			}).detach();
		}

		// ---------------------------------------------------------------- events
		class Sinks :
			public RE::BSTEventSink<RE::TESContainerChangedEvent>,
			public RE::BSTEventSink<RE::TESQuestStageEvent>,
			public RE::BSTEventSink<RE::TESDeathEvent>,
			public RE::BSTEventSink<RE::BooksRead::Event>,
			public RE::BSTEventSink<RE::LocationDiscovery::Event>
		{
		public:
			RE::BSEventNotifyControl ProcessEvent(const RE::TESContainerChangedEvent* a_event, RE::BSTEventSource<RE::TESContainerChangedEvent>*) override
			{
				if (g_inGame && a_event && a_event->itemCount > 0 && a_event->oldContainer != a_event->newContainer &&
					(a_event->newContainer == 0x14 || a_event->oldContainer == 0x14) && a_event->baseObj < 0xFF000000) {
					const auto item = RE::TESForm::LookupByID(a_event->baseObj);
					const int  delta = a_event->newContainer == 0x14 ? a_event->itemCount : -a_event->itemCount;
					// "given": appeared in the inventory from nowhere (not picked up, not from a container) - a script gave it
					const char* source = a_event->newContainer != 0x14 ? "lost" : a_event->oldContainer != 0 ? "container" : a_event->reference ? "world" : "given";
					Write("ITEM", std::format("{}\t{}\t{}\t{}", Ref(item), delta, NameOf(item), source));
				}
				return RE::BSEventNotifyControl::kContinue;
			}

			RE::BSEventNotifyControl ProcessEvent(const RE::TESQuestStageEvent* a_event, RE::BSTEventSource<RE::TESQuestStageEvent>*) override
			{
				if (g_inGame && a_event) {
					const auto quest = RE::TESForm::LookupByID<RE::TESQuest>(a_event->formID);
					if (quest) {
						Write("STAGE", std::format("{}\t{}\t{}", Ref(quest), a_event->stage, NameOf(quest)));
					}
				}
				return RE::BSEventNotifyControl::kContinue;
			}

			RE::BSEventNotifyControl ProcessEvent(const RE::TESDeathEvent* a_event, RE::BSTEventSource<RE::TESDeathEvent>*) override
			{
				if (g_inGame && a_event && a_event->dead && a_event->actorDying && a_event->actorKiller && a_event->actorKiller.get() == RE::PlayerCharacter::GetSingleton()) {
					Write("KILL", std::format("{}\t{}", Ref(a_event->actorDying.get()), NameOf(a_event->actorDying.get())));
				}
				return RE::BSEventNotifyControl::kContinue;
			}

			RE::BSEventNotifyControl ProcessEvent(const RE::BooksRead::Event* a_event, RE::BSTEventSource<RE::BooksRead::Event>*) override
			{
				if (g_inGame && a_event && a_event->book) {
					Write("BOOK", std::format("{}\t{}", Ref(a_event->book), NameOf(a_event->book)));
				}
				return RE::BSEventNotifyControl::kContinue;
			}

			RE::BSEventNotifyControl ProcessEvent(const RE::LocationDiscovery::Event* a_event, RE::BSTEventSource<RE::LocationDiscovery::Event>*) override
			{
				if (g_inGame && a_event && a_event->mapMarkerData) {
					const char* name = a_event->mapMarkerData->locationName.GetFullName();
					Write("DISCOVER", Clean(name ? name : ""));
				}
				return RE::BSEventNotifyControl::kContinue;
			}
		};

		Sinks g_sinks;

		void RegisterEvents()
		{
			if (const auto holder = RE::ScriptEventSourceHolder::GetSingleton()) {
				holder->AddEventSink<RE::TESContainerChangedEvent>(&g_sinks);
				holder->AddEventSink<RE::TESQuestStageEvent>(&g_sinks);
				holder->AddEventSink<RE::TESDeathEvent>(&g_sinks);
			}
			if (const auto src = RE::BooksRead::GetEventSource()) {
				src->AddEventSink(&g_sinks);
			}
			if (const auto src = RE::LocationDiscovery::GetEventSource()) {
				src->AddEventSink(&g_sinks);
			}
			logger::info("Progress Journal: listening (items, quest stages, kills, spells, books, discoveries; positions every {} s)", g_posSeconds);
		}

		// ---------------------------------------------------------------- what can be restored
		struct PosEntry
		{
			std::string who, name, cell, ws;
			float       x = 0, y = 0, z = 0, rot = 0;
		};

		struct Pending
		{
			std::string                                   save;
			std::size_t                                   lines = 0;
			std::map<std::string, std::pair<int, std::string>> items;  // ref -> (net count, name)
			std::vector<std::pair<std::string, int>>      stageOrder;  // quest ref, highest stage (first-seen order) - for the menu
			std::vector<std::pair<std::string, int>>      stageEvents; // every stage reached, in the order it happened - replayed
			std::map<std::string, std::pair<int, std::string>> stageItems;  // items a script gave right after a stage (come back with the replay)
			std::map<std::string, std::string>            stageNames;
			std::vector<std::string>                      skills;      // last SKILLS fields (after the type)
			std::map<std::string, std::string>            perks, spells;
			std::vector<std::string>                      kills, books, discoveries;
			bool                                          hasPlayerPos = false;
			PosEntry                                      playerPos;
			std::map<std::string, PosEntry>               followerPos;
			[[nodiscard]] bool Empty() const
			{
				return items.empty() && stageOrder.empty() && skills.empty() && perks.empty() && spells.empty() && !hasPlayerPos && followerPos.empty() &&
				       kills.empty() && books.empty() && discoveries.empty();
			}
		};

		std::mutex g_pendingLock;
		Pending    g_pending;
		bool       g_hasPending = false;
		std::string g_lastResult;
		std::string g_replayStatus;  // quest stage replay progress (under g_pendingLock)
		bool g_doItems = true, g_doStages = true, g_doSkills = true, g_doPerks = true, g_doSpells = true, g_doPos = true, g_doFollowers = true;

		// Reads the journal and collects what happened after the checkpoint for a_save (its last SAVE or LOAD line)
		// up to the next LOAD / NEWGAME. Nothing is offered if the player saved again in that stretch (a newer save
		// exists, so loading this one was a deliberate step back).
		Pending Collect(const std::string& a_save)
		{
			Pending p;
			p.save = a_save;
			std::ifstream in(g_path, std::ios::binary);
			if (!in) {
				return p;
			}
			std::vector<std::string> lines;
			for (std::string line; std::getline(in, line);) {
				if (!line.empty() && line.back() == '\r') {
					line.pop_back();
				}
				lines.push_back(std::move(line));
			}
			std::ptrdiff_t checkpoint = -1;
			for (std::ptrdiff_t i = static_cast<std::ptrdiff_t>(lines.size()) - 1; i >= 0; --i) {
				const auto f = Split(lines[i]);
				if (f.size() >= 3 && (f[1] == "SAVE" || f[1] == "LOAD") && f[2] == a_save) {
					checkpoint = i;
					break;
				}
			}
			if (checkpoint < 0) {
				return p;
			}
			auto seconds = [](const std::string& a_clock) -> long long {
				int y = 0, mo = 0, d = 0, h = 0, mi = 0, s = 0;
				if (std::sscanf(a_clock.c_str(), "%d-%d-%d %d:%d:%d", &y, &mo, &d, &h, &mi, &s) != 6) {
					return 0;
				}
				return ((static_cast<long long>(y) * 372 + mo * 31 + d) * 86400LL) + h * 3600LL + mi * 60LL + s;
			};
			long long lastStage = -1000000;
			for (std::size_t i = static_cast<std::size_t>(checkpoint) + 1; i < lines.size(); ++i) {
				const auto f = Split(lines[i]);
				if (f.size() < 2) {
					continue;
				}
				const auto& type = f[1];
				if (type == "LOAD" || type == "NEWGAME") {
					break;
				}
				if (type == "SAVE") {
					return Pending{ a_save };  // a newer save holds this progress
				}
				++p.lines;
				if (type == "ITEM" && f.size() >= 5) {
					const int delta = std::atoi(f[3].c_str());
					// given by a script within 5 s of a stage: that stage's replay gives it again, so it is kept apart
					const bool fromStage = f.size() >= 6 && f[5] == "given" && delta > 0 && seconds(f[0]) - lastStage <= 5;
					auto&      e = fromStage ? p.stageItems[f[2]] : p.items[f[2]];
					e.first += delta;
					e.second = f[4];
				} else if (type == "STAGE" && f.size() >= 5) {
					const int stage = std::atoi(f[3].c_str());
					auto      it = std::find_if(p.stageOrder.begin(), p.stageOrder.end(), [&](auto& e) { return e.first == f[2]; });
					if (it == p.stageOrder.end()) {
						p.stageOrder.emplace_back(f[2], stage);
					} else {
						it->second = std::max(it->second, stage);
					}
					p.stageNames[f[2]] = f[4];
					p.stageEvents.emplace_back(f[2], stage);
					lastStage = seconds(f[0]);
				} else if (type == "SKILLS") {
					p.skills.assign(f.begin() + 2, f.end());
				} else if (type == "PERK" && f.size() >= 4) {
					p.perks[f[2]] = f[3];
				} else if (type == "SPELL" && f.size() >= 4) {
					p.spells[f[2]] = f[3];
				} else if (type == "KILL" && f.size() >= 4) {
					p.kills.push_back(f[3]);
				} else if (type == "BOOK" && f.size() >= 4) {
					p.books.push_back(f[3]);
				} else if (type == "DISCOVER" && f.size() >= 3) {
					p.discoveries.push_back(f[2]);
				} else if (type == "POS" && f.size() >= 10) {
					PosEntry e{ f[2], f[3], f[4], f[5], std::strtof(f[6].c_str(), nullptr), std::strtof(f[7].c_str(), nullptr), std::strtof(f[8].c_str(), nullptr),
						std::strtof(f[9].c_str(), nullptr) };
					if (e.who == "PLAYER") {
						p.playerPos = e;
						p.hasPlayerPos = true;
					} else {
						p.followerPos[e.who] = e;
					}
				}
			}
			// items whose gains and losses cancel out are nothing to restore
			for (auto it = p.items.begin(); it != p.items.end();) {
				it = it->second.first == 0 ? p.items.erase(it) : std::next(it);
			}
			return p;
		}

		// ---------------------------------------------------------------- quest stage replay, one stage at a time
		struct StageRun
		{
			std::vector<std::pair<std::string, int>> events;
			std::size_t                              next = 0;
			int                                      run = 0, skipped = 0;
		};

		void RunNextStage(std::shared_ptr<StageRun> a_run);

		// called by Papyrus when SetStage returned; the next stage waits a moment so this stage's script gets to run first
		class NextStage : public RE::BSScript::IStackCallbackFunctor
		{
		public:
			explicit NextStage(std::shared_ptr<StageRun> a_run) :
				_run(std::move(a_run)) {}
			void operator()(RE::BSScript::Variable) override
			{
				auto run = _run;
				std::thread([run]() {
					std::this_thread::sleep_for(std::chrono::milliseconds(750));
					if (const auto tasks = SKSE::GetTaskInterface()) {
						tasks->AddTask([run]() { RunNextStage(run); });
					}
				}).detach();
			}
			void SetObject(const RE::BSTSmartPointer<RE::BSScript::Object>&) override {}

		private:
			std::shared_ptr<StageRun> _run;
		};

		bool StageDone(RE::TESQuest* a_quest, int a_stage)
		{
			if (a_quest->GetCurrentStageID() == a_stage) {
				return true;
			}
			if (a_quest->executedStages) {
				for (auto& s : *a_quest->executedStages) {
					if (s.data.index == a_stage) {
						return true;
					}
				}
			}
			return false;
		}

		// main thread
		void RunNextStage(std::shared_ptr<StageRun> a_run)
		{
			const auto vm = RE::BSScript::Internal::VirtualMachine::GetSingleton();
			const auto policy = vm ? vm->GetObjectHandlePolicy() : nullptr;
			while (a_run->next < a_run->events.size()) {
				const auto [ref, stage] = a_run->events[a_run->next++];
				const auto form = Resolve(ref);
				const auto quest = form ? form->As<RE::TESQuest>() : nullptr;
				if (!quest || !policy || StageDone(quest, stage)) {
					++a_run->skipped;
					continue;
				}
				const auto handle = policy->GetHandleForObject(RE::TESQuest::FORMTYPE, quest);
				RE::BSTSmartPointer<RE::BSScript::IStackCallbackFunctor> callback{ new NextStage(a_run) };
				std::unique_ptr<RE::BSScript::IFunctionArguments> args{ RE::MakeFunctionArguments(static_cast<std::int32_t>(stage)) };
				if (vm->DispatchMethodCall(handle, RE::BSFixedString{ "Quest" }, RE::BSFixedString{ "SetStage" }, args.get(), callback)) {
					++a_run->run;
					{
						std::scoped_lock l{ g_pendingLock };
						g_replayStatus = std::format("Quest replay: stage {} of {} ({} stage {})", a_run->next, a_run->events.size(), NameOf(quest), stage);
					}
					logger::info("Progress Journal: replaying {} stage {}", NameOf(quest), stage);
					return;  // NextStage continues the chain
				}
				++a_run->skipped;
			}
			const auto done = std::format("Quest replay finished: {} stages run, {} already done or not found", a_run->run, a_run->skipped);
			Write("REPLAYED", done);
			logger::info("Progress Journal: {}", done);
			std::scoped_lock l{ g_pendingLock };
			g_replayStatus = done;
		}

		void MoveActor(RE::Actor* a_actor, const PosEntry& a_pos)
		{
			const auto cell = Resolve(a_pos.cell);
			const auto ws = Resolve(a_pos.ws);
			const auto c = cell ? cell->As<RE::TESObjectCELL>() : nullptr;
			if (!a_actor || !c) {
				return;
			}
			// TESObjectREFR::MoveTo_Impl (private in this CommonLib): SE 56227 / AE 56626
			using MoveTo_t = void (*)(RE::TESObjectREFR*, const RE::ObjectRefHandle&, RE::TESObjectCELL*, RE::TESWorldSpace*, const RE::NiPoint3&, const RE::NiPoint3&);
			static REL::Relocation<MoveTo_t> moveTo{ RELOCATION_ID(56227, 56626) };
			const RE::ObjectRefHandle none{};
			const RE::NiPoint3        pos(a_pos.x, a_pos.y, a_pos.z);
			const RE::NiPoint3        rot(0.0f, 0.0f, a_pos.rot);
			moveTo(a_actor, none, c, ws ? ws->As<RE::TESWorldSpace>() : nullptr, pos, rot);
		}

		// main thread
		std::string Restore(const Pending& p)
		{
			const auto player = RE::PlayerCharacter::GetSingleton();
			if (!player) {
				return "no player";
			}
			int items = 0, stages = 0, perks = 0, spells = 0, moved = 0;
			bool skills = false;

			if (g_doItems) {
				auto all = p.items;
				if (!g_doStages) {  // the stage replay would have given these; without it they are added like any item
					for (auto& [ref, e] : p.stageItems) {
						all[ref].first += e.first;
						all[ref].second = e.second;
					}
				}
				for (auto& [ref, e] : all) {
					const auto form = Resolve(ref);
					const auto obj = form ? form->As<RE::TESBoundObject>() : nullptr;
					if (!obj) {
						continue;
					}
					if (e.first > 0) {
						player->AddObjectToContainer(obj, nullptr, e.first, nullptr);
					} else {
						player->RemoveItem(obj, -e.first, RE::ITEM_REMOVE_REASON::kRemove, nullptr, nullptr);
					}
					++items;
				}
			}
			if (g_doSkills && p.skills.size() >= 7 + RE::PlayerCharacter::PlayerSkills::Data::Skill::kTotal) {
				const auto ps = player->GetPlayerRuntimeData().skills;
				const auto av = player->AsActorValueOwner();
				const auto base = player->GetActorBase();
				if (ps && ps->data && av && base) {
					const int level = std::atoi(p.skills[0].c_str());
					if (level > player->GetLevel()) {
						base->actorData.level = static_cast<std::uint16_t>(level);
						ps->data->xp = std::strtof(p.skills[1].c_str(), nullptr);
						ps->data->levelThreshold = std::strtof(p.skills[2].c_str(), nullptr);
						skills = true;
					} else if (level == player->GetLevel() && std::strtof(p.skills[1].c_str(), nullptr) > ps->data->xp) {
						ps->data->xp = std::strtof(p.skills[1].c_str(), nullptr);
						skills = true;
					}
					const int points = std::atoi(p.skills[3].c_str());
					if (points > static_cast<int>(player->GetGameStatsData().perkCount)) {
						player->GetGameStatsData().perkCount = static_cast<std::uint8_t>(points);
						skills = true;
					}
					const RE::ActorValue attrs[3] = { RE::ActorValue::kHealth, RE::ActorValue::kMagicka, RE::ActorValue::kStamina };
					for (int i = 0; i < 3; ++i) {
						const float v = std::strtof(p.skills[4 + i].c_str(), nullptr);
						if (v > av->GetBaseActorValue(attrs[i])) {
							av->SetBaseActorValue(attrs[i], v);
							skills = true;
						}
					}
					for (std::uint32_t i = 0; i < RE::PlayerCharacter::PlayerSkills::Data::Skill::kTotal; ++i) {
						float lvl = 0, xp = 0, thr = 0;
						if (std::sscanf(p.skills[7 + i].c_str(), "%f:%f:%f", &lvl, &xp, &thr) != 3) {
							continue;
						}
						const auto value = static_cast<RE::ActorValue>(static_cast<std::uint32_t>(RE::ActorValue::kOneHanded) + i);
						const float cur = av->GetBaseActorValue(value);
						if (lvl > cur || (lvl == cur && xp > ps->data->skills[i].xp)) {
							av->SetBaseActorValue(value, lvl);
							ps->data->skills[i].xp = xp;
							ps->data->skills[i].levelThreshold = thr;
							skills = true;
						}
					}
				}
			}
			if (g_doPerks) {
				for (auto& [ref, name] : p.perks) {
					const auto form = Resolve(ref);
					const auto perk = form ? form->As<RE::BGSPerk>() : nullptr;
					if (perk && !player->HasPerk(perk)) {
						player->AddPerk(perk);
						++perks;
					}
				}
			}
			if (g_doSpells) {
				for (auto& [ref, name] : p.spells) {
					const auto form = Resolve(ref);
					const auto spell = form ? form->As<RE::SpellItem>() : nullptr;
					if (spell && !player->HasSpell(spell)) {
						player->AddSpell(spell);
						++spells;
					}
				}
			}
			if (g_doStages && !p.stageEvents.empty()) {
				auto run = std::make_shared<StageRun>();
				run->events = p.stageEvents;
				RunNextStage(run);  // one by one, each after the previous SetStage returned
				stages = static_cast<int>(p.stageEvents.size());
			}
			if (g_doFollowers) {
				for (auto& [who, pos] : p.followerPos) {
					const auto form = Resolve(who);
					if (const auto actor = form ? form->As<RE::Actor>() : nullptr; actor && !actor->IsDead(false)) {
						MoveActor(actor, pos);
						++moved;
					}
				}
			}
			if (g_doPos && p.hasPlayerPos) {
				MoveActor(player, p.playerPos);
				++moved;
			}
			const auto result = std::format("Restored: {} items, {} quest stages queued for replay (one by one), skills {}, {} perks, {} spells, {} moved (player/followers)", items, stages,
				skills ? "raised" : "unchanged", perks, spells, moved);
			Write("RESTORED", result);
			logger::info("Progress Journal: {}", result);
			return result;
		}

		// ---------------------------------------------------------------- F1 menu
		void ShowList(const char* a_title, const std::vector<std::string>& a_lines)
		{
			ImGuiMCP::TextUnformatted(a_title);
			const std::size_t shown = std::min<std::size_t>(a_lines.size(), 25);
			for (std::size_t i = 0; i < shown; ++i) {
				ImGuiMCP::TextUnformatted(("      " + a_lines[i]).c_str());
			}
			if (a_lines.size() > shown) {
				ImGuiMCP::TextUnformatted(std::format("      ... and {} more", a_lines.size() - shown).c_str());
			}
		}

		void __stdcall RenderMenu()
		{
			std::scoped_lock l{ g_pendingLock };
			ImGuiMCP::TextUnformatted("Progress Journal - progress made after a save that was never saved (crash or quit).");
			ImGuiMCP::TextUnformatted(("Journal file: " + g_path.string()).c_str());
			ImGuiMCP::Separator();
			if (!g_lastResult.empty()) {
				ImGuiMCP::TextUnformatted(g_lastResult.c_str());
				if (!g_replayStatus.empty()) {
					ImGuiMCP::TextUnformatted(g_replayStatus.c_str());
				}
				ImGuiMCP::Separator();
			}
			if (!g_hasPending) {
				ImGuiMCP::TextUnformatted("Nothing to restore for the save that is loaded.");
				return;
			}
			const auto& p = g_pending;
			ImGuiMCP::TextUnformatted(std::format("Save \"{}\": {} journal entries after it were never saved.", p.save, p.lines).c_str());
			ImGuiMCP::Separator();

			std::vector<std::string> list;
			for (auto& [ref, e] : p.items) {
				list.push_back(std::format("{:+} {}", e.first, e.second));
			}
			ImGuiMCP::Checkbox(std::format("Items and gold ({})", p.items.size()).c_str(), &g_doItems);
			ShowList("", list);

			list.clear();
			for (auto& [ref, stage] : p.stageEvents) {
				list.push_back(std::format("{} - stage {}", p.stageNames.at(ref), stage));
			}
			ImGuiMCP::Checkbox(std::format("Quest stages ({} stages in {} quests, replayed one by one in this order)", p.stageEvents.size(), p.stageOrder.size()).c_str(), &g_doStages);
			ShowList("", list);
			if (!p.stageItems.empty()) {
				list.clear();
				for (auto& [ref, e] : p.stageItems) {
					list.push_back(std::format("{:+} {}", e.first, e.second));
				}
				ShowList("      Given by those stages' scripts (the replay gives them again; added as items only if quest stages are not ticked):", list);
			}

			ImGuiMCP::Checkbox(p.skills.empty() ? "Skills and level (no change recorded)" : std::format("Skills and level (level {} recorded)", p.skills[0]).c_str(), &g_doSkills);

			list.clear();
			for (auto& [ref, name] : p.perks) {
				list.push_back(name);
			}
			ImGuiMCP::Checkbox(std::format("Perks ({})", p.perks.size()).c_str(), &g_doPerks);
			ShowList("", list);

			list.clear();
			for (auto& [ref, name] : p.spells) {
				list.push_back(name);
			}
			ImGuiMCP::Checkbox(std::format("Spells ({})", p.spells.size()).c_str(), &g_doSpells);
			ShowList("", list);

			ImGuiMCP::Checkbox(p.hasPlayerPos ? std::format("Move me back to where I was (x {:.0f} y {:.0f})", p.playerPos.x, p.playerPos.y).c_str() : "Move me back (no position recorded)",
				&g_doPos);
			list.clear();
			for (auto& [who, pos] : p.followerPos) {
				list.push_back(std::format("{} at x {:.0f} y {:.0f} z {:.0f}", pos.name, pos.x, pos.y, pos.z));
			}
			ImGuiMCP::Checkbox(std::format("Move followers back to where they were ({})", p.followerPos.size()).c_str(), &g_doFollowers);
			ShowList("", list);

			ImGuiMCP::Separator();
			ShowList(std::format("For information (not restored) - killed by you: {}", p.kills.size()).c_str(), p.kills);
			ShowList(std::format("Books read: {}", p.books.size()).c_str(), p.books);
			ShowList(std::format("Locations discovered: {}", p.discoveries.size()).c_str(), p.discoveries);
			ImGuiMCP::Separator();

			if (ImGuiMCP::Button("Restore selected")) {
				auto copy = g_pending;
				g_hasPending = false;
				g_lastResult = "Restoring...";
				if (const auto tasks = SKSE::GetTaskInterface()) {
					tasks->AddTask([copy]() {
						const auto r = Restore(copy);
						std::scoped_lock l2{ g_pendingLock };
						g_lastResult = r;
					});
				}
			}
			ImGuiMCP::SameLine();
			if (ImGuiMCP::Button("Dismiss")) {
				g_hasPending = false;
				g_lastResult = "Dismissed - nothing restored.";
				Write("DISMISSED", p.save);
			}
		}

		// ---------------------------------------------------------------- load / save checkpoints
		void AfterLoad(bool a_newGame)
		{
			const auto player = RE::PlayerCharacter::GetSingleton();
			if (!a_newGame && !g_loadingSave.empty()) {
				auto p = Collect(g_loadingSave);
				std::scoped_lock l{ g_pendingLock };
				g_lastResult.clear();
				g_hasPending = !p.Empty();
				g_pending = std::move(p);
				if (g_hasPending) {
					logger::info("Progress Journal: {} entries after save \"{}\" were never saved - offered for restore", g_pending.lines, g_pending.save);
					const auto msg = std::format("Snapshot - Progress Journal\n\nThis save is behind: {} changes were made after it and never saved (a crash, or a quit without saving).\n\n"
					                             "Open F1 > Snapshot > Progress Journal to restore them.",
						g_pending.lines);
					if (const auto tasks = SKSE::GetTaskInterface()) {
						tasks->AddTask([msg]() { RE::DebugMessageBox(msg.c_str()); });
					}
				}
			}
			Write(a_newGame ? "NEWGAME" : "LOAD", a_newGame ? std::string() : g_loadingSave);
			// baselines (not progress): the state the save itself holds
			g_knownPerks = CurrentPerks();
			g_knownSpells = CurrentSpells();
			g_lastSkills = SkillsLine();
			if (!g_lastSkills.empty()) {
				Write("SKILLS", g_lastSkills);
			}
			g_lastPos.clear();
			if (player) {
				RecordPosition(player, true);
				for (auto f : Followers()) {
					RecordPosition(f, true);
				}
			}
			g_inGame = true;
		}
	}

	void LoadSettings(const std::string& a_ini)
	{
		char buf[32]{};
		GetPrivateProfileStringA("Journal", "bEnabled", "1", buf, sizeof(buf), a_ini.c_str());
		g_enabled = std::atoi(buf) != 0;
		GetPrivateProfileStringA("Journal", "fPositionSeconds", "5", buf, sizeof(buf), a_ini.c_str());
		g_posSeconds = std::max(1.0f, std::strtof(buf, nullptr));
		GetPrivateProfileStringA("Journal", "fPositionMinMove", "256", buf, sizeof(buf), a_ini.c_str());
		g_posMinMove = std::strtof(buf, nullptr);
		logger::info("Progress Journal settings: enabled {} position every {} s (min move {})", g_enabled, g_posSeconds, g_posMinMove);
	}

	void OnMessage(SKSE::MessagingInterface::Message* a_msg)
	{
		if (!g_enabled || !a_msg) {
			return;
		}
		switch (a_msg->type) {
		case SKSE::MessagingInterface::kDataLoaded:
			OpenFile();
			RegisterEvents();
			StartTimer();
			if (SKSEMenuFramework::IsInstalled()) {
				SKSEMenuFramework::SetSection("Snapshot");
				SKSEMenuFramework::AddSectionItem("Progress Journal", RenderMenu);
				logger::info("Progress Journal: F1 menu page added");
			} else {
				logger::warn("Progress Journal: SKSE Menu Framework not found - restore page unavailable (journal still written)");
			}
			break;
		case SKSE::MessagingInterface::kPreLoadGame:
			g_inGame = false;
			g_loadingSave = SaveName(a_msg->data, a_msg->dataLen);
			break;
		case SKSE::MessagingInterface::kPostLoadGame:
			if (a_msg->data) {  // load succeeded
				AfterLoad(false);
			}
			break;
		case SKSE::MessagingInterface::kNewGame:
			{
				std::scoped_lock l{ g_pendingLock };
				g_hasPending = false;
			}
			AfterLoad(true);
			break;
		case SKSE::MessagingInterface::kSaveGame:
			Write("SAVE", SaveName(a_msg->data, a_msg->dataLen));
			break;
		default:
			break;
		}
	}
}
