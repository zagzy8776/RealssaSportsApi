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

// Function to get current date in YYYY-MM-DD format
std::string getTodayDate() {
    auto now = std::chrono::system_clock::now();
    std::time_t now_time = std::chrono::system_clock::to_time_t(now);
    struct tm timeinfo;
    localtime_s(&timeinfo, &now_time);
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
                std::cerr << "Merger Error: " << e.what() << std::endl;
            }
        }

        // Poll every 5 minutes (Safe for free tier limits)
        std::this_thread::sleep_for(std::chrono::minutes(5));
    }
}

int main() {
    const char* envKey = std::getenv("RAPID_FOOTBALL_KEY");
    std::string myKey = envKey ? envKey : "e1936a6aa9msh81b9aea3572a9cbp1503fcjsnaee4ac958e8c";

    // Start background sync
    std::thread worker(smartEngineLoop, myKey);
    worker.detach();

    // Start API Server for Vercel
    httplib::Server svr;
    svr.Get("/scores", [](const httplib::Request&, httplib::Response& res) {
        std::lock_guard<std::mutex> lock(data_mutex);
        res.set_header("Access-Control-Allow-Origin", "*");
        res.set_content(latest_merged_data, "application/json");
        });

    std::cout << "Realssa Backend running on Port 8080..." << std::endl;
    svr.listen("0.0.0.0", 8080);

    return 0;
}