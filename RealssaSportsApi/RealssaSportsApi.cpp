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

// Function to convert YYYY-MM-DD to YYYYMMDD
std::string formatDateForAPI(const std::string& date) {
    std::string formatted = date;
    formatted.erase(std::remove(formatted.begin(), formatted.end(), '-'), formatted.end());
    return formatted;
}

void fetchAllMatches(const std::string& apiKey) {
    while (true) {
        std::cout << "\n--- ⚽ Realssa Engine: Fetching All Matches ---" << std::endl;

        json allMatches;
        allMatches["data"] = json::array();
        allMatches["status"] = "success";

        auto now = std::chrono::system_clock::now();
        auto timestamp = std::chrono::duration_cast<std::chrono::seconds>(
            now.time_since_epoch()
        ).count();
        allMatches["server_timestamp"] = timestamp;
        allMatches["server"] = "Realssa C++ Engine";

        std::string today = getTodayDate();
        std::string dateParam = formatDateForAPI(today);
        allMatches["date"] = today;

        std::cout << "📅 Fetching all matches for: " << today << " (API param: " << dateParam << ")" << std::endl;

        // ========== 1. Get ALL matches by date (live, upcoming, finished) ==========
        std::cout << "📡 Fetching all matches by date..." << std::endl;
        auto matchesRes = cpr::Get(
            cpr::Url{ "https://free-api-live-football-data.p.rapidapi.com/football-get-matches-by-date" },
            cpr::Header{
                {"x-rapidapi-key", apiKey},
                {"x-rapidapi-host", "free-api-live-football-data.p.rapidapi.com"}
            },
            cpr::Parameters{ {"date", dateParam} }
        );

        if (matchesRes.status_code == 200) {
            try {
                auto matchData = json::parse(matchesRes.text);

                // Handle different response structures
                if (matchData.contains("data") && matchData["data"].is_array()) {
                    allMatches["data"] = matchData["data"];
                    std::cout << "✅ Matches by date: " << matchData["data"].size() << " matches" << std::endl;
                }
                else if (matchData.contains("response")) {
                    // Check if response.matches exists (THIS WAS THE FIX!)
                    if (matchData["response"].contains("matches") && matchData["response"]["matches"].is_array()) {
                        allMatches["data"] = matchData["response"]["matches"];
                        std::cout << "✅ Matches by date: " << matchData["response"]["matches"].size() << " matches" << std::endl;
                    }
                    else if (matchData["response"].is_array()) {
                        allMatches["data"] = matchData["response"];
                        std::cout << "✅ Matches by date: " << matchData["response"].size() << " matches" << std::endl;
                    }
                    else {
                        std::cout << "⚠️ Unexpected 'response' structure" << std::endl;
                        std::cerr << "   Sample response: " << matchesRes.text.substr(0, 300) << "..." << std::endl;
                    }
                }
                else {
                    std::cout << "⚠️ No 'data' or 'response' in matches response" << std::endl;
                    std::cerr << "   Sample response: " << matchesRes.text.substr(0, 300) << "..." << std::endl;
                }
            }
            catch (const std::exception& e) {
                std::cerr << "❌ Parse error (matches by date): " << e.what() << std::endl;
                std::cerr << "   Response: " << matchesRes.text.substr(0, 300) << "..." << std::endl;
            }
        }
        else {
            std::cerr << "❌ API Error (matches by date): HTTP " << matchesRes.status_code << std::endl;
            std::cerr << "   Response: " << matchesRes.text.substr(0, 300) << "..." << std::endl;
        }

        // ========== 2. Get LIVE matches separately for real-time updates ==========
        std::cout << "📡 Fetching current live matches..." << std::endl;
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
                int liveCount = 0;
                json liveMatchesArray = json::array();

                // Extract live matches from different structures
                if (liveData.contains("data") && liveData["data"].is_array()) {
                    liveMatchesArray = liveData["data"];
                    liveCount = liveMatchesArray.size();
                }
                else if (liveData.contains("response")) {
                    // Also check for response.matches in live data
                    if (liveData["response"].contains("matches") && liveData["response"]["matches"].is_array()) {
                        liveMatchesArray = liveData["response"]["matches"];
                        liveCount = liveMatchesArray.size();
                    }
                    else if (liveData["response"].is_array()) {
                        liveMatchesArray = liveData["response"];
                        liveCount = liveMatchesArray.size();
                    }
                }

                if (liveCount > 0) {
                    // Create a map of existing match IDs to avoid duplicates
                    std::map<std::string, size_t> existingIds;
                    for (size_t i = 0; i < allMatches["data"].size(); i++) {
                        auto& match = allMatches["data"][i];
                        if (match.contains("id")) {
                            std::string id = match["id"].is_string() ?
                                match["id"].get<std::string>() :
                                std::to_string(match["id"].get<int>());
                            existingIds[id] = i;
                        }
                    }

                    // Merge or update with live data
                    for (auto& liveMatch : liveMatchesArray) {
                        if (liveMatch.contains("id")) {
                            std::string matchId = liveMatch["id"].is_string() ?
                                liveMatch["id"].get<std::string>() :
                                std::to_string(liveMatch["id"].get<int>());

                            if (existingIds.find(matchId) != existingIds.end()) {
                                // Update existing match with live data (more current scores)
                                allMatches["data"][existingIds[matchId]] = liveMatch;
                            }
                            else {
                                // Add new live match
                                allMatches["data"].push_back(liveMatch);
                            }
                        }
                        else {
                            // No ID, just add it
                            allMatches["data"].push_back(liveMatch);
                        }
                    }

                    std::cout << "✅ Live matches: " << liveCount << " matches" << std::endl;
                }
                else {
                    std::cout << "ℹ️  No live matches currently" << std::endl;
                }
            }
            catch (const std::exception& e) {
                std::cerr << "❌ Live parse error: " << e.what() << std::endl;
                std::cerr << "   Response: " << liveRes.text.substr(0, 300) << "..." << std::endl;
            }
        }
        else {
            std::cerr << "⚠️ Live endpoint error: HTTP " << liveRes.status_code << std::endl;
        }

        // ========== Save the combined data ==========
        {
            std::lock_guard<std::mutex> lock(data_mutex);
            allMatches["total_matches"] = allMatches["data"].size();
            allMatches["last_updated"] = timestamp;
            latest_match_data = allMatches.dump();
        }

        std::cout << "✅ Total matches available: " << allMatches["data"].size() << std::endl;
        std::cout << "━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━" << std::endl;

        // Poll every 2 minutes
        std::cout << "⏳ Waiting 2 minutes for next update..." << std::endl;
        std::this_thread::sleep_for(std::chrono::minutes(2));
    }
}

int main() {
    std::cout << R"(
    ╔═══════════════════════════════════════╗
    ║   Realssa Football - C++ Engine      ║
    ║   All Matches: Live, Upcoming, Past  ║
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
    std::cout << "📅 Today's date: " << getTodayDate() << std::endl;

    // Start background sync
    std::thread worker(fetchAllMatches, myKey);
    worker.detach();

    // Start API Server
    httplib::Server svr;

    // CORS preflight handler (must be before other routes)
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
        res.set_content("⚽ Realssa Football Engine is Online - All Matches Available!", "text/plain");
        });

    svr.Get("/health", [](const httplib::Request&, httplib::Response& res) {
        json health;
        health["status"] = "online";
        health["engine"] = "Realssa C++ Football Engine";
        health["version"] = "2.1.0";
        health["features"] = { "live_scores", "upcoming_matches", "finished_matches", "match_details" };

        auto now = std::chrono::system_clock::now();
        auto timestamp = std::chrono::duration_cast<std::chrono::seconds>(
            now.time_since_epoch()
        ).count();
        health["server_time"] = timestamp;
        health["server_date"] = getTodayDate();

        res.set_header("Access-Control-Allow-Origin", "*");
        res.set_header("Content-Type", "application/json");
        res.set_content(health.dump(2), "application/json");
        });

    std::cout << "\n🚀 Realssa Backend running on Port 8080..." << std::endl;
    std::cout << "📡 Endpoints available:" << std::endl;
    std::cout << "   - GET /         - Health check" << std::endl;
    std::cout << "   - GET /scores   - All match data (live, upcoming, finished)" << std::endl;
    std::cout << "   - GET /health   - Server status" << std::endl;
    std::cout << "\n✨ Ready to serve all football data!\n" << std::endl;

    svr.listen("0.0.0.0", 8080);

    return 0;
}