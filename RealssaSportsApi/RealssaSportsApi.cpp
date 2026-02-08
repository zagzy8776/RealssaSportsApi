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

using json = nlohmann::json;

// QUOTA PROTECTION - CRITICAL SETTINGS
const int MAX_API_CALLS_PER_DAY = 450;  // Safety limit (out of 500 free tier)
const int CACHE_DURATION_HOURS = 3;      // Cache data for 3 hours minimum
const int POLL_INTERVAL_NO_LIVE = 60;    // 60 minutes when no live matches
const int POLL_INTERVAL_WITH_LIVE = 10;  // 10 minutes when live matches exist

// Global state
json cached_fixtures;
std::chrono::system_clock::time_point last_fixture_fetch;
std::mutex cache_mutex;
std::mutex counter_mutex;
int api_calls_today = 0;
int live_match_count = 0;
std::chrono::system_clock::time_point last_reset_date;

// Cross-platform date handling
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

// Check if we need to reset daily counter
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

// Check if we can make an API call (QUOTA PROTECTION)
bool canMakeApiCall() {
    std::lock_guard<std::mutex> lock(counter_mutex);
    if (api_calls_today >= MAX_API_CALLS_PER_DAY) {
        std::cerr << "\n⚠️  QUOTA LIMIT REACHED! (" << api_calls_today << "/" << MAX_API_CALLS_PER_DAY << ")" << std::endl;
        std::cerr << "⏸️  Skipping API call to protect quota" << std::endl;
        return false;
    }
    return true;
}

// Helper function to make RapidAPI requests with quota protection
cpr::Response makeRapidApiRequest(const std::string& endpoint, const std::string& apiKey) {
    if (!canMakeApiCall()) {
        // Return empty response if quota exceeded
        return cpr::Response{};
    }

    auto response = cpr::Get(
        cpr::Url{ "https://sportapi7.p.rapidapi.com" + endpoint },
        cpr::Header{
            {"x-rapidapi-host", "sportapi7.p.rapidapi.com"},
            {"x-rapidapi-key", apiKey}
        },
        cpr::Timeout{ 10000 }  // 10 second timeout
    );

    // Only increment if we got a response
    if (response.status_code != 0) {
        std::lock_guard<std::mutex> lock(counter_mutex);
        api_calls_today++;
        std::cout << "   📊 API Calls: " << api_calls_today << "/" << MAX_API_CALLS_PER_DAY << std::endl;
    }

    return response;
}

// Format match data from SportAPI7 response
json formatMatch(const json& event, const std::string& dateLabel) {
    json match;

    try {
        // Extract basic info
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

        // Score
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

            // Map status codes
            if (statusCode == 100 || statusType == "inprogress") {
                match["statusId"] = 2; // Live
                match["statusName"] = "LIVE";
            }
            else if (statusCode == 120 || statusCode == 110 || statusType == "finished") {
                match["statusId"] = 6; // Finished
                match["statusName"] = "FT";
            }
            else if (statusCode == 31) {
                match["statusId"] = 2;
                match["statusName"] = "HT"; // Half Time
            }
            else {
                match["statusId"] = 1; // Scheduled
                match["statusName"] = "VS";
            }

            // Add time info
            if (event["status"].contains("description")) {
                match["time"] = event["status"]["description"];
            }
        }
        else {
            match["statusId"] = 1;
            match["statusName"] = "VS";
        }

        // League info
        if (event.contains("tournament") && event["tournament"].is_object()) {
            match["league"] = event["tournament"].value("name", "");
            match["league_country"] = event["tournament"].value("category", json::object()).value("name", "");
            if (event["tournament"].contains("id")) {
                match["league_logo"] = "https://img.sofascore.com/api/v1/unique-tournament/" +
                    std::to_string(event["tournament"]["id"].get<int>()) + "/image";
            }
        }

        // Start time (Unix timestamp)
        if (event.contains("startTimestamp")) {
            match["timestamp"] = event["startTimestamp"];
        }

    }
    catch (const std::exception& e) {
        std::cerr << "⚠️  Error formatting match: " << e.what() << std::endl;
    }

    return match;
}

// Refresh fixture cache - ULTRA CONSERVATIVE VERSION
void refreshFixtureCache(const std::string& apiKey) {
    checkAndResetDailyCounter();

    auto now = std::chrono::system_clock::now();
    auto hours_since_fetch = std::chrono::duration_cast<std::chrono::hours>(
        now - last_fixture_fetch
    ).count();

    // STRICT CACHING: Only refresh if cache is old enough
    if (hours_since_fetch < CACHE_DURATION_HOURS && !cached_fixtures.empty()) {
        std::cout << "💾 Using cached fixtures (last fetch: " << hours_since_fetch << "h ago, expires in "
            << (CACHE_DURATION_HOURS - hours_since_fetch) << "h)" << std::endl;
        return;
    }

    // QUOTA CHECK before fetching
    if (!canMakeApiCall()) {
        std::cout << "⏸️  Skipping cache refresh - quota protection active" << std::endl;
        return;
    }

    std::cout << "\n🔄 Refreshing fixture cache..." << std::endl;

    json allFixtures;
    allFixtures["data"] = json::array();

    std::string today = getTodayDate();

    // ENDPOINT 1: Live Events (MOST IMPORTANT)
    std::cout << "📡 Fetching LIVE events..." << std::endl;
    auto liveResponse = makeRapidApiRequest("/api/v1/sport/football/events/live", apiKey);

    if (liveResponse.status_code == 200) {
        try {
            auto data = json::parse(liveResponse.text);

            if (data.contains("events") && data["events"].is_array()) {
                std::cout << "   ✅ Found " << data["events"].size() << " live matches" << std::endl;

                for (auto& event : data["events"]) {
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
        std::cerr << "   ❌ RATE LIMIT HIT - Stopping all requests!" << std::endl;
        api_calls_today = MAX_API_CALLS_PER_DAY; // Block further calls
        return;
    }

    // Small delay between requests
    std::this_thread::sleep_for(std::chrono::seconds(2));

    // ENDPOINT 2: Scheduled Events for Today (ONLY if we have quota left)
    if (canMakeApiCall()) {
        std::cout << "📡 Fetching scheduled events for today..." << std::endl;
        auto scheduledResponse = makeRapidApiRequest(
            "/api/v1/sport/football/scheduled-events/" + today,
            apiKey
        );

        if (scheduledResponse.status_code == 200) {
            try {
                auto data = json::parse(scheduledResponse.text);

                if (data.contains("events") && data["events"].is_array()) {
                    std::cout << "   ✅ Found " << data["events"].size() << " scheduled matches" << std::endl;

                    for (auto& event : data["events"]) {
                        allFixtures["data"].push_back(formatMatch(event, "today"));
                    }
                }
            }
            catch (const std::exception& e) {
                std::cerr << "   ❌ Parse error: " << e.what() << std::endl;
            }
        }
    }

    {
        std::lock_guard<std::mutex> lock(cache_mutex);
        cached_fixtures = allFixtures;
        last_fixture_fetch = now;
    }

    std::cout << "✅ Cache refreshed: " << allFixtures["data"].size() << " total matches" << std::endl;
    std::cout << "💾 Next refresh in " << CACHE_DURATION_HOURS << " hours" << std::endl;
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

// Background polling thread - ULTRA CONSERVATIVE
void backgroundPoller(const std::string& apiKey) {
    while (true) {
        refreshFixtureCache(apiKey);

        int liveMatches = countLiveMatches();

        {
            std::lock_guard<std::mutex> lock(counter_mutex);
            live_match_count = liveMatches;
        }

        // CONSERVATIVE POLLING: Long intervals to protect quota
        int pollInterval = (liveMatches > 0) ? POLL_INTERVAL_WITH_LIVE : POLL_INTERVAL_NO_LIVE;

        std::cout << "\n📊 Quota Status:" << std::endl;
        std::cout << "   API Calls: " << api_calls_today << "/" << MAX_API_CALLS_PER_DAY << std::endl;
        std::cout << "   Live Matches: " << liveMatches << std::endl;
        std::cout << "   ⏳ Next poll: " << pollInterval << " minutes" << std::endl;

        std::this_thread::sleep_for(std::chrono::minutes(pollInterval));
    }
}

int main() {
    crow::SimpleApp app;

    // Get API key from environment
    const char* api_key_env = std::getenv("RAPIDAPI_KEY");
    if (!api_key_env) {
        std::cerr << "❌ ERROR: RAPIDAPI_KEY environment variable not set!" << std::endl;
        return 1;
    }

    std::string api_key = api_key_env;
    std::cout << "✅ API Key: " << api_key.substr(0, 10) << "..." << std::endl;

    // Initialize cache and counters
    cached_fixtures["data"] = json::array();
    last_fixture_fetch = std::chrono::system_clock::now() - std::chrono::hours(CACHE_DURATION_HOURS + 1);
    last_reset_date = std::chrono::system_clock::now();

    std::cout << "\n⚡ QUOTA PROTECTION ACTIVE" << std::endl;
    std::cout << "   Max API Calls: " << MAX_API_CALLS_PER_DAY << "/day" << std::endl;
    std::cout << "   Cache Duration: " << CACHE_DURATION_HOURS << " hours" << std::endl;
    std::cout << "   Poll Interval (No Live): " << POLL_INTERVAL_NO_LIVE << " min" << std::endl;
    std::cout << "   Poll Interval (Live): " << POLL_INTERVAL_WITH_LIVE << " min" << std::endl;

    // Start background polling
    std::thread poller(backgroundPoller, api_key);
    poller.detach();

    // Health check endpoint
    CROW_ROUTE(app, "/")
        ([]() {
        crow::response res;
        res.set_header("Content-Type", "application/json");
        res.set_header("Access-Control-Allow-Origin", "*");
        res.write("{\n  \"message\": \"RealSSA Sports API - Quota Protected\",\n  \"status\": \"online\",\n  \"version\": \"9.0.0 - Ultra Safe\"\n}");
        return res;
            });

    // Scores endpoint
    CROW_ROUTE(app, "/scores")
        ([&api_key]() {
        // Check if cache needs refresh (but respect quota limits)
        auto now = std::chrono::system_clock::now();
        auto hours_since = std::chrono::duration_cast<std::chrono::hours>(
            now - last_fixture_fetch
        ).count();

        // Only force refresh if cache is VERY old and we have quota
        if (hours_since >= CACHE_DURATION_HOURS && canMakeApiCall()) {
            refreshFixtureCache(api_key);
        }

        json response;
        {
            std::lock_guard<std::mutex> lock(cache_mutex);
            response = cached_fixtures;
        }

        // Add metadata
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

    // Health endpoint with detailed stats
    CROW_ROUTE(app, "/health")
        ([]() {
        json health;
        health["status"] = "online";
        health["engine"] = "RealSSA Quota-Protected Engine";
        health["api_provider"] = "sportapi7.p.rapidapi.com";
        health["version"] = "9.0.0 - Ultra Safe";
        health["today"] = getTodayDate();

        // Quota information
        health["quota"] = {
            {"calls_today", api_calls_today},
            {"max_calls_per_day", MAX_API_CALLS_PER_DAY},
            {"calls_remaining", MAX_API_CALLS_PER_DAY - api_calls_today},
            {"quota_exhausted", api_calls_today >= MAX_API_CALLS_PER_DAY}
        };

        // Match stats
        health["matches"] = {
            {"live", live_match_count},
            {"total_cached", cached_fixtures.contains("data") ? cached_fixtures["data"].size() : 0}
        };

        // System info
        health["cache"] = {
            {"duration_hours", CACHE_DURATION_HOURS},
            {"poll_interval_no_live_min", POLL_INTERVAL_NO_LIVE},
            {"poll_interval_live_min", POLL_INTERVAL_WITH_LIVE}
        };

        health["server_time"] = std::chrono::system_clock::to_time_t(std::chrono::system_clock::now());
        health["features"] = { "ultra_conservative_quota", "smart_caching", "dynamic_polling", "auto_quota_reset" };

        crow::response res;
        res.set_header("Content-Type", "application/json");
        res.set_header("Access-Control-Allow-Origin", "*");
        res.write(health.dump(2));
        return res;
            });

    // Get port from environment or default to 8080
    const char* port_env = std::getenv("PORT");
    int port = port_env ? std::stoi(port_env) : 8080;

    std::cout << "\n🚀 Server starting on Port " << port << std::endl;

    app.port(port).multithreaded().run();

    return 0;
}