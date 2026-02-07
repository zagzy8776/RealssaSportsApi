#include <iostream>
#include <string>
#include <vector>
#include <thread>
#include <chrono>
#include <cpr/cpr.h>
#include <nlohmann/json.hpp>

using json = nlohmann::json;

struct Match {
    std::string homeTeam, awayTeam, league;
    int homeScore, awayScore, minute;
};

void sendNotification(const Match& m) {
    // This is a placeholder where our notification logic will go next
    std::cout << ">>> NOTIFICATION: Goal in " << m.league << "! "
        << m.homeTeam << " " << m.homeScore << " - "
        << m.awayScore << " " << m.awayTeam << std::endl;
}

void processLiveMatches(const std::string& apiKey) {
    cpr::Response r = cpr::Get(
        cpr::Url{ "https://v3.football.api-sports.io/fixtures?live=all" },
        cpr::Header{ {"x-apisports-key", apiKey}, {"x-rapidapi-host", "v3.football.api-sports.io"} }
    );

    if (r.status_code == 200) {
        auto data = json::parse(r.text);
        for (auto& item : data["response"]) {
            Match m;
            m.homeTeam = item["teams"]["home"]["name"];
            m.awayTeam = item["teams"]["away"]["name"];
            m.homeScore = item["goals"]["home"].is_null() ? 0 : (int)item["goals"]["home"];
            m.awayScore = item["goals"]["away"].is_null() ? 0 : (int)item["goals"]["away"];
            m.minute = item["fixture"]["status"]["elapsed"];
            m.league = item["league"]["name"];

            // Logic: Highlight important matches (Premier League, NPFL, etc.)
            if (m.league == "Premier League" || m.league == "NPFL" || m.league == "Ligue 1") {
                std::cout << "[IMPORTANT] ";
            }

            std::cout << "[" << m.league << "] " << m.homeTeam << " " << m.homeScore
                << " - " << m.awayScore << " " << m.awayTeam << " (" << m.minute << "')" << std::endl;

            // We will trigger notifications here in the next step
        }
    }
}

int main() {
    // Hidden in Environment Variables for Railway safety
    const char* envKey = std::getenv("SPORTS_API_KEY");
    std::string myApiKey = envKey ? envKey : "fbfe74b149da8cac8b38fece2220b528";

    while (true) {
        std::cout << "\n--- Realssa Engine: Fetching Global Updates ---" << std::endl;
        processLiveMatches(myApiKey);

        std::cout << "Waiting 15 minutes for next update..." << std::endl;
        std::this_thread::sleep_for(std::chrono::minutes(15)); // Loop for efficiency
    }
    return 0;
}