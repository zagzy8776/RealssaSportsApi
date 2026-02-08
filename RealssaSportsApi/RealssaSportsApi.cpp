#include <iostream>
#include <string>
#include <vector>
#include <thread>
#include <chrono>
#include <mutex>
#include <cpr/cpr.h>
#include <nlohmann/json.hpp>
#include <httplib.h>

using json = nlohmann::json;

// Thread-safe storage for Vercel
std::string latest_match_data = "{\"status\": \"initializing\"}";
std::mutex data_mutex;

struct Match {
    std::string homeTeam, awayTeam, league;
    int homeScore, awayScore;
    std::string startingTime;
};

void processLiveMatches(const std::string& apiToken) {
    while (true) {
        std::cout << "\n--- Realssa Engine: Fetching Sportmonks Live Updates ---" << std::endl;

        // Using the Sportmonks V3 endpoint with the includes you specified
        std::string url = "https://api.sportmonks.com/v3/football/livescores/inplay";

        cpr::Response r = cpr::Get(
            cpr::Url{ url },
            cpr::Parameters{
                {"api_token", apiToken},
                {"include", "participants;scores;league"}
            }
        );

        if (r.status_code == 200) {
            try {
                auto data = json::parse(r.text);

                // 1. Update global string for the Vercel API
                {
                    std::lock_guard<std::mutex> lock(data_mutex);
                    latest_match_data = r.text;
                }

                // 2. Process for console visibility (Your original logic style)
                if (data.contains("data") && data["data"].is_array()) {
                    for (auto& item : data["data"]) {
                        Match m;
                        m.league = item["league"]["name"];
                        m.startingTime = item["starting_at"];

                        // Sportmonks puts participants in an array, usually [0] is home, [1] is away
                        if (item["participants"].size() >= 2) {
                            m.homeTeam = item["participants"][0]["name"];
                            m.awayTeam = item["participants"][1]["name"];
                        }

                        // Filtering important leagues just like you wanted
                        if (m.league == "Premier League" || m.league == "NPFL") {
                            std::cout << "[IMPORTANT] ";
                        }

                        std::cout << "[" << m.league << "] " << m.homeTeam << " vs " << m.awayTeam
                            << " | Time: " << m.startingTime << std::endl;
                    }
                }
                else {
                    std::cout << "No matches currently in-play." << std::endl;
                }
            }
            catch (const std::exception& e) {
                std::cerr << "Parsing Error: " << e.what() << std::endl;
            }
        }
        else {
            std::cerr << "Sportmonks API Error: " << r.status_code << " - " << r.text << std::endl;
        }

        // Delay to respect rate limits (Sportmonks is generous, but 2-5 mins is safer than 15)
        std::cout << "Waiting 5 minutes for next sync..." << std::endl;
        std::this_thread::sleep_for(std::chrono::minutes(5));
    }
}

int main() {
    // Hidden in Environment Variables for Railway safety
    const char* envKey = std::getenv("SPORTMONKS_TOKEN");
    std::string myToken = envKey ? envKey : "ALtscQqBIKoL0vsLPO8ELYiQ19V90d6phTZbFBeWO6r7Uqk1Q86Op4lCOo8D";

    // 1. Start fetching in background
    std::thread worker(processLiveMatches, myToken);
    worker.detach();

    // 2. Setup the Server for Vercel
    httplib::Server svr;

    svr.Get("/scores", [](const httplib::Request&, httplib::Response& res) {
        std::lock_guard<std::mutex> lock(data_mutex);
        res.set_header("Access-Control-Allow-Origin", "*");
        res.set_content(latest_match_data, "application/json");
        });

    svr.Get("/", [](const httplib::Request&, httplib::Response& res) {
        res.set_content("Realssa Sportmonks Engine is Online", "text/plain");
        });

    std::cout << "Realssa API listening on port 8080..." << std::endl;
    svr.listen("0.0.0.0", 8080);

    return 0;
}