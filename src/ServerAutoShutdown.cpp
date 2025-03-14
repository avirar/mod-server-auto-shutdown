/*
 * This file is part of the AzerothCore Project. See AUTHORS file for Copyright information
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms of the GNU Affero General Public License as published by the
 * Free Software Foundation; either version 3 of the License, or (at your
 * option) any later version.
 *
 * This program is distributed in the hope that it will be useful, but WITHOUT
 * ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or
 * FITNESS FOR A PARTICULAR PURPOSE. See the GNU Affero General Public License for
 * more details.
 *
 * You should have received a copy of the GNU General Public License along
 * with this program. If not, see <http://www.gnu.org/licenses/>.
 */

#include "ServerAutoShutdown.h"
#include "Config.h"
#include "Duration.h"
#include "GameEventMgr.h"
#include "Language.h"
#include "Log.h"
#include "ObjectMgr.h"
#include "StringConvert.h"
#include "StringFormat.h"
#include "TaskScheduler.h"
#include "Tokenize.h"
#include "Util.h"
#include "World.h"
#include "WorldSessionMgr.h"

namespace
{
    // Scheduler - for update
    TaskScheduler scheduler;

    time_t GetNextResetTime(time_t time, uint32 day, uint8 hour, uint8 minute, uint8 second)
    {
        tm timeLocal = Acore::Time::TimeBreakdown(time);
        timeLocal.tm_hour = hour;
        timeLocal.tm_min = minute;
        timeLocal.tm_sec = second;

        time_t midnightLocal = mktime(&timeLocal);

        if (day > 1 || midnightLocal <= time)
            midnightLocal += 86400 * day;

        return midnightLocal;
    }
}

/*static*/ ServerAutoShutdown* ServerAutoShutdown::instance()
{
    static ServerAutoShutdown instance;
    return &instance;
}

void ServerAutoShutdown::Init()
{
    _isEnableModule = sConfigMgr->GetOption<bool>("ServerAutoShutdown.Enabled", false);

    if (!_isEnableModule)
        return;

    std::string configTimes = sConfigMgr->GetOption<std::string>("ServerAutoShutdown.Time", "04:00:00");
    auto const& timeStrings = Acore::Tokenize(configTimes, ';', false);
    LOG_INFO("ServerAutoShutdown", "Loaded ServerAutoShutdown.Time: {}", configTimes);

    std::vector<std::tuple<uint8, uint8, uint8>> resetTimes;

    for (const auto& timeString : timeStrings)
    {
        auto const& tokens = Acore::Tokenize(timeString, ':', false);

        if (tokens.size() != 3)
        {
            LOG_ERROR("module", "> ServerAutoShutdown: Incorrect time format in config option 'ServerAutoShutdown.Time' - '{}'", timeString);
            continue;
        }

        // Check convert to int
        auto CheckTime = [tokens](std::initializer_list<uint8> index)
        {
            for (auto const& itr : index)
            {
                if (!Acore::StringTo<uint8>(tokens.at(itr)))
                    return false;
            }

            return true;
        };

        if (!CheckTime({ 0, 1, 2 }))
        {
            LOG_ERROR("module", "> ServerAutoShutdown: Incorrect time in config option 'ServerAutoShutdown.Time' - '{}'", timeString);
            continue;
        }

        uint8 hour = *Acore::StringTo<uint8>(tokens.at(0));
        uint8 minute = *Acore::StringTo<uint8>(tokens.at(1));
        uint8 second = *Acore::StringTo<uint8>(tokens.at(2));

        if (hour > 23 || minute >= 60 || second >= 60)
        {
            LOG_ERROR("module", "> ServerAutoShutdown: Incorrect time value in config option 'ServerAutoShutdown.Time' - '{}'", timeString);
            continue;
        }

        resetTimes.emplace_back(hour, minute, second);
    }

    if (resetTimes.empty())
    {
        LOG_ERROR("module", "> ServerAutoShutdown: No valid shutdown times provided in config.");
        _isEnableModule = false;
        return;
    }

    auto nowTime = time(nullptr);

    // Cancel all tasks for support reload config
    scheduler.CancelAll();
    sWorld->ShutdownCancel();

    for (const auto& [hour, minute, second] : resetTimes)
    {
        uint64 nextResetTime = GetNextResetTime(nowTime, 1, hour, minute, second); // default to daily reset
        uint32 diffToShutdown = nextResetTime - static_cast<uint32>(nowTime);

        if (diffToShutdown < 10)
        {
            LOG_WARN("module", "> ServerAutoShutdown: Next time to shutdown < 10 seconds, Skipping this time");
            continue;
        }

        LOG_INFO("module", " ");
        LOG_INFO("module", "> ServerAutoShutdown: Next time to shutdown - {}", Acore::Time::TimeToHumanReadable(Seconds(nextResetTime)));
        LOG_INFO("module", "> ServerAutoShutdown: Remaining time to shutdown - {}", Acore::Time::ToTimeString<Seconds>(diffToShutdown));
        LOG_INFO("module", " ");

        uint32 preAnnounceSeconds = sConfigMgr->GetOption<uint32>("ServerAutoShutdown.PreAnnounce.Seconds", 3600);
        if (preAnnounceSeconds > 86400)
        {
            LOG_ERROR("module", "> ServerAutoShutdown: Time to preannounce exceeds 1 day? ({}). Changing to 1 hour (3600)", preAnnounceSeconds);
            preAnnounceSeconds = 3600;
        }

        uint32 timeToPreAnnounce = static_cast<uint32>(nextResetTime) - preAnnounceSeconds;
        uint32 diffToPreAnnounce = timeToPreAnnounce - static_cast<uint32>(nowTime);

        // Ignore pre-announce time and set it to 1 second before shutdown if less than preAnnounceSeconds
        if (diffToShutdown < preAnnounceSeconds)
        {
            timeToPreAnnounce = static_cast<uint32>(nowTime) + 1;
            diffToPreAnnounce = 1;
            preAnnounceSeconds = diffToShutdown;
        }

        LOG_INFO("module", "> ServerAutoShutdown: Next time to pre-announce - {}", Acore::Time::TimeToHumanReadable(Seconds(timeToPreAnnounce)));
        LOG_INFO("module", "> ServerAutoShutdown: Remaining time to pre-announce - {}", Acore::Time::ToTimeString<Seconds>(diffToPreAnnounce));
        LOG_INFO("module", " ");

        // Add task for pre-shutdown announce
        scheduler.Schedule(Seconds(diffToPreAnnounce), [preAnnounceSeconds](TaskContext /*context*/)
        {
            // Fetch the message format from the configuration
            std::string preAnnounceMessageFormat = sConfigMgr->GetOption<std::string>(
                "ServerAutoShutdown.PreAnnounce.Message",
                "[SERVER]: Automated (quick) server restart in {}"
            );
            
            // Format the time string
            std::string formattedTime = Acore::Time::ToTimeString<Seconds>(
                preAnnounceSeconds, TimeOutput::Seconds, TimeFormat::FullText
            );
            
            // Use Acore::StringFormat to substitute the placeholder
            std::string message = Acore::StringFormat(preAnnounceMessageFormat, formattedTime);
            
            // Log the formatted message
            LOG_INFO("module", "{}", message);


            sWorldSessionMgr->SendServerMessage(SERVER_MSG_STRING, message);
            sWorld->ShutdownServ(preAnnounceSeconds, SHUTDOWN_MASK_RESTART, SHUTDOWN_EXIT_CODE);
        });
    }
}

void ServerAutoShutdown::OnUpdate(uint32 diff)
{
    // If module is disabled, do not perform update
    if (!_isEnableModule)
        return;

    scheduler.Update(diff);
}

void ServerAutoShutdown::StartPersistentGameEvents()
{
    std::string eventList = sConfigMgr->GetOption<std::string>("ServerAutoShutdown.StartEvents", "");

    std::vector<std::string_view> tokens = Acore::Tokenize(eventList, ' ', false);
    GameEventMgr::GameEventDataMap const& events = sGameEventMgr->GetEventMap();

    for (auto token : tokens)
    {
        if (token.empty())
        {
            continue;
        }

        uint32 eventId = *Acore::StringTo<uint32>(token);
        sGameEventMgr->StartEvent(eventId);

        GameEventData const& eventData = events[eventId];
        LOG_INFO("module", "> ServerAutoShutdown: Starting event {} ({}).", eventData.Description, eventId);
    }
}
