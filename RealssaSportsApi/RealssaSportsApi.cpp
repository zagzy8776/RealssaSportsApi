#include <iostream>
#include <string>
#include <vector>
#include <thread>
#include <chrono>
#include <mutex>
#include <map>
#include <algorithm>
#include <cpr/cpr.h>
#include <nlohmann/json.hpp>
#include <httplib.h>

using json = nlohmann::json;

// Global thread-safe data
std::string latest_match_data = "{\"status\": \"initializing\"}";
std::mutex data_mutex;

// Structure to hold date info
struct DateInfo {
    std::string display;  // YYYY-MM-DD format for display
    std::string api;      // YYYYMMDD format for API
    std::string label;    // "Today", "Tomorrow", etc.
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

    // Display format: YYYY-MM-DD
    char displayBuffer[11];
    strftime(displayBuffer, sizeof(displayBuffer), "%Y-%m-%d", &timeinfo);
    date.display = std::string(displayBuffer);

    // API format: YYYYMMDD
    char apiBuffer[9];
    strftime(apiBuffer, sizeof(apiBuffer), "%Y%m%d", &timeinfo);
    date.api = std::string(apiBuffer);

    // Label
    if (dayOffset == 0) date.label = "Today";
    else if (dayOffset == 1) date.label = "Tomorrow";
    else if (dayOffset == -1) date.label = "Yesterday";
    else date.label = std::to_string(dayOffset) + " days";

    return date;
}

void fetchAllMatches(const std::string& apiKey) {
    while (true) {
        std::cout << "\n━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━" << std::endl;
        std::cout << "⚽ REALSSA ENGINE: AUTO-FETCHING MATCHES" << std::endl;
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

        // Get today and tomorrow dates automatically
        DateInfo today = getDateWithOffset(0);
        DateInfo tomorrow = getDateWithOffset(1);

        allMatches["today"] = today.display;
        allMatches["tomorrow"] = tomorrow.display;

        std::cout << "📅 Today: " << today.display << " (API: " << today.api << ")" << std::endl;
        std::cout << "📅 Tomorrow: " << tomorrow.display << " (API: " << tomorrow.api << ")" << std::endl;

        // Map to track match IDs and avoid duplicates
        std::map<int, bool> seenMatchIds;

        // Fetch TODAY'S matches
        std::cout << "\n📡 Fetching TODAY'S matches..." << std::endl;
        auto todayRes = cpr::Get(
            cpr::Url{ "https://free-api-live-football-data.p.rapidapi.com/football-get-matches-by-date" },
            cpr::Header{
                {"x-rapidapi-key", apiKey},
                {"x-rapidapi-host", "free-api-live-football-data.p.rapidapi.com"}
            },
            cpr::Parameters{ {"date", today.api} }
        );

        std::cout << "   HTTP Status: " << todayRes.status_code << std::endl;

        if (todayRes.status_code == 200) {
            try {
                auto matchData = json::parse(todayRes.text);

                // Debug: Show response structure
                std::cout << "   Response keys: ";
                for (auto& el : matchData.items()) {
                    std::cout << el.key() << " ";
                }
                std::cout << std::endl;

                // Extract matches from response
                json matchesArray;
                bool found = false;

                if (matchData.contains("data") && matchData["data"].is_array()) {
                    matchesArray = matchData["data"];
                    found = true;
                }
                else if (matchData.contains("response")) {
                    if (matchData["response"].contains("matches") && matchData["response"]["matches"].is_array()) {
                        matchesArray = matchData["response"]["matches"];
                        found = true;
                    }
                    else if (matchData["response"].is_array()) {
                        matchesArray = matchData["response"];
                        found = true;
                    }
                }

                if (found && matchesArray.is_array()) {
                    std::cout << "   ✅ Found " << matchesArray.size() << " matches today" << std::endl;

                    for (auto& match : matchesArray) {
                        if (match.contains("id")) {
                            int matchId = match["id"].get<int>();
                            if (seenMatchIds.find(matchId) == seenMatchIds.end()) {
                                match["fetch_date"] = "today";
                                allMatches["data"].push_back(match);
                                seenMatchIds[matchId] = true;
                            }
                        }
                        else {
                            match["fetch_date"] = "today";
                            allMatches["data"].push_back(match);
                        }
                    }
                }
                else {
                    std::cout << "   ⚠️  No matches found in today's response" << std::endl;
                    std::cout << "   Sample: " << todayRes.text.substr(0, 200) << "..." << std::endl;
                }
            }
            catch (const std::exception& e) {
                std::cerr << "   ❌ Parse error: " << e.what() << std::endl;
            }
        }
        else {
            std::cerr << "   ❌ API Error: HTTP " << todayRes.status_code << std::endl;
        }

        // Fetch TOMORROW'S matches
        std::cout << "\n📡 Fetching TOMORROW'S matches..." << std::endl;
        auto tomorrowRes = cpr::Get(
            cpr::Url{ "https://free-api-live-football-data.p.rapidapi.com/football-get-matches-by-date" },
            cpr::Header{
                {"x-rapidapi-key", apiKey},
                {"x-rapidapi-host", "free-api-live-football-data.p.rapidapi.com"}
            },
            cpr::Parameters{ {"date", tomorrow.api} }
        );

        std::cout << "   HTTP Status: " << tomorrowRes.status_code << std::endl;

        if (tomorrowRes.status_code == 200) {
            try {
                auto matchData = json::parse(tomorrowRes.text);

                json matchesArray;
                bool found = false;

                if (matchData.contains("data") && matchData["data"].is_array()) {
                    matchesArray = matchData["data"];
                    found = true;
                }
                else if (matchData.contains("response")) {
                    if (matchData["response"].contains("matches") && matchData["response"]["matches"].is_array()) {
                        matchesArray = matchData["response"]["matches"];
                        found = true;
                    }
                    else if (matchData["response"].is_array()) {
                        matchesArray = matchData["response"];
                        found = true;
                    }
                }

                if (found && matchesArray.is_array()) {
                    std::cout << "   ✅ Found " << matchesArray.size() << " matches tomorrow" << std::endl;

                    for (auto& match : matchesArray) {
                        if (match.contains("id")) {
                            int matchId = match["id"].get<int>();
                            if (seenMatchIds.find(matchId) == seenMatchIds.end()) {
                                match["fetch_date"] = "tomorrow";
                                allMatches["data"].push_back(match);
                                seenMatchIds[matchId] = true;
                            }
                        }
                        else {
                            match["fetch_date"] = "tomorrow";
                            allMatches["data"].push_back(match);
                        }
                    }
                }
                else {
                    std::cout << "   ⚠️  No matches found in tomorrow's response" << std::endl;
                }
            }
            catch (const std::exception& e) {
                std::cerr << "   ❌ Parse error: " << e.what() << std::endl;
            }
        }
        else {
            std::cerr << "   ❌ API Error: HTTP " << tomorrowRes.status_code << std::endl;
        }

        // Fetch LIVE matches (always check)
        std::cout << "\n📡 Fetching LIVE matches..." << std::endl;
        auto liveRes = cpr::Get(
            cpr::Url{ "https://free-api-live-football-data.p.rapidapi.com/football-current-live" },
            cpr::Header{
                {"x-rapidapi-key", apiKey},
                {"x-rapidapi-host", "free-api-live-football-data.p.rapidapi.com"}
            }
        );

        if (liveRes.status_code == 200) {
            try {
                auto liveData = json::parse(liveRes.text);
                json liveMatchesArray = json::array();

                if (liveData.contains("data") && liveData["data"].is_array()) {
                    liveMatchesArray = liveData["data"];
                }
                else if (liveData.contains("response")) {
                    if (liveData["response"].contains("matches") && liveData["response"]["matches"].is_array()) {
                        liveMatchesArray = liveData["response"]["matches"];
                    }
                    else if (liveData["response"].is_array()) {
                        liveMatchesArray = liveData["response"];
                    }
                }

                if (liveMatchesArray.size() > 0) {
                    std::cout << "   ✅ Found " << liveMatchesArray.size() << " live matches" << std::endl;

                    // Update existing matches or add new ones
                    std::map<int, size_t> matchPositions;
                    for (size_t i = 0; i < allMatches["data"].size(); i++) {
                        auto& match = allMatches["data"][i];
                        if (match.contains("id")) {
                            matchPositions[match["id"].get<int>()] = i;
                        }
                    }

                    for (auto& liveMatch : liveMatchesArray) {
                        if (liveMatch.contains("id")) {
                            int matchId = liveMatch["id"].get<int>();
                            liveMatch["is_live"] = true;

                            if (matchPositions.find(matchId) != matchPositions.end()) {
                                // Update existing match with live data
                                allMatches["data"][matchPositions[matchId]] = liveMatch;
                            }
                            else {
                                // Add new live match
                                allMatches["data"].push_back(liveMatch);
                            }
                        }
                    }
                }
                else {
                    std::cout << "   ℹ️  No live matches currently" << std::endl;
                }
            }
            catch (const std::exception& e) {
                std::cerr << "   ❌ Live parse error: " << e.what() << std::endl;
            }
        }

        // Save the combined data
        {
            std::lock_guard<std::mutex> lock(data_mutex);
            allMatches["total_matches"] = allMatches["data"].size();
            allMatches["last_updated"] = timestamp;
            latest_match_data = allMatches.dump();
        }

        std::cout << "\n✅ TOTAL MATCHES: " << allMatches["data"].size() << std::endl;
        std::cout << "   (Today + Tomorrow + Live)" << std::endl;
        std::cout << "━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━" << std::endl;

        // Update every 2 minutes
        std::cout << "⏳ Next update in 2 minutes...\n" << std::endl;
        std::this_thread::sleep_for(std::chrono::minutes(2));
    }
}

int main() {
    std::cout << R"(
    ╔═══════════════════════════════════════╗
    ║   Realssa Football - C++ Engine      ║
    ║   Auto-Updating: Today & Tomorrow    ║
    ╚═══════════════════════════════════════╝
    )" << std::endl;

    const char* envKey = std::getenv("RAPID_FOOTBALL_KEY");
    std::string myKey = envKey ? envKey : "";

    if (myKey.empty()) {
        std::cerr << "❌ ERROR: RAPID_FOOTBALL_KEY environment variable not set!" << std::endl;
        std::cerr << "   Please set your RapidAPI key in Railway." << std::endl;
        return 1;
    }

    std::cout << "✅ API Key loaded successfully" << std::endl;
    std::cout << "🔄 Automatic date calculation enabled (UTC)" << std::endl;
    std::cout << "📅 System will automatically know today and tomorrow" << std::endl;

    // Start background sync
    std::thread worker(fetchAllMatches, myKey);
    worker.detach();

    // Start API Server
    httplib::Server svr;

    // CORS preflight handler
    svr.Options("/(.*)", [](const httplib::Request&, httplib::Response& res) {
        res.set_header("Access-Control-Allow-Origin", "*");
        res.set_header("Access-Control-Allow-Methods", "GET, OPTIONS");
        res.set_header("Access-Control-Allow-Headers", "Content-Type");
        res.status = 204;
        });

    svr.Get("/scores", [](const httplib::Request&, httplib::Response& res) {
        std::lock_guard<std::mutex> lock(data_mutex);
        res.set_header("Access-Control-Allow-Origin", "*");
        res.set_header("Access-Control-Allow-Methods", "GET, OPTIONS");
        res.set_header("Access-Control-Allow-Headers", "Content-Type");
        res.set_header("Content-Type", "application/json");
        res.set_content(latest_match_data, "application/json");
        });

    svr.Get("/", [](const httplib::Request&, httplib::Response& res) {
        res.set_header("Access-Control-Allow-Origin", "*");
        res.set_content("⚽ Realssa Football Engine - Auto-Updates Every 2 Minutes!", "text/plain");
        });

    svr.Get("/health", [](const httplib::Request&, httplib::Response& res) {
        json health;
        health["status"] = "online";
        health["engine"] = "Realssa C++ Football Engine";
        health["version"] = "3.0.0 - Auto Date";
        health["features"] = { "auto_today", "auto_tomorrow", "live_scores", "auto_updates" };

        auto now = std::chrono::system_clock::now();
        auto timestamp = std::chrono::duration_cast<std::chrono::seconds>(
            now.time_since_epoch()
        ).count();
        health["server_time"] = timestamp;

        DateInfo today = getDateWithOffset(0);
        DateInfo tomorrow = getDateWithOffset(1);
        health["today"] = today.display;
        health["tomorrow"] = tomorrow.display;

        res.set_header("Access-Control-Allow-Origin", "*");
        res.set_header("Content-Type", "application/json");
        res.set_content(health.dump(2), "application/json");
        });

    std::cout << "\n🚀 Realssa Backend running on Port 8080..." << std::endl;
    std::cout << "📡 Endpoints:" << std::endl;
    std::cout << "   - GET /scores   - Today's + Tomorrow's matches + Live" << std::endl;
    std::cout << "   - GET /health   - Server status with current dates" << std::endl;
    std::cout << "\n✨ Server will automatically update dates every day!\n" << std::endl;

    svr.listen("0.0.0.0", 8080);

    return 0;
}