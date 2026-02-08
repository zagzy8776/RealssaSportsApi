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
std::string latest_merged_data = "{\"status\": \"initializing\"}";
std::mutex data_mutex;

// Function to get current date in YYYY-MM-DD format (Cross-platform)
std::string getTodayDate() {
    auto now = std::chrono::system_clock::now();
    std::time_t now_time = std::chrono::system_clock::to_time_t(now);
    struct tm timeinfo;

    // Cross-platform: use localtime_r on Linux/Unix, localtime_s on Windows
#ifdef _WIN32
    localtime_s(&timeinfo, &now_time);
#else
    localtime_r(&now_time, &timeinfo);
#endif

    char buffer[11];
    strftime(buffer, sizeof(buffer), "%Y-%m-%d", &timeinfo);
    return std::string(buffer);
}

void smartEngineLoop(const std::string& apiKey) {
    while (true) {
        std::cout << "\n--- Realssa Smart Engine: Syncing Data ---" << std::endl;

        // 1. Fetch Today's Full Schedule (All leagues)
        auto scheduleRes = cpr::Get(
            cpr::Url{ "https://free-api-live-football-data.p.rapidapi.com/football-fixtures-day" },
            cpr::Header{ {"x-rapidapi-key", apiKey}, {"x-rapidapi-host", "free-api-live-football-data.p.rapidapi.com"} },
            cpr::Parameters{ {"date", getTodayDate()} }
        );

        // 2. Fetch Live Scores Only
        auto liveRes = cpr::Get(
            cpr::Url{ "https://free-api-live-football-data.p.rapidapi.com/football-current-live" },
            cpr::Header{ {"x-rapidapi-key", apiKey}, {"x-rapidapi-host", "free-api-live-football-data.p.rapidapi.com"} }
        );

        if (scheduleRes.status_code == 200 && liveRes.status_code == 200) {
            try {
                auto fullSchedule = json::parse(scheduleRes.text);
                auto liveData = json::parse(liveRes.text);

                // MERGER LOGIC: 
                // We map live scores into a lookup table by Match ID
                std::map<std::string, json> liveMap;
                for (auto& match : liveData["data"]) {
                    liveMap[match["id"].get<std::string>()] = match;
                }

                // Update the schedule with live data where available
                for (auto& match : fullSchedule["data"]) {
                    std::string id = match["id"].get<std::string>();
                    if (liveMap.count(id)) {
                        match["status"] = "LIVE";
                        match["goals"] = liveMap[id]["goals"]; // Update score
                    }
                    else {
                        match["status"] = "NS"; // Not Started
                    }
                }

                // Save to global string for Vercel
                std::lock_guard<std::mutex> lock(data_mutex);
                latest_merged_data = fullSchedule.dump();
                std::cout << "✅ Merged " << fullSchedule["data"].size() << " matches for today." << std::endl;

            }
            catch (const std::exception& e) {
                std::cerr << "❌ Merger Error: " << e.what() << std::endl;
            }
        }
        else {
            std::cerr << "❌ API Error - Schedule: " << scheduleRes.status_code
                << " | Live: " << liveRes.status_code << std::endl;
        }

        // Poll every 5 minutes (Safe for free tier limits)
        std::cout << "⏳ Waiting 5 minutes for next update..." << std::endl;
        std::this_thread::sleep_for(std::chrono::minutes(5));
    }
}

int main() {
    std::cout << R"(
    ╔═══════════════════════════════════════╗
    ║   Realssa Smart Engine - Football    ║
    ║   Live Scores & Schedule Tracker     ║
    ╚═══════════════════════════════════════╝
    )" << std::endl;

    const char* envKey = std::getenv("RAPID_FOOTBALL_KEY");
    std::string myKey = envKey ? envKey : "";

    if (myKey.empty()) {
        std::cerr << "❌ ERROR: RAPID_FOOTBALL_KEY environment variable not set!" << std::endl;
        std::cerr << "   Please set your RapidAPI key before running." << std::endl;
        return 1;
    }

    std::cout << "✅ API Key loaded successfully" << std::endl;
    std::cout << "📅 Today's date: " << getTodayDate() << std::endl;

    // Start background sync
    std::thread worker(smartEngineLoop, myKey);
    worker.detach();

    // Start API Server for Vercel
    httplib::Server svr;

    svr.Get("/scores", [](const httplib::Request&, httplib::Response& res) {
        std::lock_guard<std::mutex> lock(data_mutex);
        res.set_header("Access-Control-Allow-Origin", "*");
        res.set_header("Access-Control-Allow-Methods", "GET, OPTIONS");
        res.set_header("Access-Control-Allow-Headers", "Content-Type");
        res.set_content(latest_merged_data, "application/json");
        });

    svr.Get("/", [](const httplib::Request&, httplib::Response& res) {
        res.set_header("Access-Control-Allow-Origin", "*");
        res.set_content("⚽ Realssa Smart Engine is Online!", "text/plain");
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
    std::cout << "   - GET /scores   - Live match data" << std::endl;
    std::cout << "\n✨ Ready to serve live football scores!\n" << std::endl;

    svr.listen("0.0.0.0", 8080);

    return 0;
}