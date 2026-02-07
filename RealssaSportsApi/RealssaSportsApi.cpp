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

// Global storage so the server can access what the loop fetches
std::string latest_match_data = "{\"status\": \"initializing\"}";
std::mutex data_mutex;

// Track previous scores for goal notifications
std::map<std::string, std::pair<int, int>> previous_scores; // fixture_id -> (home_score, away_score)
std::mutex scores_mutex;

struct Match {
    std::string homeTeam, awayTeam, league, fixtureId;
    int homeScore, awayScore, minute;
};

void sendNotification(const Match& m) {
    std::cout << ">>> 🚨 GOAL NOTIFICATION: " << m.league << "! "
        << m.homeTeam << " " << m.homeScore << " - "
        << m.awayScore << " " << m.awayTeam << " (" << m.minute << "')" << std::endl;
}

void processLiveMatches(const std::string& apiKey) {
    while (true) {
        std::cout << "\n--- ⚽ Realssa Engine: Fetching Global Updates ---" << std::endl;

        cpr::Response r = cpr::Get(
            cpr::Url{ "https://v3.football.api-sports.io/fixtures?live=all" },
            cpr::Header{ {"x-apisports-key", apiKey}, {"x-rapidapi-host", "v3.football.api-sports.io"} }
        );

        if (r.status_code == 200) {
            try {
                auto data = json::parse(r.text);

                // Add timestamp to the response
                auto now = std::chrono::system_clock::now();
                auto timestamp = std::chrono::duration_cast<std::chrono::seconds>(
                    now.time_since_epoch()
                ).count();

                json response_with_timestamp = data;
                response_with_timestamp["timestamp"] = timestamp;
                response_with_timestamp["server"] = "Realssa C++ Engine";

                // Update the global string safely for the web server
                {
                    std::lock_guard<std::mutex> lock(data_mutex);
                    latest_match_data = response_with_timestamp.dump();
                }

                std::cout << "✅ Successfully fetched " << data["response"].size() << " live matches" << std::endl;

                // Process matches and check for goals
                for (auto& item : data["response"]) {
                    Match m;
                    m.fixtureId = std::to_string((int)item["fixture"]["id"]);
                    m.homeTeam = item["teams"]["home"]["name"];
                    m.awayTeam = item["teams"]["away"]["name"];
                    m.homeScore = item["goals"]["home"].is_null() ? 0 : (int)item["goals"]["home"];
                    m.awayScore = item["goals"]["away"].is_null() ? 0 : (int)item["goals"]["away"];
                    m.minute = item["fixture"]["status"]["elapsed"].is_null() ? 0 : (int)item["fixture"]["status"]["elapsed"];
                    m.league = item["league"]["name"];

                    // Check if this is an important league
                    bool isImportant = (m.league == "Premier League" ||
                        m.league == "NPFL" ||
                        m.league == "Ligue 1" ||
                        m.league == "La Liga" ||
                        m.league == "Serie A" ||
                        m.league == "Bundesliga" ||
                        m.league == "UEFA Champions League");

                    if (isImportant) {
                        std::cout << "[⭐ IMPORTANT] ";
                    }

                    std::cout << "[" << m.league << "] " << m.homeTeam << " " << m.homeScore
                        << " - " << m.awayScore << " " << m.awayTeam << " (" << m.minute << "')" << std::endl;

                    // Check for goal notifications
                    {
                        std::lock_guard<std::mutex> lock(scores_mutex);
                        auto it = previous_scores.find(m.fixtureId);

                        if (it != previous_scores.end()) {
                            int prevHome = it->second.first;
                            int prevAway = it->second.second;

                            // Goal scored!
                            if (m.homeScore > prevHome || m.awayScore > prevAway) {
                                sendNotification(m);
                            }
                        }

                        // Update previous scores
                        previous_scores[m.fixtureId] = { m.homeScore, m.awayScore };
                    }
                }
            }
            catch (const json::exception& e) {
                std::cerr << "❌ JSON parsing error: " << e.what() << std::endl;
                std::lock_guard<std::mutex> lock(data_mutex);
                latest_match_data = "{\"error\": \"JSON parsing failed\", \"status\": \"error\"}";
            }
        }
        else {
            std::cerr << "❌ API Error: HTTP " << r.status_code << std::endl;
            std::cerr << "   Response: " << r.text << std::endl;

            std::lock_guard<std::mutex> lock(data_mutex);
            json error_response;
            error_response["error"] = "Failed to fetch data from API";
            error_response["status_code"] = r.status_code;
            error_response["status"] = "error";
            latest_match_data = error_response.dump();
        }

        std::cout << "⏳ Waiting 2 minutes for next update..." << std::endl;
        std::this_thread::sleep_for(std::chrono::minutes(2)); // Changed from 15 to 2 minutes
    }
}

int main() {
    std::cout << R"(
    ╔═══════════════════════════════════════╗
    ║   Realssa Sports API - C++ Engine    ║
    ║   Live Football Scores Tracker       ║
    ╚═══════════════════════════════════════╝
    )" << std::endl;

    // Get API key from environment variable
    const char* envKey = std::getenv("SPORTS_API_KEY");
    std::string myApiKey = envKey ? envKey : "";

    if (myApiKey.empty()) {
        std::cerr << "❌ ERROR: SPORTS_API_KEY environment variable not set!" << std::endl;
        std::cerr << "   Please set your API key before running the application." << std::endl;
        std::cerr << "   Example: export SPORTS_API_KEY=your_key_here" << std::endl;
        return 1;
    }

    std::cout << "✅ API Key loaded successfully" << std::endl;

    // Start the background worker thread
    std::thread worker(processLiveMatches, myApiKey);
    worker.detach();

    // Setup the HTTP Server for your Vercel Frontend
    httplib::Server svr;

    // Main scores endpoint
    svr.Get("/scores", [](const httplib::Request&, httplib::Response& res) {
        std::lock_guard<std::mutex> lock(data_mutex);
        res.set_header("Access-Control-Allow-Origin", "*");
        res.set_header("Access-Control-Allow-Methods", "GET, OPTIONS");
        res.set_header("Access-Control-Allow-Headers", "Content-Type");
        res.set_content(latest_match_data, "application/json");
        });

    // Health check endpoint
    svr.Get("/", [](const httplib::Request&, httplib::Response& res) {
        res.set_header("Access-Control-Allow-Origin", "*");
        res.set_content("⚽ Realssa C++ Engine is Online and Running!", "text/plain");
        });

    // Status endpoint with more details
    svr.Get("/status", [](const httplib::Request&, httplib::Response& res) {
        json status;
        status["status"] = "online";
        status["engine"] = "Realssa C++ Sports API";
        status["version"] = "1.0.0";
        status["polling_interval"] = "2 minutes";

        auto now = std::chrono::system_clock::now();
        auto timestamp = std::chrono::duration_cast<std::chrono::seconds>(
            now.time_since_epoch()
        ).count();
        status["server_time"] = timestamp;

        res.set_header("Access-Control-Allow-Origin", "*");
        res.set_content(status.dump(2), "application/json");
        });

    // CORS preflight handler
    svr.Options("/(.*)", [](const httplib::Request&, httplib::Response& res) {
        res.set_header("Access-Control-Allow-Origin", "*");
        res.set_header("Access-Control-Allow-Methods", "GET, OPTIONS");
        res.set_header("Access-Control-Allow-Headers", "Content-Type");
        res.status = 204;
        });

    std::cout << "🚀 Realssa API starting on port 8080..." << std::endl;
    std::cout << "📡 Endpoints available:" << std::endl;
    std::cout << "   - GET /         - Health check" << std::endl;
    std::cout << "   - GET /scores   - Live match data" << std::endl;
    std::cout << "   - GET /status   - Server status" << std::endl;
    std::cout << "\n✨ Ready to serve live football scores!\n" << std::endl;

    svr.listen("0.0.0.0", 8080);

    return 0;
}