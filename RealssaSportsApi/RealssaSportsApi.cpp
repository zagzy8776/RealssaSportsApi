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
#include <unordered_map>

using json = nlohmann::json;

// ============= CONFIGURATION =============
const std::string OPENLIGA_BASE = "https://api.openligadb.de";

// Expanded list of leagues – most reliable ones first
// You can add/remove shortcuts from https://api.openligadb.de/getavailableleagues
const std::vector<std::string> LEAGUES = {
    "bl1",      // 1. Bundesliga – best live support
    "bl2",      // 2. Bundesliga
    "bl3",      // 3. Liga
    "dfb",      // DFB-Pokal
    "cl",       // Champions League
    "ucl",      // UEFA Champions League (alternative/current)
    "el",       // Europa League
    "uel",      // UEFA Europa League variant
    "PL",       // Premier League
    "SA",       // Serie A
    "PD",       // La Liga
    "lg1"       // Ligue 1 (older data, but included)
};

const int CACHE_DURATION_MINUTES = 5;       // How often to refresh data
const int POLL_INTERVAL_LIVE_MIN = 1;     // Poll every 1 min when live games exist
const int POLL_INTERVAL_NO_LIVE_MIN = 5;    // Poll every 5 min otherwise

// ============= GLOBAL STATE =============
json cached_fixtures;
std::chrono::system_clock::time_point last_fixture_fetch;
std::mutex cache_mutex;
int live_match_count = 0;

// ============= UTILITY FUNCTIONS =============
std::pair<int, std::string> deriveStatus(const json& match) {
    bool finished = match.value("matchIsFinished", false);
    if (finished) return { 6, "FT" };

    std::string dt_str = match.value("matchDateTimeUTC", "");
    if (dt_str.empty()) return { 1, "NS" };

    // Parse ISO UTC time roughly
    std::tm tm{};
    if (strptime(dt_str.c_str(), "%Y-%m-%dT%H:%M:%SZ", &tm)) {
        auto match_time = timegm(&tm);  // UTC time_t
        auto now = std::time(nullptr);
        auto diff_min = (now - match_time) / 60;

        // Consider live if within ~30 min before → 3.5 hours after kickoff
        if (diff_min > -30 && diff_min < 210) {
            return { 2, "LIVE" };
        }
        if (diff_min >= 210) {
            return { 6, "FT" };  // Auto-finish old matches
        }
    }
    return { 1, "NS" };
}

json formatOpenLigaMatch(const json& raw) {
    json m;
    try {
        m["id"] = raw.value("matchID", 0);

        // Timestamp (unix seconds) for frontend date handling
        std::string dt = raw.value("matchDateTime", "");
        if (!dt.empty()) {
            std::tm tm{};
            strptime(dt.c_str(), "%Y-%m-%dT%H:%M:%S", &tm);  // note: local offset, approx
            m["timestamp"] = static_cast<int64_t>(std::mktime(&tm));
        }

        // Date string for grouping
        if (dt.size() >= 10) {
            m["date"] = dt.substr(0, 10);
        }

        // Teams
        auto t1 = raw.value("team1", json{});
        auto t2 = raw.value("team2", json{});
        m["home_team"] = t1.value("teamName", "Unknown");
        m["home_team_id"] = t1.value("teamId", 0);
        m["home_team_logo"] = t1.value("teamIconUrl", "");
        m["away_team"] = t2.value("teamName", "Unknown");
        m["away_team_id"] = t2.value("teamId", 0);
        m["away_team_logo"] = t2.value("teamIconUrl", "");

        // League info
        m["league"] = raw.value("leagueName", "Unknown League");
        m["league_country"] = "Various / Europe";

        // Scores
        int h = 0, a = 0, ht_h = 0, ht_a = 0;
        if (raw.contains("matchResults") && raw["matchResults"].is_array() && !raw["matchResults"].empty()) {
            auto latest = raw["matchResults"].back();
            h = latest.value("pointsTeam1", 0);
            a = latest.value("pointsTeam2", 0);
            if (raw["matchResults"].size() >= 2) {
                auto ht = raw["matchResults"][0];
                ht_h = ht.value("pointsTeam1", 0);
                ht_a = ht.value("pointsTeam2", 0);
            }
        }
        else if (raw.contains("goals") && raw["goals"].is_array() && !raw["goals"].empty()) {
            auto last = raw["goals"].back();
            h = last.value("scoreTeam1", 0);
            a = last.value("scoreTeam2", 0);
        }
        m["home_score"] = h;
        m["away_score"] = a;
        m["home_score_ht"] = ht_h;
        m["away_score_ht"] = ht_a;

        // Status & live time
        auto [sid, sname] = deriveStatus(raw);
        m["statusId"] = sid;
        m["statusName"] = sname;

        if (sname == "LIVE" && raw.contains("goals") && !raw["goals"].empty()) {
            int minute = raw["goals"].back().value("matchMinute", 0);
            m["time"] = std::to_string(minute) + "'";
        }
    }
    catch (const std::exception& e) {
        std::cerr << "Error formatting match: " << e.what() << std::endl;
    }
    return m;
}

int countLiveMatches() {
    std::lock_guard<std::mutex> lock(cache_mutex);
    int count = 0;
    if (cached_fixtures.contains("data") && cached_fixtures["data"].is_array()) {
        for (const auto& match : cached_fixtures["data"]) {
            if (match.value("statusId", 1) == 2) ++count;
        }
    }
    return count;
}

void refreshFixtureCache() {
    auto now = std::chrono::system_clock::now();
    auto minutes_since = std::chrono::duration_cast<std::chrono::minutes>(now - last_fixture_fetch).count();

    if (minutes_since < CACHE_DURATION_MINUTES && !cached_fixtures.empty()) {
        return;  // Cache still fresh
    }

    std::cout << "\n🔄 Refreshing OpenLigaDB cache...\n";

    json all;
    all["data"] = json::array();

    for (const auto& shortcut : LEAGUES) {
        auto resp = cpr::Get(
            cpr::Url{ OPENLIGA_BASE + "/getmatchdata/" + shortcut },
            cpr::Timeout{ 15000 }
        );

        if (resp.status_code == 200) {
            try {
                auto data = json::parse(resp.text);
                if (data.is_array()) {
                    for (const auto& raw : data) {
                        all["data"].push_back(formatOpenLigaMatch(raw));
                    }
                    std::cout << "  → " << data.size() << " matches from " << shortcut << "\n";
                }
            }
            catch (const std::exception& e) {
                std::cerr << "  Parse error for " << shortcut << ": " << e.what() << "\n";
            }
        }
        else {
            std::cout << "  Failed " << shortcut << " (" << resp.status_code << ")\n";
        }
    }

    {
        std::lock_guard<std::mutex> lock(cache_mutex);
        cached_fixtures = all;
        last_fixture_fetch = now;
    }

    std::cout << "Cache updated: " << cached_fixtures["data"].size() << " total matches\n";
}

void backgroundPoller() {
    while (true) {
        refreshFixtureCache();
        int live = countLiveMatches();
        {
            std::lock_guard<std::mutex> lock(cache_mutex);
            live_match_count = live;
        }

        int interval_min = (live > 0) ? POLL_INTERVAL_LIVE_MIN : POLL_INTERVAL_NO_LIVE_MIN;
        std::cout << "Live: " << live << " | Next poll: " << interval_min << " min\n";
        std::this_thread::sleep_for(std::chrono::minutes(interval_min));
    }
}

// ============= MAIN =============
int main() {
    crow::SimpleApp app;

    std::cout << "⚡ RealSSA Sports API – OpenLigaDB Edition\n";
    std::cout << "   Free forever – no key, no quota\n";
    std::cout << "   Leagues: " << LEAGUES.size() << " configured\n\n";

    cached_fixtures["data"] = json::array();
    last_fixture_fetch = std::chrono::system_clock::now() - std::chrono::minutes(CACHE_DURATION_MINUTES + 10);

    std::thread poller(backgroundPoller);
    poller.detach();

    // ROOT
    CROW_ROUTE(app, "/")
        ([]() {
        crow::response res;
        res.set_header("Content-Type", "application/json");
        res.set_header("Access-Control-Allow-Origin", "*");
        res.write(R"({"message":"RealSSA Sports API","version":"OpenLigaDB","status":"online"})");
        return res;
            });

    // SCORES – main endpoint
    CROW_ROUTE(app, "/scores")
        ([]() {
            {
                std::lock_guard<std::mutex> lock(cache_mutex);
                if (cached_fixtures.empty() || cached_fixtures["data"].empty()) {
                    refreshFixtureCache();
                }
            }

            json response;
            {
                std::lock_guard<std::mutex> lock(cache_mutex);
                response = cached_fixtures;
            }

            crow::response res;
            res.set_header("Content-Type", "application/json");
            res.set_header("Access-Control-Allow-Origin", "*");
            res.write(response.dump(2));
            return res;
            });

    // HEALTH
    CROW_ROUTE(app, "/health")
        ([]() {
        json health;
        health["status"] = "online";
        health["version"] = "OpenLigaDB Edition";
        health["live_matches"] = live_match_count;
        health["cached_matches"] = cached_fixtures.contains("data") ? cached_fixtures["data"].size() : 0;
        crow::response res;
        res.set_header("Content-Type", "application/json");
        res.set_header("Access-Control-Allow-Origin", "*");
        res.write(health.dump(2));
        return res;
            });

    const char* port_env = std::getenv("PORT");
    int port = port_env ? std::stoi(port_env) : 8080;
    std::cout << "🚀 Server running on port " << port << std::endl;
    app.port(port).multithreaded().run();

    return 0;
}