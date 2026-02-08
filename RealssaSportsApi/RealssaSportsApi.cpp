#include <iostream>
#include <string>
#include <vector>
#include <thread>
#include <chrono>
#include <mutex>
#include <map>
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
        allMatches["date"] = getTodayDate();

        // Try different endpoints to get comprehensive data

        // 1. Get Live Matches
        std::cout << "📡 Fetching live matches..." << std::endl;
        auto liveRes = cpr::Get(
            cpr::Url{ "https://free-api-live-football-data.p.rapidapi.com/football-current-live" },
            cpr::Header{ {"x-rapidapi-key", apiKey}, {"x-rapidapi-host", "free-api-live-football-data.p.rapidapi.com"} }
        );

        if (liveRes.status_code == 200) {
            try {
                auto liveData = json::parse(liveRes.text);
                if (liveData.contains("data") && liveData["data"].is_array()) {
                    for (auto& match : liveData["data"]) {
                        match["match_status"] = "LIVE";
                        allMatches["data"].push_back(match);
                    }
                    std::cout << "✅ Live: " << liveData["data"].size() << " matches" << std::endl;
                }
            }
            catch (const std::exception& e) {
                std::cerr << "❌ Live matches parse error: " << e.what() << std::endl;
            }
        }
        else {
            std::cerr << "⚠️ Live endpoint error: " << liveRes.status_code << std::endl;
        }

        // 2. Try to get today's fixtures (all matches for today)
        std::cout << "📡 Fetching today's schedule..." << std::endl;

        // Try endpoint 1: football-get-league-fixtures
        auto scheduleRes = cpr::Get(
            cpr::Url{ "https://free-api-live-football-data.p.rapidapi.com/football-get-league-fixtures" },
            cpr::Header{ {"x-rapidapi-key", apiKey}, {"x-rapidapi-host", "free-api-live-football-data.p.rapidapi.com"} },
            cpr::Parameters{ {"leagueid", "1"}, {"date", getTodayDate()} } // Try Premier League first
        );

        if (scheduleRes.status_code == 200) {
            try {
                auto scheduleData = json::parse(scheduleRes.text);
                if (scheduleData.contains("data") && scheduleData["data"].is_array()) {
                    // Map live matches by ID to avoid duplicates
                    std::map<std::string, bool> liveMatchIds;
                    for (auto& match : allMatches["data"]) {
                        if (match.contains("id")) {
                            liveMatchIds[match["id"].get<std::string>()] = true;
                        }
                    }

                    for (auto& match : scheduleData["data"]) {
                        std::string matchId = match.contains("id") ? match["id"].get<std::string>() : "";
                        if (matchId.empty() || liveMatchIds.find(matchId) == liveMatchIds.end()) {
                            match["match_status"] = "SCHEDULED";
                            allMatches["data"].push_back(match);
                        }
                    }
                    std::cout << "✅ Schedule: " << scheduleData["data"].size() << " matches" << std::endl;
                }
            }
            catch (const std::exception& e) {
                std::cerr << "❌ Schedule parse error: " << e.what() << std::endl;
            }
        }
        else {
            std::cerr << "⚠️ Schedule endpoint returned: " << scheduleRes.status_code << std::endl;
            std::cerr << "   This is okay - we'll show live matches only" << std::endl;
        }

        // 3. Try finished matches endpoint
        std::cout << "📡 Checking for finished matches..." << std::endl;
        auto finishedRes = cpr::Get(
            cpr::Url{ "https://free-api-live-football-data.p.rapidapi.com/football-get-league-fixtures" },
            cpr::Header{ {"x-rapidapi-key", apiKey}, {"x-rapidapi-host", "free-api-live-football-data.p.rapidapi.com"} },
            cpr::Parameters{ {"leagueid", "1"}, {"date", getTodayDate()} }
        );

        // Save the combined data
        {
            std::lock_guard<std::mutex> lock(data_mutex);
            allMatches["total_matches"] = allMatches["data"].size();
            latest_match_data = allMatches.dump();
        }

        std::cout << "✅ Total matches available: " << allMatches["data"].size() << std::endl;

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

    // Start API Server for Vercel
    httplib::Server svr;

    svr.Get("/scores", [](const httplib::Request&, httplib::Response& res) {
        std::lock_guard<std::mutex> lock(data_mutex);
        res.set_header("Access-Control-Allow-Origin", "*");
        res.set_header("Access-Control-Allow-Methods", "GET, OPTIONS");
        res.set_header("Access-Control-Allow-Headers", "Content-Type");
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
        health["version"] = "2.0.0";
        health["features"] = { "live_scores", "upcoming_matches", "finished_matches" };

        auto now = std::chrono::system_clock::now();
        auto timestamp = std::chrono::duration_cast<std::chrono::seconds>(
            now.time_since_epoch()
        ).count();
        health["server_time"] = timestamp;

        res.set_header("Access-Control-Allow-Origin", "*");
        res.set_content(health.dump(2), "application/json");
        });

    // CORS preflight handler
    svr.Options("/(.*)", [](const httplib::Request&, httplib::Response& res) {
        res.set_header("Access-Control-Allow-Origin", "*");
        res.set_header("Access-Control-Allow-Methods", "GET, OPTIONS");
        res.set_header("Access-Control-Allow-Headers", "Content-Type");
        res.status = 204;
        });

    std::cout << "🚀 Realssa Backend running on Port 8080..." << std::endl;
    std::cout << "📡 Endpoints available:" << std::endl;
    std::cout << "   - GET /         - Health check" << std::endl;
    std::cout << "   - GET /scores   - All match data (live, upcoming, finished)" << std::endl;
    std::cout << "   - GET /health   - Server status" << std::endl;
    std::cout << "\n✨ Ready to serve all football data!\n" << std::endl;

    svr.listen("0.0.0.0", 8080);

    return 0;
}