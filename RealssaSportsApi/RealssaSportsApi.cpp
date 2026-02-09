#include <crow.h>
#include <nlohmann/json.hpp>
#include <cpr/cpr.h>
#include <iostream>
#include <string>
#include <vector>
#include <chrono>
#include <thread>
#include <mutex>
#include <ctime>
#include <unordered_set>
#include <unordered_map>

using json = nlohmann::json;

// ============= CONFIGURATION =============
const int MAX_API_CALLS_PER_DAY = 450;
const int CACHE_DURATION_HOURS = 1;          // Reduced for faster updates
const int POLL_INTERVAL_NO_LIVE = 30;        // 30 minutes when no live
const int POLL_INTERVAL_WITH_LIVE = 2;       // 2 minutes when live
const int MATCH_DURATION_HOURS = 2;          // Football match typically ~2 hours

// ============= GLOBAL STATE =============
json cached_fixtures;
std::chrono::system_clock::time_point last_fixture_fetch;
std::mutex cache_mutex;
std::mutex counter_mutex;
int api_calls_today = 0;
int live_match_count = 0;
std::chrono::system_clock::time_point last_reset_date;

// Track when matches went live (for auto-FT detection)
std::unordered_map<int, std::chrono::system_clock::time_point> live_match_timestamps;
std::mutex timestamps_mutex;

// ============= UTILITY FUNCTIONS =============

std::string getTodayDate() {
    auto now = std::chrono::system_clock::now();
    std::time_t now_time = std::chrono::system_clock::to_time_t(now);
    std::tm timeinfo;

#ifdef _WIN32
    localtime_s(&timeinfo, &now_time);
#else
    localtime_r(&now_time, &timeinfo);
#endif

    char buffer[11];
    std::strftime(buffer, sizeof(buffer), "%Y-%m-%d", &timeinfo);
    return std::string(buffer);
}

std::string getYesterdayDate() {
    auto now = std::chrono::system_clock::now();
    auto yesterday = now - std::chrono::hours(24);
    std::time_t yesterday_time = std::chrono::system_clock::to_time_t(yesterday);
    std::tm timeinfo;

#ifdef _WIN32
    localtime_s(&timeinfo, &yesterday_time);
#else
    localtime_r(&yesterday_time, &timeinfo);
#endif

    char buffer[11];
    std::strftime(buffer, sizeof(buffer), "%Y-%m-%d", &timeinfo);
    return std::string(buffer);
}

void checkAndResetDailyCounter() {
    auto now = std::chrono::system_clock::now();
    auto hours_diff = std::chrono::duration_cast<std::chrono::hours>(
        now - last_reset_date
    ).count();

    if (hours_diff >= 24) {
        std::lock_guard<std::mutex> lock(counter_mutex);
        api_calls_today = 0;
        last_reset_date = now;
        std::cout << "\n🔄 Daily API counter reset!" << std::endl;
    }
}

bool canMakeApiCall() {
    std::lock_guard<std::mutex> lock(counter_mutex);
    if (api_calls_today >= MAX_API_CALLS_PER_DAY) {
        std::cerr << "⚠️  QUOTA LIMIT REACHED! (" << api_calls_today
            << "/" << MAX_API_CALLS_PER_DAY << ")" << std::endl;
        return false;
    }
    return true;
}

cpr::Response makeRapidApiRequest(const std::string& endpoint, const std::string& apiKey) {
    if (!canMakeApiCall()) {
        return cpr::Response{};
    }

    auto response = cpr::Get(
        cpr::Url{ "https://sportapi7.p.rapidapi.com" + endpoint },
        cpr::Header{
            {"x-rapidapi-host", "sportapi7.p.rapidapi.com"},
            {"x-rapidapi-key", apiKey}
        },
        cpr::Timeout{ 10000 }
    );

    if (response.status_code != 0) {
        std::lock_guard<std::mutex> lock(counter_mutex);
        api_calls_today++;
        std::cout << "   📊 API Calls: " << api_calls_today
            << "/" << MAX_API_CALLS_PER_DAY << std::endl;
    }

    return response;
}

// ============= STATUS DETECTION =============

std::string mapStatusToLabel(int statusCode, const std::string& statusType) {
    // Comprehensive status mapping
    if (statusCode == 100 || statusType == "inprogress") {
        return "LIVE";
    }
    else if (statusCode == 120 || statusCode == 110 || statusType == "finished") {
        return "FT";
    }
    else if (statusCode == 60 || statusCode == 50 || statusCode == 31) {
        return "HT";
    }
    else if (statusCode == 0 || statusType == "notstarted") {
        return "NS";
    }
    else if (statusType == "canceled" || statusType == "cancelled") {
        return "Canc.";
    }
    else if (statusType == "postponed") {
        return "Postp.";
    }
    else if (statusType == "interrupted" || statusType == "suspended") {
        return "Susp.";
    }
    else if (statusCode >= 70 && statusCode <= 90) {
        return "2H"; // Second half variations
    }
    return "VS";
}

int mapStatusToId(int statusCode, const std::string& statusType) {
    // 1 = Scheduled, 2 = Live, 3 = HT, 6 = Finished, 7 = Cancelled/Postponed
    if (statusCode == 100 || statusType == "inprogress" ||
        (statusCode >= 70 && statusCode <= 90)) {
        return 2;  // Live (including 2nd half)
    }
    else if (statusCode == 120 || statusCode == 110 || statusType == "finished") {
        return 6;  // Finished
    }
    else if (statusCode == 60 || statusCode == 50 || statusCode == 31) {
        return 3;  // Half Time
    }
    else if (statusType == "canceled" || statusType == "cancelled" ||
        statusType == "postponed") {
        return 7;  // Cancelled/Postponed
    }
    else if (statusCode == 0 || statusType == "notstarted") {
        return 1;  // Scheduled
    }
    return 1;
}

// Auto-finish detection based on time
void applyAutoFinishLogic(json& match) {
    if (!match.contains("timestamp") || !match.contains("statusId")) {
        return;
    }

    int statusId = match["statusId"].get<int>();
    int64_t timestamp = match["timestamp"].get<int64_t>();

    // Only check live or HT matches
    if (statusId != 2 && statusId != 3) {
        return;
    }

    auto now = std::chrono::system_clock::now();
    auto match_start = std::chrono::system_clock::from_time_t(timestamp);
    auto duration = std::chrono::duration_cast<std::chrono::minutes>(now - match_start);

    // If match started more than 130 minutes ago (90min + 30min extra time + 10min buffer)
    if (duration.count() > 130) {
        std::cout << "   🤖 Auto-finishing match " << match["id"]
            << " (started " << duration.count() << " minutes ago)" << std::endl;
        match["statusId"] = 6;
        match["statusName"] = "FT";
        match["date"] = "finished";
    }
}

// ============= MATCH FORMATTING =============

json formatMatch(const json& event, const std::string& dateLabel) {
    json match;

    try {
        match["id"] = event.value("id", 0);
        match["date"] = dateLabel;

        // Teams
        if (event.contains("homeTeam") && event["homeTeam"].is_object()) {
            match["home_team"] = event["homeTeam"].value("name", "Unknown");
            if (event["homeTeam"].contains("id")) {
                match["home_team_id"] = event["homeTeam"]["id"];
                match["home_team_logo"] = "https://img.sofascore.com/api/v1/team/" +
                    std::to_string(event["homeTeam"]["id"].get<int>()) + "/image";
            }
        }

        if (event.contains("awayTeam") && event["awayTeam"].is_object()) {
            match["away_team"] = event["awayTeam"].value("name", "Unknown");
            if (event["awayTeam"].contains("id")) {
                match["away_team_id"] = event["awayTeam"]["id"];
                match["away_team_logo"] = "https://img.sofascore.com/api/v1/team/" +
                    std::to_string(event["awayTeam"]["id"].get<int>()) + "/image";
            }
        }

        // Scores
        if (event.contains("homeScore") && event["homeScore"].is_object()) {
            match["home_score"] = event["homeScore"].value("current", 0);
            match["home_score_ht"] = event["homeScore"].value("period1", 0);
        }
        else {
            match["home_score"] = 0;
        }

        if (event.contains("awayScore") && event["awayScore"].is_object()) {
            match["away_score"] = event["awayScore"].value("current", 0);
            match["away_score_ht"] = event["awayScore"].value("period1", 0);
        }
        else {
            match["away_score"] = 0;
        }

        // Status
        if (event.contains("status") && event["status"].is_object()) {
            int statusCode = event["status"].value("code", 0);
            std::string statusType = event["status"].value("type", "");

            match["statusId"] = mapStatusToId(statusCode, statusType);
            match["statusName"] = mapStatusToLabel(statusCode, statusType);

            if (event["status"].contains("description")) {
                match["time"] = event["status"]["description"];
            }

            // Debug logging
            std::cout << "   Match " << match["id"] << " - Code: " << statusCode
                << " Type: " << statusType << " → " << match["statusName"] << std::endl;
        }
        else {
            match["statusId"] = 1;
            match["statusName"] = "NS";
        }

        // League info
        if (event.contains("tournament") && event["tournament"].is_object()) {
            match["league"] = event["tournament"].value("name", "");

            if (event["tournament"].contains("category") &&
                event["tournament"]["category"].is_object()) {
                match["league_country"] = event["tournament"]["category"].value("name", "");
            }

            if (event["tournament"].contains("id")) {
                match["league_logo"] = "https://img.sofascore.com/api/v1/unique-tournament/" +
                    std::to_string(event["tournament"]["id"].get<int>()) + "/image";
            }
        }

        // Timestamp
        if (event.contains("startTimestamp")) {
            match["timestamp"] = event["startTimestamp"];
        }

        // Apply auto-finish logic
        applyAutoFinishLogic(match);

    }
    catch (const std::exception& e) {
        std::cerr << "⚠️  Error formatting match: " << e.what() << std::endl;
    }

    return match;
}

void markFinishedMatches(json& allMatches, const std::unordered_set<int>& liveMatchIds) {
    if (!allMatches.contains("data") || !allMatches["data"].is_array()) {
        return;
    }

    for (auto& match : allMatches["data"]) {
        if (!match.contains("id")) continue;

        int matchId = match["id"].get<int>();
        int statusId = match.value("statusId", 1);

        // If match was LIVE/HT but is NOT in current live feed, mark as finished
        if ((statusId == 2 || statusId == 3) && liveMatchIds.find(matchId) == liveMatchIds.end()) {
            std::cout << "   ✅ Auto-finishing match " << matchId
                << " (no longer in live feed)" << std::endl;
            match["statusId"] = 6;
            match["statusName"] = "FT";
            match["date"] = "finished";
        }

        // Also apply time-based auto-finish
        applyAutoFinishLogic(match);
    }
}

// ============= CACHE REFRESH =============

void refreshFixtureCache(const std::string& apiKey) {
    checkAndResetDailyCounter();

    auto now = std::chrono::system_clock::now();
    auto hours_since_fetch = std::chrono::duration_cast<std::chrono::hours>(
        now - last_fixture_fetch
    ).count();

    // Quick status update if cache is fresh
    if (hours_since_fetch < CACHE_DURATION_HOURS && !cached_fixtures.empty()) {
        if (canMakeApiCall()) {
            std::cout << "\n🔄 Quick status update..." << std::endl;

            auto liveResponse = makeRapidApiRequest("/api/v1/sport/football/events/live", apiKey);

            if (liveResponse.status_code == 200) {
                try {
                    auto data = json::parse(liveResponse.text);
                    std::unordered_set<int> currentLiveIds;

                    if (data.contains("events") && data["events"].is_array()) {
                        for (const auto& event : data["events"]) {
                            if (event.contains("id")) {
                                currentLiveIds.insert(event["id"].get<int>());
                            }
                        }
                    }

                    std::lock_guard<std::mutex> lock(cache_mutex);
                    markFinishedMatches(cached_fixtures, currentLiveIds);

                }
                catch (const std::exception& e) {
                    std::cerr << "   ❌ Parse error: " << e.what() << std::endl;
                }
            }
        }
        return;
    }

    if (!canMakeApiCall()) {
        std::cout << "⏸️  Skipping cache refresh - quota protection" << std::endl;
        return;
    }

    std::cout << "\n🔄 FULL CACHE REFRESH..." << std::endl;

    json allFixtures;
    allFixtures["data"] = json::array();
    std::unordered_set<int> liveMatchIds;

    std::string today = getTodayDate();
    std::string yesterday = getYesterdayDate();

    // LIVE EVENTS
    std::cout << "\n📡 Fetching LIVE events..." << std::endl;
    auto liveResponse = makeRapidApiRequest("/api/v1/sport/football/events/live", apiKey);

    if (liveResponse.status_code == 200) {
        try {
            auto data = json::parse(liveResponse.text);

            if (data.contains("events") && data["events"].is_array()) {
                std::cout << "   ✅ Found " << data["events"].size() << " live matches" << std::endl;

                for (auto& event : data["events"]) {
                    if (event.contains("id")) {
                        int matchId = event["id"].get<int>();
                        liveMatchIds.insert(matchId);
                    }
                    allFixtures["data"].push_back(formatMatch(event, "live"));
                }
            }
        }
        catch (const std::exception& e) {
            std::cerr << "   ❌ Error: " << e.what() << std::endl;
        }
    }

    std::this_thread::sleep_for(std::chrono::seconds(2));

    // TODAY'S EVENTS
    if (canMakeApiCall()) {
        std::cout << "\n📡 Fetching TODAY'S events..." << std::endl;
        auto todayResponse = makeRapidApiRequest(
            "/api/v1/sport/football/scheduled-events/" + today, apiKey);

        if (todayResponse.status_code == 200) {
            try {
                auto data = json::parse(todayResponse.text);

                if (data.contains("events") && data["events"].is_array()) {
                    int count = 0;
                    for (auto& event : data["events"]) {
                        if (event.contains("id") &&
                            liveMatchIds.find(event["id"].get<int>()) == liveMatchIds.end()) {
                            allFixtures["data"].push_back(formatMatch(event, "today"));
                            count++;
                        }
                    }
                    std::cout << "   ✅ Found " << count << " matches today" << std::endl;
                }
            }
            catch (const std::exception& e) {
                std::cerr << "   ❌ Error: " << e.what() << std::endl;
            }
        }
    }

    std::this_thread::sleep_for(std::chrono::seconds(2));

    // YESTERDAY'S EVENTS
    if (canMakeApiCall()) {
        std::cout << "\n📡 Fetching YESTERDAY'S events..." << std::endl;
        auto yesterdayResponse = makeRapidApiRequest(
            "/api/v1/sport/football/scheduled-events/" + yesterday, apiKey);

        if (yesterdayResponse.status_code == 200) {
            try {
                auto data = json::parse(yesterdayResponse.text);

                if (data.contains("events") && data["events"].is_array()) {
                    int count = 0;
                    for (auto& event : data["events"]) {
                        if (event.contains("status") && event["status"].is_object()) {
                            std::string statusType = event["status"].value("type", "");
                            if (statusType == "finished") {
                                allFixtures["data"].push_back(formatMatch(event, "yesterday"));
                                count++;
                            }
                        }
                    }
                    std::cout << "   ✅ Found " << count << " finished matches yesterday" << std::endl;
                }
            }
            catch (const std::exception& e) {
                std::cerr << "   ❌ Error: " << e.what() << std::endl;
            }
        }
    }

    {
        std::lock_guard<std::mutex> lock(cache_mutex);
        cached_fixtures = allFixtures;
        last_fixture_fetch = now;
    }

    std::cout << "\n✅ Cache refresh complete: " << allFixtures["data"].size()
        << " total matches" << std::endl;
}

int countLiveMatches() {
    std::lock_guard<std::mutex> lock(cache_mutex);
    int count = 0;

    if (cached_fixtures.contains("data") && cached_fixtures["data"].is_array()) {
        for (const auto& match : cached_fixtures["data"]) {
            if (match.contains("statusId") && match["statusId"] == 2) {
                count++;
            }
        }
    }

    return count;
}

void backgroundPoller(const std::string& apiKey) {
    while (true) {
        refreshFixtureCache(apiKey);

        int liveMatches = countLiveMatches();
        {
            std::lock_guard<std::mutex> lock(counter_mutex);
            live_match_count = liveMatches;
        }

        int pollInterval = (liveMatches > 0) ? POLL_INTERVAL_WITH_LIVE : POLL_INTERVAL_NO_LIVE;

        std::cout << "\n📊 STATUS:" << std::endl;
        std::cout << "   API Calls: " << api_calls_today << "/" << MAX_API_CALLS_PER_DAY << std::endl;
        std::cout << "   Live Matches: " << liveMatches << std::endl;
        std::cout << "   ⏰ Next poll: " << pollInterval << " minutes" << std::endl;

        std::this_thread::sleep_for(std::chrono::minutes(pollInterval));
    }
}

// ============= MAIN =============

int main() {
    crow::SimpleApp app;

    const char* api_key_env = std::getenv("RAPIDAPI_KEY");
    if (!api_key_env) {
        std::cerr << "❌ ERROR: RAPIDAPI_KEY not set!" << std::endl;
        return 1;
    }

    std::string api_key = api_key_env;
    std::cout << "✅ API Key loaded" << std::endl;

    cached_fixtures["data"] = json::array();
    last_fixture_fetch = std::chrono::system_clock::now() - std::chrono::hours(CACHE_DURATION_HOURS + 1);
    last_reset_date = std::chrono::system_clock::now();

    std::cout << "\n⚡ REALSSA SPORTS API v12.0 - AUTO-FT EDITION" << std::endl;
    std::cout << "=============================================" << std::endl;

    std::thread poller(backgroundPoller, api_key);
    poller.detach();

    // ROOT
    CROW_ROUTE(app, "/")
        ([]() {
        crow::response res;
        res.set_header("Content-Type", "application/json");
        res.set_header("Access-Control-Allow-Origin", "*");
        res.write(R"({"message":"RealSSA Sports API","version":"12.0","status":"online"})");
        return res;
            });

    // SCORES
    CROW_ROUTE(app, "/scores")
        ([&api_key]() {
        auto now = std::chrono::system_clock::now();
        auto hours_since = std::chrono::duration_cast<std::chrono::hours>(
            now - last_fixture_fetch).count();

        if (hours_since >= CACHE_DURATION_HOURS && canMakeApiCall()) {
            refreshFixtureCache(api_key);
        }

        json response;
        {
            std::lock_guard<std::mutex> lock(cache_mutex);
            response = cached_fixtures;
        }

        response["quota_status"] = {
            {"calls_used", api_calls_today},
            {"calls_limit", MAX_API_CALLS_PER_DAY},
            {"calls_remaining", MAX_API_CALLS_PER_DAY - api_calls_today}
        };

        crow::response res;
        res.set_header("Content-Type", "application/json");
        res.set_header("Access-Control-Allow-Origin", "*");
        res.write(response.dump(2));
        return res;
            });

    // STATISTICS
    CROW_ROUTE(app, "/stats/<int>")
        ([&api_key](int match_id) {
        std::string endpoint = "/api/v1/event/" + std::to_string(match_id) + "/statistics";

        if (!canMakeApiCall()) {
            crow::response res(503);
            res.set_header("Content-Type", "application/json");
            res.set_header("Access-Control-Allow-Origin", "*");
            res.write(R"({"error":"Quota limit reached"})");
            return res;
        }

        auto statsResponse = makeRapidApiRequest(endpoint, api_key);

        crow::response res;
        res.set_header("Content-Type", "application/json");
        res.set_header("Access-Control-Allow-Origin", "*");

        if (statsResponse.status_code == 200) {
            res.write(statsResponse.text);
        }
        else {
            res.code = 404;
            res.write(R"({"error":"Stats not available"})");
        }

        return res;
            });

    // HEALTH
    CROW_ROUTE(app, "/health")
        ([]() {
        json health;
        health["status"] = "online";
        health["version"] = "12.0 - Auto-FT";
        health["quota"] = {
            {"calls_today", api_calls_today},
            {"limit", MAX_API_CALLS_PER_DAY}
        };
        health["matches"] = {
            {"live", live_match_count},
            {"cached", cached_fixtures.contains("data") ? cached_fixtures["data"].size() : 0}
        };

        crow::response res;
        res.set_header("Content-Type", "application/json");
        res.set_header("Access-Control-Allow-Origin", "*");
        res.write(health.dump(2));
        return res;
            });

    const char* port_env = std::getenv("PORT");
    int port = port_env ? std::stoi(port_env) : 8080;

    std::cout << "🚀 Server on port " << port << std::endl;
    app.port(port).multithreaded().run();

    return 0;
}