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
#include <iomanip>

using json = nlohmann::json;

// ============= CONFIGURATION =============
const int MAX_API_CALLS_PER_DAY = 450;
const int CACHE_DURATION_HOURS = 2;
const int POLL_INTERVAL_NO_LIVE = 60;   // 60 minutes when no live matches
const int POLL_INTERVAL_WITH_LIVE = 3;  // 3 minutes when there are live matches

// ============= GLOBAL STATE =============
json cached_fixtures;
std::chrono::system_clock::time_point last_fixture_fetch;
std::mutex cache_mutex;
std::mutex counter_mutex;
int api_calls_today = 0;
int live_match_count = 0;
std::chrono::system_clock::time_point last_reset_date;

// ============= UTILITY FUNCTIONS =============

// Get current date in YYYY-MM-DD format
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

// Get yesterday's date
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

// Reset daily API counter if needed
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

// Check if we can make an API call (quota protection)
bool canMakeApiCall() {
    std::lock_guard<std::mutex> lock(counter_mutex);
    if (api_calls_today >= MAX_API_CALLS_PER_DAY) {
        std::cerr << "\n⚠️  QUOTA LIMIT REACHED! (" << api_calls_today
            << "/" << MAX_API_CALLS_PER_DAY << ")" << std::endl;
        return false;
    }
    return true;
}

// Make RapidAPI request with quota protection
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

// ============= MATCH FORMATTING =============

std::string mapStatusToLabel(int statusCode, const std::string& statusType) {
    // Map SportAPI7 status codes to readable labels
    if (statusCode == 100 || statusType == "inprogress") {
        return "LIVE";
    }
    else if (statusCode == 120 || statusCode == 110 || statusType == "finished") {
        return "FT";
    }
    else if (statusCode == 31) {
        return "HT";
    }
    else if (statusCode == 0 || statusType == "notstarted") {
        return "NS";  // Not Started
    }
    else {
        return "VS";
    }
}

int mapStatusToId(int statusCode, const std::string& statusType) {
    // Map to our internal status IDs
    // 1 = Scheduled, 2 = Live, 3 = HT, 6 = Finished
    if (statusCode == 100 || statusType == "inprogress") {
        return 2;  // Live
    }
    else if (statusCode == 120 || statusCode == 110 || statusType == "finished") {
        return 6;  // Finished
    }
    else if (statusCode == 31) {
        return 3;  // Half Time
    }
    else if (statusCode == 0 || statusType == "notstarted") {
        return 1;  // Scheduled
    }
    else {
        return 1;  // Default to scheduled
    }
}

json formatMatch(const json& event, const std::string& dateLabel) {
    json match;

    try {
        match["id"] = event.value("id", 0);
        match["date"] = dateLabel;

        // Teams
        if (event.contains("homeTeam") && event["homeTeam"].is_object()) {
            match["home_team"] = event["homeTeam"].value("name", "Unknown");
            if (event["homeTeam"].contains("id")) {
                match["home_team_logo"] = "https://img.sofascore.com/api/v1/team/" +
                    std::to_string(event["homeTeam"]["id"].get<int>()) + "/image";
            }
        }

        if (event.contains("awayTeam") && event["awayTeam"].is_object()) {
            match["away_team"] = event["awayTeam"].value("name", "Unknown");
            if (event["awayTeam"].contains("id")) {
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
        }
        else {
            match["statusId"] = 1;
            match["statusName"] = "VS";
        }

        // League/Tournament info
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

        // Timestamp (CRITICAL for frontend date categorization)
        if (event.contains("startTimestamp")) {
            match["timestamp"] = event["startTimestamp"];
        }

    }
    catch (const std::exception& e) {
        std::cerr << "⚠️  Error formatting match: " << e.what() << std::endl;
    }

    return match;
}

// Mark matches as finished if they're not in the live feed anymore
void markFinishedMatches(json& allMatches, const std::unordered_set<int>& liveMatchIds) {
    if (!allMatches.contains("data") || !allMatches["data"].is_array()) {
        return;
    }

    for (auto& match : allMatches["data"]) {
        if (!match.contains("id")) continue;

        int matchId = match["id"].get<int>();
        int statusId = match.value("statusId", 1);

        // If match was LIVE but is NOT in current live feed, mark as finished
        if (statusId == 2 && liveMatchIds.find(matchId) == liveMatchIds.end()) {
            match["statusId"] = 6;
            match["statusName"] = "FT";
            match["date"] = "finished";
            std::cout << "   ✅ Marked match " << matchId << " as finished" << std::endl;
        }
    }
}

// ============= CACHE REFRESH =============

void refreshFixtureCache(const std::string& apiKey) {
    checkAndResetDailyCounter();

    auto now = std::chrono::system_clock::now();
    auto hours_since_fetch = std::chrono::duration_cast<std::chrono::hours>(
        now - last_fixture_fetch
    ).count();

    // Quick status update if cache is still fresh
    if (hours_since_fetch < CACHE_DURATION_HOURS && !cached_fixtures.empty()) {
        if (canMakeApiCall()) {
            std::cout << "\n🔄 Quick status update (cache fresh)..." << std::endl;

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

                    std::cout << "   ✅ Status update complete" << std::endl;
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

    // ===== ENDPOINT 1: LIVE EVENTS =====
    std::cout << "\n📡 Fetching LIVE events..." << std::endl;
    auto liveResponse = makeRapidApiRequest("/api/v1/sport/football/events/live", apiKey);

    if (liveResponse.status_code == 200) {
        try {
            auto data = json::parse(liveResponse.text);

            if (data.contains("events") && data["events"].is_array()) {
                int liveCount = data["events"].size();
                std::cout << "   ✅ Found " << liveCount << " live matches" << std::endl;

                for (auto& event : data["events"]) {
                    if (event.contains("id")) {
                        liveMatchIds.insert(event["id"].get<int>());
                    }
                    allFixtures["data"].push_back(formatMatch(event, "live"));
                }
            }
            else {
                std::cout << "   ℹ️  No live matches" << std::endl;
            }
        }
        catch (const std::exception& e) {
            std::cerr << "   ❌ Parse error: " << e.what() << std::endl;
        }
    }
    else if (liveResponse.status_code == 403) {
        std::cerr << "   ❌ Invalid API Key!" << std::endl;
    }
    else if (liveResponse.status_code == 429) {
        std::cerr << "   ❌ RATE LIMIT HIT!" << std::endl;
        api_calls_today = MAX_API_CALLS_PER_DAY;
        return;
    }

    std::this_thread::sleep_for(std::chrono::seconds(2));

    // ===== ENDPOINT 2: TODAY'S SCHEDULED EVENTS =====
    if (canMakeApiCall()) {
        std::cout << "\n📡 Fetching TODAY'S scheduled events..." << std::endl;
        auto todayResponse = makeRapidApiRequest(
            "/api/v1/sport/football/scheduled-events/" + today,
            apiKey
        );

        if (todayResponse.status_code == 200) {
            try {
                auto data = json::parse(todayResponse.text);

                if (data.contains("events") && data["events"].is_array()) {
                    int todayCount = data["events"].size();
                    std::cout << "   ✅ Found " << todayCount << " scheduled matches today" << std::endl;

                    for (auto& event : data["events"]) {
                        // Skip if already added as live
                        if (event.contains("id") &&
                            liveMatchIds.find(event["id"].get<int>()) != liveMatchIds.end()) {
                            continue;
                        }
                        allFixtures["data"].push_back(formatMatch(event, "today"));
                    }
                }
            }
            catch (const std::exception& e) {
                std::cerr << "   ❌ Parse error: " << e.what() << std::endl;
            }
        }
    }

    std::this_thread::sleep_for(std::chrono::seconds(2));

    // ===== ENDPOINT 3: YESTERDAY'S RESULTS =====
    if (canMakeApiCall()) {
        std::cout << "\n📡 Fetching YESTERDAY'S results..." << std::endl;
        auto yesterdayResponse = makeRapidApiRequest(
            "/api/v1/sport/football/scheduled-events/" + yesterday,
            apiKey
        );

        if (yesterdayResponse.status_code == 200) {
            try {
                auto data = json::parse(yesterdayResponse.text);

                if (data.contains("events") && data["events"].is_array()) {
                    int yesterdayCount = 0;

                    for (auto& event : data["events"]) {
                        // Only include finished matches from yesterday
                        if (event.contains("status") && event["status"].is_object()) {
                            std::string statusType = event["status"].value("type", "");
                            int statusCode = event["status"].value("code", 0);

                            if (statusType == "finished" || statusCode == 120 || statusCode == 110) {
                                allFixtures["data"].push_back(formatMatch(event, "yesterday"));
                                yesterdayCount++;
                            }
                        }
                    }

                    std::cout << "   ✅ Found " << yesterdayCount << " finished matches from yesterday" << std::endl;
                }
            }
            catch (const std::exception& e) {
                std::cerr << "   ❌ Parse error: " << e.what() << std::endl;
            }
        }
    }

    // Update cache
    {
        std::lock_guard<std::mutex> lock(cache_mutex);
        cached_fixtures = allFixtures;
        last_fixture_fetch = now;
    }

    std::cout << "\n✅ Cache refresh complete!" << std::endl;
    std::cout << "   Total matches: " << allFixtures["data"].size() << std::endl;
    std::cout << "   Next full refresh: " << CACHE_DURATION_HOURS << " hours" << std::endl;
}

// Count live matches
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

// Background polling thread
void backgroundPoller(const std::string& apiKey) {
    while (true) {
        refreshFixtureCache(apiKey);

        int liveMatches = countLiveMatches();
        {
            std::lock_guard<std::mutex> lock(counter_mutex);
            live_match_count = liveMatches;
        }

        int pollInterval = (liveMatches > 0) ? POLL_INTERVAL_WITH_LIVE : POLL_INTERVAL_NO_LIVE;

        std::cout << "\n📊 STATUS REPORT:" << std::endl;
        std::cout << "   API Calls Today: " << api_calls_today << "/" << MAX_API_CALLS_PER_DAY << std::endl;
        std::cout << "   Live Matches: " << liveMatches << std::endl;
        std::cout << "   ⏰ Next poll: " << pollInterval << " minutes" << std::endl;

        std::this_thread::sleep_for(std::chrono::minutes(pollInterval));
    }
}

// ============= MAIN =============

int main() {
    crow::SimpleApp app;

    // Get API key from environment
    const char* api_key_env = std::getenv("RAPIDAPI_KEY");
    if (!api_key_env) {
        std::cerr << "❌ ERROR: RAPIDAPI_KEY environment variable not set!" << std::endl;
        return 1;
    }

    std::string api_key = api_key_env;
    std::cout << "✅ API Key loaded: " << api_key.substr(0, 10) << "..." << std::endl;

    // Initialize
    cached_fixtures["data"] = json::array();
    last_fixture_fetch = std::chrono::system_clock::now() - std::chrono::hours(CACHE_DURATION_HOURS + 1);
    last_reset_date = std::chrono::system_clock::now();

    std::cout << "\n⚡ REALSSA SPORTS API v11.0" << std::endl;
    std::cout << "================================" << std::endl;
    std::cout << "   Max API Calls: " << MAX_API_CALLS_PER_DAY << "/day" << std::endl;
    std::cout << "   Cache Duration: " << CACHE_DURATION_HOURS << " hours" << std::endl;
    std::cout << "   Poll Interval (No Live): " << POLL_INTERVAL_NO_LIVE << " min" << std::endl;
    std::cout << "   Poll Interval (Live): " << POLL_INTERVAL_WITH_LIVE << " min" << std::endl;
    std::cout << "================================\n" << std::endl;

    // Start background poller
    std::thread poller(backgroundPoller, api_key);
    poller.detach();

    // ===== ROUTES =====

    // Root endpoint
    CROW_ROUTE(app, "/")
        ([]() {
        crow::response res;
        res.set_header("Content-Type", "application/json");
        res.set_header("Access-Control-Allow-Origin", "*");
        res.write(R"({
  "message": "RealSSA Sports API",
  "status": "online",
  "version": "11.0.0",
  "features": ["Live Scores", "Yesterday Results", "Today Matches", "Auto Status Tracking"]
})");
        return res;
            });

    // Scores endpoint
    CROW_ROUTE(app, "/scores")
        ([&api_key]() {
        auto now = std::chrono::system_clock::now();
        auto hours_since = std::chrono::duration_cast<std::chrono::hours>(
            now - last_fixture_fetch
        ).count();

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

    // Stats endpoint
    CROW_ROUTE(app, "/stats/<int>")
        ([&api_key](int match_id) {
        std::string endpoint = "/api/v1/event/" + std::to_string(match_id) + "/statistics";

        if (!canMakeApiCall()) {
            crow::response res(503);
            res.set_header("Content-Type", "application/json");
            res.set_header("Access-Control-Allow-Origin", "*");
            res.write(R"({"error": "Quota limit reached"})");
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
            res.write(R"({"error": "Stats not available"})");
        }

        return res;
            });

    // Health endpoint
    CROW_ROUTE(app, "/health")
        ([]() {
        json health;
        health["status"] = "online";
        health["version"] = "11.0.0";
        health["engine"] = "RealSSA Smart Engine";
        health["today"] = getTodayDate();

        health["quota"] = {
            {"calls_today", api_calls_today},
            {"max_calls_per_day", MAX_API_CALLS_PER_DAY},
            {"calls_remaining", MAX_API_CALLS_PER_DAY - api_calls_today}
        };

        health["matches"] = {
            {"live", live_match_count},
            {"total_cached", cached_fixtures.contains("data") ? cached_fixtures["data"].size() : 0}
        };

        health["cache"] = {
            {"duration_hours", CACHE_DURATION_HOURS},
            {"poll_interval_live_min", POLL_INTERVAL_WITH_LIVE},
            {"poll_interval_no_live_min", POLL_INTERVAL_NO_LIVE}
        };

        crow::response res;
        res.set_header("Content-Type", "application/json");
        res.set_header("Access-Control-Allow-Origin", "*");
        res.write(health.dump(2));
        return res;
            });

    // Start server
    const char* port_env = std::getenv("PORT");
    int port = port_env ? std::stoi(port_env) : 8080;

    std::cout << "🚀 Server starting on Port " << port << std::endl;
    std::cout << "🌐 Live at: http://localhost:" << port << std::endl;

    app.port(port).multithreaded().run();

    return 0;
}