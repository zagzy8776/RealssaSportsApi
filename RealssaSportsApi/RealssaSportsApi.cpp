#include <iostream>
#include <string>
#include <vector>
#include <thread>
#include <chrono>
#include <mutex>
#include <map>
#include <set>
#include <cpr/cpr.h>
#include <nlohmann/json.hpp>
#include <httplib.h>

using json = nlohmann::json;

// Global thread-safe data
std::string latest_match_data = "{\"status\": \"initializing\"}";
std::mutex data_mutex;

// Cached fixture data (refreshed less frequently)
json cached_fixtures;
std::chrono::system_clock::time_point last_fixture_fetch;
std::mutex cache_mutex;

// Request counter
int api_calls_today = 0;
std::mutex counter_mutex;

// Priority leagues (to reduce API calls)
const std::vector<std::string> PRIORITY_LEAGUES = {
    "PL",   // Premier League
    "PD",   // La Liga
    "SA",   // Serie A
    "BL1",  // Bundesliga
    "FL1",  // Ligue 1
    "CL",   // Champions League
    "WC",   // World Cup
    "EC"    // European Championship
};

// Function to get current date in YYYY-MM-DD format (Cross-platform)
std::string getTodayDate() {
    auto now = std::chrono::system_clock::now();
    std::time_t now_time = std::chrono::system_clock::to_time_t(now);
    struct tm timeinfo;

#ifdef _WIN32
    localtime_s(&timeinfo, &now_time);
#else
    localtime_r(&now_time, &timeinfo);
#endif

    char buffer[11];
    strftime(buffer, sizeof(buffer), "%Y-%m-%d", &timeinfo);
    return std::string(buffer);
}

// Get date with offset
std::string getDateWithOffset(int dayOffset) {
    auto now = std::chrono::system_clock::now();
    auto target = now + std::chrono::hours(24 * dayOffset);
    std::time_t target_time = std::chrono::system_clock::to_time_t(target);
    struct tm timeinfo;

#ifdef _WIN32
    localtime_s(&timeinfo, &target_time);
#else
    localtime_r(&target_time, &timeinfo);
#endif

    char buffer[11];
    strftime(buffer, sizeof(buffer), "%Y-%m-%d", &timeinfo);
    return std::string(buffer);
}

// Format match data from API
json formatMatch(const json& match, const std::string& dateLabel = "") {
    json formattedMatch;

    formattedMatch["id"] = match.value("id", 0);
    formattedMatch["time"] = match.value("utcDate", "");

    // Teams
    formattedMatch["home"]["name"] = match["homeTeam"].value("name", "");
    formattedMatch["home"]["shortName"] = match["homeTeam"].value("shortName", "");
    formattedMatch["home"]["tla"] = match["homeTeam"].value("tla", "");
    formattedMatch["home"]["crest"] = match["homeTeam"].value("crest", "");

    formattedMatch["away"]["name"] = match["awayTeam"].value("name", "");
    formattedMatch["away"]["shortName"] = match["awayTeam"].value("shortName", "");
    formattedMatch["away"]["tla"] = match["awayTeam"].value("tla", "");
    formattedMatch["away"]["crest"] = match["awayTeam"].value("crest", "");

    // Scores
    if (match.contains("score") && match["score"].contains("fullTime")) {
        formattedMatch["home"]["score"] = match["score"]["fullTime"]["home"].is_null() ? 0 : match["score"]["fullTime"]["home"].get<int>();
        formattedMatch["away"]["score"] = match["score"]["fullTime"]["away"].is_null() ? 0 : match["score"]["fullTime"]["away"].get<int>();
    }
    else {
        formattedMatch["home"]["score"] = 0;
        formattedMatch["away"]["score"] = 0;
    }

    // Half-time score
    if (match.contains("score") && match["score"].contains("halfTime")) {
        formattedMatch["halftime"]["home"] = match["score"]["halfTime"]["home"].is_null() ? 0 : match["score"]["halfTime"]["home"].get<int>();
        formattedMatch["halftime"]["away"] = match["score"]["halfTime"]["away"].is_null() ? 0 : match["score"]["halfTime"]["away"].get<int>();
    }

    // Status
    std::string status = match.value("status", "SCHEDULED");
    formattedMatch["status"] = status;
    formattedMatch["status_text"] = status;

    if (status == "IN_PLAY" || status == "PAUSED") {
        formattedMatch["statusId"] = 2; // Live
        formattedMatch["is_live"] = true;
    }
    else if (status == "FINISHED") {
        formattedMatch["statusId"] = 6; // Finished
        formattedMatch["is_live"] = false;
    }
    else {
        formattedMatch["statusId"] = 1; // Scheduled
        formattedMatch["is_live"] = false;
    }

    // Competition/League
    if (match.contains("competition")) {
        formattedMatch["leagueId"] = match["competition"].value("id", 0);
        formattedMatch["leagueName"] = match["competition"].value("name", "");
        formattedMatch["leagueCode"] = match["competition"].value("code", "");
        formattedMatch["leagueEmblem"] = match["competition"].value("emblem", "");
    }

    formattedMatch["matchday"] = match.value("matchday", 0);
    formattedMatch["venue"] = match.value("venue", "");

    if (!dateLabel.empty()) {
        formattedMatch["date_label"] = dateLabel;
    }

    return formattedMatch;
}

// Fetch all fixtures (cached for 2 hours)
void refreshFixtureCache(const std::string& apiKey) {
    auto now = std::chrono::system_clock::now();
    auto hours_since_fetch = std::chrono::duration_cast<std::chrono::hours>(
        now - last_fixture_fetch
    ).count();

    // Only refresh if 2+ hours have passed
    if (hours_since_fetch < 2 && !cached_fixtures.empty()) {
        std::cout << "💾 Using cached fixtures (last fetch: " << hours_since_fetch << "h ago)" << std::endl;
        return;
    }

    std::cout << "\n🔄 Refreshing fixture cache..." << std::endl;

    json allFixtures;
    allFixtures["data"] = json::array();

    std::string today = getTodayDate();
    std::string tomorrow = getDateWithOffset(1);

    // Fetch today's fixtures
    std::cout << "📡 Fetching today's fixtures (" << today << ")..." << std::endl;
    auto todayResponse = cpr::Get(
        cpr::Url{ "https://api.football-data.org/v4/matches" },
        cpr::Header{ {"X-Auth-Token", apiKey} },
        cpr::Parameters{ {"dateFrom", today}, {"dateTo", today} }
    );

    {
        std::lock_guard<std::mutex> lock(counter_mutex);
        api_calls_today++;
    }

    if (todayResponse.status_code == 200) {
        try {
            auto data = json::parse(todayResponse.text);
            if (data.contains("matches") && data["matches"].is_array()) {
                for (auto& match : data["matches"]) {
                    allFixtures["data"].push_back(formatMatch(match, "today"));
                }
                std::cout << "   ✅ " << data["matches"].size() << " matches" << std::endl;
            }
        }
        catch (const std::exception& e) {
            std::cerr << "   ❌ Error: " << e.what() << std::endl;
        }
    }
    else {
        std::cerr << "   ❌ HTTP " << todayResponse.status_code << std::endl;
    }

    std::this_thread::sleep_for(std::chrono::seconds(2));

    // Fetch tomorrow's fixtures
    std::cout << "📡 Fetching tomorrow's fixtures (" << tomorrow << ")..." << std::endl;
    auto tomorrowResponse = cpr::Get(
        cpr::Url{ "https://api.football-data.org/v4/matches" },
        cpr::Header{ {"X-Auth-Token", apiKey} },
        cpr::Parameters{ {"dateFrom", tomorrow}, {"dateTo", tomorrow} }
    );

    {
        std::lock_guard<std::mutex> lock(counter_mutex);
        api_calls_today++;
    }

    if (tomorrowResponse.status_code == 200) {
        try {
            auto data = json::parse(tomorrowResponse.text);
            if (data.contains("matches") && data["matches"].is_array()) {
                for (auto& match : data["matches"]) {
                    allFixtures["data"].push_back(formatMatch(match, "tomorrow"));
                }
                std::cout << "   ✅ " << data["matches"].size() << " matches" << std::endl;
            }
        }
        catch (const std::exception& e) {
            std::cerr << "   ❌ Error: " << e.what() << std::endl;
        }
    }
    else {
        std::cerr << "   ❌ HTTP " << tomorrowResponse.status_code << std::endl;
    }

    {
        std::lock_guard<std::mutex> lock(cache_mutex);
        cached_fixtures = allFixtures;
        last_fixture_fetch = now;
    }

    std::cout << "✅ Fixture cache refreshed: " << allFixtures["data"].size() << " total matches" << std::endl;
}

// Smart update loop
void smartUpdateLoop(const std::string& apiKey) {
    // Initialize
    last_fixture_fetch = std::chrono::system_clock::now() - std::chrono::hours(3);

    while (true) {
        std::cout << "\n━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━" << std::endl;
        std::cout << "⚽ REALSSA SMART ENGINE" << std::endl;
        std::cout << "━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━" << std::endl;

        // Refresh fixture cache (only if needed - every 2 hours)
        refreshFixtureCache(apiKey);

        // Build response from cache
        json response;
        {
            std::lock_guard<std::mutex> lock(cache_mutex);
            response = cached_fixtures;
        }

        // Add metadata
        auto now = std::chrono::system_clock::now();
        auto timestamp = std::chrono::duration_cast<std::chrono::seconds>(
            now.time_since_epoch()
        ).count();

        response["status"] = "success";
        response["server_timestamp"] = timestamp;
        response["server"] = "Realssa Smart Engine";
        response["provider"] = "football-data.org";
        response["today"] = getTodayDate();
        response["tomorrow"] = getDateWithOffset(1);
        response["total_matches"] = response["data"].size();

        // Count live matches
        int liveCount = 0;
        int finishedCount = 0;
        int upcomingCount = 0;

        for (auto& match : response["data"]) {
            if (match.value("is_live", false)) {
                liveCount++;
            }
            else if (match.value("statusId", 0) == 6) {
                finishedCount++;
            }
            else {
                upcomingCount++;
            }
        }

        response["live_matches"] = liveCount;
        response["finished_matches"] = finishedCount;
        response["upcoming_matches"] = upcomingCount;
        response["api_calls_today"] = api_calls_today;

        // Save to global
        {
            std::lock_guard<std::mutex> lock(data_mutex);
            latest_match_data = response.dump();
        }

        std::cout << "\n📊 SUMMARY:" << std::endl;
        std::cout << "   🔴 Live: " << liveCount << std::endl;
        std::cout << "   ✅ Finished: " << finishedCount << std::endl;
        std::cout << "   ⏰ Upcoming: " << upcomingCount << std::endl;
        std::cout << "   📊 Total: " << response["data"].size() << std::endl;
        std::cout << "   📞 API Calls Today: " << api_calls_today << std::endl;
        std::cout << "━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━" << std::endl;

        // Smart polling:
        // - If there are live matches: update every 5 minutes
        // - If no live matches: update every 30 minutes
        int waitMinutes = (liveCount > 0) ? 5 : 30;

        std::cout << "⏳ Next update in " << waitMinutes << " minutes";
        if (liveCount > 0) {
            std::cout << " (LIVE MATCHES DETECTED - Fast polling)";
        }
        std::cout << "\n" << std::endl;

        std::this_thread::sleep_for(std::chrono::minutes(waitMinutes));
    }
}

int main() {
    std::cout << R"(
    ╔═══════════════════════════════════════╗
    ║   Realssa Smart Football Engine      ║
    ║   Powered by football-data.org       ║
    ║   🚀 Intelligent Caching System      ║
    ╚═══════════════════════════════════════╝
    )" << std::endl;

    const char* envKey = std::getenv("FOOTBALL_DATA_KEY");
    std::string myKey = envKey ? envKey : "";

    if (myKey.empty()) {
        std::cerr << "❌ ERROR: FOOTBALL_DATA_KEY environment variable not set!" << std::endl;
        return 1;
    }

    std::cout << "✅ API Key: " << myKey.substr(0, 8) << "..." << std::endl;
    std::cout << "🌐 Provider: football-data.org" << std::endl;
    std::cout << "💾 Smart Caching: Fixtures cached for 2 hours" << std::endl;
    std::cout << "⚡ Dynamic Polling: 5min when live, 30min otherwise" << std::endl;
    std::cout << "📊 Est. Daily Calls: 10-50 (depending on live matches)" << std::endl;

    // Start background sync
    std::thread worker(smartUpdateLoop, myKey);
    worker.detach();

    // Start API Server
    httplib::Server svr;

    svr.Options("/(.*)", [](const httplib::Request&, httplib::Response& res) {
        res.set_header("Access-Control-Allow-Origin", "*");
        res.set_header("Access-Control-Allow-Methods", "GET, OPTIONS");
        res.set_header("Access-Control-Allow-Headers", "Content-Type");
        res.status = 204;
        });

    svr.Get("/scores", [](const httplib::Request&, httplib::Response& res) {
        std::lock_guard<std::mutex> lock(data_mutex);
        res.set_header("Access-Control-Allow-Origin", "*");
        res.set_header("Content-Type", "application/json");
        res.set_content(latest_match_data, "application/json");
        });

    svr.Get("/", [](const httplib::Request&, httplib::Response& res) {
        res.set_header("Access-Control-Allow-Origin", "*");
        res.set_content("⚽ Realssa Smart Football Engine - Powered by football-data.org!", "text/plain");
        });

    svr.Get("/health", [](const httplib::Request&, httplib::Response& res) {
        json health;
        health["status"] = "online";
        health["engine"] = "Realssa Smart Football Engine";
        health["version"] = "7.0.0 - Smart Cache";
        health["api_provider"] = "football-data.org";
        health["features"] = {
            "smart_caching",
            "dynamic_polling",
            "live_detection",
            "auto_date_rotation"
        };

        auto now = std::chrono::system_clock::now();
        auto timestamp = std::chrono::duration_cast<std::chrono::seconds>(
            now.time_since_epoch()
        ).count();
        health["server_time"] = timestamp;
        health["today"] = getTodayDate();
        health["tomorrow"] = getDateWithOffset(1);
        health["api_calls_today"] = api_calls_today;

        res.set_header("Access-Control-Allow-Origin", "*");
        res.set_header("Content-Type", "application/json");
        res.set_content(health.dump(2), "application/json");
        });

    std::cout << "\n🚀 Server running on Port 8080..." << std::endl;
    std::cout << "📡 Endpoints:" << std::endl;
    std::cout << "   GET /         - Health check" << std::endl;
    std::cout << "   GET /scores   - All matches with smart updates" << std::endl;
    std::cout << "   GET /health   - Server status + API usage" << std::endl;
    std::cout << "\n✨ Smart engine ready!\n" << std::endl;

    svr.listen("0.0.0.0", 8080);

    return 0;
}