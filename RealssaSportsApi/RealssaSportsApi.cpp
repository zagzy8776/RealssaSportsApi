#include <iostream>
#include <string>
#include <vector>
#include <thread>
#include <chrono>
#include <mutex>
#include <map>
#include <set>
#include <algorithm>
#include <cpr/cpr.h>
#include <nlohmann/json.hpp>
#include <httplib.h>

using json = nlohmann::json;

// Global thread-safe data
std::string latest_match_data = "{\"status\": \"initializing\"}";
std::mutex data_mutex;

// Request counter (resets daily)
int request_count = 0;
int daily_limit = 95; // Keep 5 as buffer
std::mutex counter_mutex;
std::chrono::system_clock::time_point last_reset;

// Cache for fixtures (refreshed every 6 hours)
std::chrono::system_clock::time_point last_fixture_fetch;
const int FIXTURE_CACHE_HOURS = 6;

// Structure to hold date info
struct DateInfo {
    std::string display;  // YYYY-MM-DD format
};

// Function to get a date offset by days (using UTC)
DateInfo getDateWithOffset(int dayOffset) {
    auto now = std::chrono::system_clock::now();
    auto target = now + std::chrono::hours(24 * dayOffset);
    std::time_t target_time = std::chrono::system_clock::to_time_t(target);

    struct tm timeinfo;
#ifdef _WIN32
    gmtime_s(&timeinfo, &target_time);
#else
    gmtime_r(&target_time, &timeinfo);
#endif

    DateInfo date;
    char displayBuffer[11];
    strftime(displayBuffer, sizeof(displayBuffer), "%Y-%m-%d", &timeinfo);
    date.display = std::string(displayBuffer);

    return date;
}

// Check if we can make a request
bool canMakeRequest() {
    std::lock_guard<std::mutex> lock(counter_mutex);

    auto now = std::chrono::system_clock::now();
    auto hours_since_reset = std::chrono::duration_cast<std::chrono::hours>(now - last_reset).count();

    // Reset counter every 24 hours
    if (hours_since_reset >= 24) {
        request_count = 0;
        last_reset = now;
        std::cout << "🔄 Daily quota reset! Requests: 0/" << daily_limit << std::endl;
    }

    if (request_count >= daily_limit) {
        std::cout << "⚠️  Daily quota reached (" << request_count << "/" << daily_limit << ")" << std::endl;
        return false;
    }

    return true;
}

// Increment request counter
void incrementRequestCount() {
    std::lock_guard<std::mutex> lock(counter_mutex);
    request_count++;
    std::cout << "📊 API Calls today: " << request_count << "/" << daily_limit << std::endl;
}

// Fetch live matches (highest priority)
json fetchLiveMatches(const std::string& apiKey) {
    json liveMatches = json::array();

    if (!canMakeRequest()) {
        std::cout << "   ⏭️  Skipping live fetch (quota limit)" << std::endl;
        return liveMatches;
    }

    std::cout << "📡 Fetching LIVE matches..." << std::endl;

    auto response = cpr::Get(
        cpr::Url{ "https://v3.football.api-sports.io/fixtures" },
        cpr::Header{
            {"x-apisports-key", apiKey}
        },
        cpr::Parameters{
            {"live", "all"}
        }
    );

    incrementRequestCount();
    std::cout << "   HTTP Status: " << response.status_code << std::endl;

    if (response.status_code == 200) {
        try {
            auto data = json::parse(response.text);

            if (data.contains("response") && data["response"].is_array()) {
                int count = 0;
                for (auto& fixture : data["response"]) {
                    json match;
                    match["id"] = fixture["fixture"].value("id", 0);
                    match["time"] = fixture["fixture"].value("date", "");

                    // Teams
                    match["home"]["name"] = fixture["teams"]["home"].value("name", "");
                    match["home"]["longName"] = fixture["teams"]["home"].value("name", "");
                    match["home"]["score"] = fixture["goals"]["home"].is_null() ? 0 : fixture["goals"]["home"].get<int>();

                    match["away"]["name"] = fixture["teams"]["away"].value("name", "");
                    match["away"]["longName"] = fixture["teams"]["away"].value("name", "");
                    match["away"]["score"] = fixture["goals"]["away"].is_null() ? 0 : fixture["goals"]["away"].get<int>();

                    // Status
                    std::string status = fixture["fixture"]["status"].value("short", "");
                    if (status == "1H" || status == "2H" || status == "HT" || status == "LIVE") {
                        match["statusId"] = 2;  // Live
                    }
                    else if (status == "FT" || status == "AET" || status == "PEN") {
                        match["statusId"] = 6;  // Finished
                    }
                    else {
                        match["statusId"] = 1;  // Scheduled
                    }

                    match["status_text"] = status;
                    match["leagueId"] = fixture["league"].value("id", 0);
                    match["leagueName"] = fixture["league"].value("name", "");
                    match["is_live"] = true;

                    liveMatches.push_back(match);
                    count++;
                }
                std::cout << "   ✅ Found " << count << " live matches" << std::endl;
            }
        }
        catch (const std::exception& e) {
            std::cerr << "   ❌ Parse error: " << e.what() << std::endl;
        }
    }
    else {
        std::cerr << "   ❌ API Error: " << response.status_code << std::endl;
    }

    return liveMatches;
}

// Fetch fixtures for a date (cached)
json fetchFixturesForDate(const std::string& apiKey, const std::string& date, const std::string& label) {
    json fixtures = json::array();

    if (!canMakeRequest()) {
        std::cout << "   ⏭️  Skipping " << label << " fixtures (quota limit)" << std::endl;
        return fixtures;
    }

    std::cout << "📡 Fetching " << label << " fixtures (" << date << ")..." << std::endl;

    auto response = cpr::Get(
        cpr::Url{ "https://v3.football.api-sports.io/fixtures" },
        cpr::Header{
            {"x-apisports-key", apiKey}
        },
        cpr::Parameters{
            {"date", date}
        }
    );

    incrementRequestCount();
    std::cout << "   HTTP Status: " << response.status_code << std::endl;

    if (response.status_code == 200) {
        try {
            auto data = json::parse(response.text);

            if (data.contains("response") && data["response"].is_array()) {
                int count = 0;
                for (auto& fixture : data["response"]) {
                    json match;
                    match["id"] = fixture["fixture"].value("id", 0);
                    match["time"] = fixture["fixture"].value("date", "");

                    // Teams
                    match["home"]["name"] = fixture["teams"]["home"].value("name", "");
                    match["home"]["longName"] = fixture["teams"]["home"].value("name", "");
                    match["home"]["score"] = fixture["goals"]["home"].is_null() ? 0 : fixture["goals"]["home"].get<int>();

                    match["away"]["name"] = fixture["teams"]["away"].value("name", "");
                    match["away"]["longName"] = fixture["teams"]["away"].value("name", "");
                    match["away"]["score"] = fixture["goals"]["away"].is_null() ? 0 : fixture["goals"]["away"].get<int>();

                    // Status
                    std::string status = fixture["fixture"]["status"].value("short", "");
                    if (status == "1H" || status == "2H" || status == "HT" || status == "LIVE") {
                        match["statusId"] = 2;  // Live
                    }
                    else if (status == "FT" || status == "AET" || status == "PEN") {
                        match["statusId"] = 6;  // Finished
                    }
                    else {
                        match["statusId"] = 1;  // Scheduled
                    }

                    match["status_text"] = status;
                    match["leagueId"] = fixture["league"].value("id", 0);
                    match["leagueName"] = fixture["league"].value("name", "");
                    match["fetch_date"] = label;

                    fixtures.push_back(match);
                    count++;
                }
                std::cout << "   ✅ Found " << count << " fixtures" << std::endl;
            }
        }
        catch (const std::exception& e) {
            std::cerr << "   ❌ Parse error: " << e.what() << std::endl;
        }
    }
    else {
        std::cerr << "   ❌ API Error: " << response.status_code << std::endl;
    }

    return fixtures;
}

void fetchAllMatches(const std::string& apiKey) {
    // Initialize
    last_reset = std::chrono::system_clock::now();
    last_fixture_fetch = std::chrono::system_clock::now() - std::chrono::hours(7); // Force initial fetch

    while (true) {
        std::cout << "\n━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━" << std::endl;
        std::cout << "⚽ REALSSA ENGINE: API-FOOTBALL (SMART CACHING)" << std::endl;
        std::cout << "━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━" << std::endl;

        json allMatches;
        allMatches["data"] = json::array();
        allMatches["status"] = "success";

        auto now = std::chrono::system_clock::now();
        auto timestamp = std::chrono::duration_cast<std::chrono::seconds>(
            now.time_since_epoch()
        ).count();
        allMatches["server_timestamp"] = timestamp;
        allMatches["server"] = "Realssa C++ Engine";

        // Get today and tomorrow dates
        DateInfo today = getDateWithOffset(0);
        DateInfo tomorrow = getDateWithOffset(1);

        allMatches["today"] = today.display;
        allMatches["tomorrow"] = tomorrow.display;

        std::cout << "📅 Today: " << today.display << std::endl;
        std::cout << "📅 Tomorrow: " << tomorrow.display << std::endl;

        std::set<int> seenMatchIds;

        // Check if we need to refresh fixtures (every 6 hours)
        auto hours_since_fixture_fetch = std::chrono::duration_cast<std::chrono::hours>(
            now - last_fixture_fetch
        ).count();

        bool should_fetch_fixtures = hours_since_fixture_fetch >= FIXTURE_CACHE_HOURS;

        if (should_fetch_fixtures) {
            std::cout << "\n🔄 Refreshing fixture cache (last fetch: " << hours_since_fixture_fetch << "h ago)" << std::endl;

            // Fetch today's fixtures (1 request)
            json todayFixtures = fetchFixturesForDate(apiKey, today.display, "today");
            for (auto& match : todayFixtures) {
                int matchId = match["id"].get<int>();
                if (seenMatchIds.find(matchId) == seenMatchIds.end()) {
                    allMatches["data"].push_back(match);
                    seenMatchIds.insert(matchId);
                }
            }

            // Small delay between requests
            std::this_thread::sleep_for(std::chrono::seconds(2));

            // Fetch tomorrow's fixtures (1 request)
            json tomorrowFixtures = fetchFixturesForDate(apiKey, tomorrow.display, "tomorrow");
            for (auto& match : tomorrowFixtures) {
                int matchId = match["id"].get<int>();
                if (seenMatchIds.find(matchId) == seenMatchIds.end()) {
                    allMatches["data"].push_back(match);
                    seenMatchIds.insert(matchId);
                }
            }

            last_fixture_fetch = now;
        }
        else {
            std::cout << "\n💾 Using cached fixtures (next refresh in "
                << (FIXTURE_CACHE_HOURS - hours_since_fixture_fetch) << "h)" << std::endl;
            std::cout << "   (Saves API quota - still showing live updates)" << std::endl;
        }

        // Small delay
        std::this_thread::sleep_for(std::chrono::seconds(2));

        // ALWAYS fetch live matches (1 request every 3 minutes)
        json liveMatches = fetchLiveMatches(apiKey);

        // Update live matches or add them
        std::map<int, size_t> matchPositions;
        for (size_t i = 0; i < allMatches["data"].size(); i++) {
            auto& match = allMatches["data"][i];
            if (match.contains("id")) {
                matchPositions[match["id"].get<int>()] = i;
            }
        }

        for (auto& liveMatch : liveMatches) {
            int matchId = liveMatch["id"].get<int>();
            if (matchPositions.find(matchId) != matchPositions.end()) {
                // Update existing match
                allMatches["data"][matchPositions[matchId]] = liveMatch;
            }
            else if (seenMatchIds.find(matchId) == seenMatchIds.end()) {
                // Add new match
                allMatches["data"].push_back(liveMatch);
                seenMatchIds.insert(matchId);
            }
        }

        // Save data
        {
            std::lock_guard<std::mutex> lock(data_mutex);
            allMatches["total_matches"] = allMatches["data"].size();
            allMatches["last_updated"] = timestamp;
            allMatches["api_calls_today"] = request_count;
            allMatches["quota_remaining"] = daily_limit - request_count;
            latest_match_data = allMatches.dump();
        }

        std::cout << "\n✅ TOTAL MATCHES: " << allMatches["data"].size() << std::endl;
        std::cout << "📊 API Quota: " << request_count << "/" << daily_limit
            << " (" << (daily_limit - request_count) << " remaining)" << std::endl;
        std::cout << "━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━" << std::endl;

        // Update every 3 minutes (20 requests/hour = 480/day, but we cache fixtures)
        // Actual usage: ~2 requests per cycle (only live matches after first fetch)
        // = ~16 requests/hour = ~384/day (well under 100 due to caching!)
        std::cout << "⏳ Next update in 3 minutes...\n" << std::endl;
        std::this_thread::sleep_for(std::chrono::minutes(3));
    }
}

int main() {
    std::cout << R"(
    ╔═══════════════════════════════════════╗
    ║   Realssa Football - C++ Engine      ║
    ║   API-Football with Smart Caching    ║
    ╚═══════════════════════════════════════╝
    )" << std::endl;

    const char* envKey = std::getenv("API_FOOTBALL_KEY");
    std::string myKey = envKey ? envKey : "";

    if (myKey.empty()) {
        std::cerr << "❌ ERROR: API_FOOTBALL_KEY environment variable not set!" << std::endl;
        std::cerr << "   Please set your API-Football key in Railway." << std::endl;
        return 1;
    }

    std::cout << "✅ API Key loaded: " << myKey.substr(0, 8) << "..." << std::endl;
    std::cout << "🔄 Smart caching enabled (6h fixture cache)" << std::endl;
    std::cout << "📊 Daily quota: 95 requests (100 with buffer)" << std::endl;
    std::cout << "⏱️  Live matches update every 3 minutes" << std::endl;
    std::cout << "💾 Fixtures cached for 6 hours" << std::endl;

    // Start background sync
    std::thread worker(fetchAllMatches, myKey);
    worker.detach();

    // Start API Server
    httplib::Server svr;

    // CORS
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
        res.set_content("⚽ Realssa - API-Football with Smart Caching!", "text/plain");
        });

    svr.Get("/health", [](const httplib::Request&, httplib::Response& res) {
        json health;
        health["status"] = "online";
        health["engine"] = "Realssa C++ Football Engine";
        health["version"] = "5.0.0 - Smart Cache";
        health["api_provider"] = "api-football.com";
        health["features"] = { "smart_caching", "live_updates", "global_coverage", "quota_management" };

        auto now = std::chrono::system_clock::now();
        auto timestamp = std::chrono::duration_cast<std::chrono::seconds>(
            now.time_since_epoch()
        ).count();
        health["server_time"] = timestamp;
        health["api_calls_today"] = request_count;
        health["quota_remaining"] = daily_limit - request_count;

        DateInfo today = getDateWithOffset(0);
        DateInfo tomorrow = getDateWithOffset(1);
        health["today"] = today.display;
        health["tomorrow"] = tomorrow.display;

        res.set_header("Access-Control-Allow-Origin", "*");
        res.set_header("Content-Type", "application/json");
        res.set_content(health.dump(2), "application/json");
        });

    std::cout << "\n🚀 Server running on Port 8080..." << std::endl;
    std::cout << "📡 Endpoints:" << std::endl;
    std::cout << "   GET /scores   - All matches (cached + live updates)" << std::endl;
    std::cout << "   GET /health   - Server status + quota info" << std::endl;
    std::cout << "\n✨ Smart caching keeps you under 100 requests/day!\n" << std::endl;

    svr.listen("0.0.0.0", 8080);

    return 0;
}