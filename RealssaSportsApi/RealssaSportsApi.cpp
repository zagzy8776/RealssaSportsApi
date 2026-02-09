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
#include <algorithm>

using json = nlohmann::json;

// OpenLigaDB Configuration - NO API KEY NEEDED!
const std::string OPENLIGA_BASE = "https://api.openligadb.de";

// League shortcuts to fetch (add more as needed)
const std::vector<std::string> LEAGUES = {
    "bl1",    // Bundesliga (Germany)
    "bl2",    // 2. Bundesliga
    "bl3",    // 3. Liga
    "cl",     // Champions League
    "el",     // Europa League
    "PL",     // Premier League
    "SA",     // Serie A
    "PD",     // La Liga (Primera Division)
    "lg1",    // Ligue 1
    "erl"     // Eredivisie
};

// Polling intervals
const int POLL_INTERVAL_WITH_LIVE = 3;    // 3 minutes when live matches
const int POLL_INTERVAL_NO_LIVE = 10;     // 10 minutes when no live matches

// Global state
json cached_fixtures;
std::chrono::system_clock::time_point last_fixture_fetch;
std::mutex cache_mutex;
std::mutex counter_mutex;
int api_calls_today = 0;
int live_match_count = 0;

// Cross-platform date handling
std::string getTodayDate() {
    auto now = std::chrono::system_clock::now();
    std::time_t now_time = std::chrono::system_clock::to_time_t(now);
    std::tm timeinfo;

#ifdef _WIN32
    localtime_s(&timeinfo, &now_time);
#else
    localtime_r(&now_time, &timeinfo);
#endif

    char buffer[11];
    std::strftime(buffer, sizeof(buffer), "%Y-%m-%d", &timeinfo);
    return std::string(buffer);
}

// Parse ISO datetime to Unix timestamp
time_t parseISODateTime(const std::string& iso_str) {
    if (iso_str.empty()) return 0;

    std::tm tm = {};
    // Try to parse "2026-02-08T17:30:00"
    if (sscanf(iso_str.c_str(), "%d-%d-%dT%d:%d:%d",
        &tm.tm_year, &tm.tm_mon, &tm.tm_mday,
        &tm.tm_hour, &tm.tm_min, &tm.tm_sec) == 6) {
        tm.tm_year -= 1900;
        tm.tm_mon -= 1;
        return std::mktime(&tm);
    }
    return 0;
}

// Derive match status from OpenLigaDB data
std::pair<int, std::string> deriveStatus(const json& match) {
    bool finished = match.value("matchIsFinished", false);

    if (finished) {
        return { 6, "FT" };
    }

    std::string dt_str = match.value("matchDateTimeUTC", "");
    if (dt_str.empty()) {
        dt_str = match.value("matchDateTime", "");
    }

    if (dt_str.empty()) {
        return { 1, "NS" };
    }

    time_t match_time = parseISODateTime(dt_str);
    time_t now = std::time(nullptr);

    int diff_min = static_cast<int>((now - match_time) / 60);

    // Match is live if started within last 3 hours and not finished
    if (diff_min > -30 && diff_min < 180) {
        return { 2, "LIVE" };
    }

    // Auto-finish old "live" matches
    if (diff_min >= 180) {
        return { 6, "FT" };
    }

    return { 1, "NS" };
}

// Format OpenLigaDB match to our standard format
json formatOpenLigaMatch(const json& raw) {
    json m;

    try {
        m["id"] = raw.value("matchID", 0);

        // Parse timestamp
        std::string dt_str = raw.value("matchDateTime", "");
        time_t timestamp = parseISODateTime(dt_str);
        m["timestamp"] = timestamp;

        // Date string
        if (!dt_str.empty() && dt_str.length() >= 10) {
            m["date"] = dt_str.substr(0, 10);
        }
        else {
            m["date"] = "unknown";
        }

        // Teams
        if (raw.contains("team1") && raw["team1"].is_object()) {
            auto t1 = raw["team1"];
            m["home_team"] = t1.value("teamName", "Unknown");
            m["home_team_id"] = t1.value("teamId", 0);
            m["home_team_logo"] = t1.value("teamIconUrl", "");
        }
        else {
            m["home_team"] = "Unknown";
            m["home_team_id"] = 0;
            m["home_team_logo"] = "";
        }

        if (raw.contains("team2") && raw["team2"].is_object()) {
            auto t2 = raw["team2"];
            m["away_team"] = t2.value("teamName", "Unknown");
            m["away_team_id"] = t2.value("teamId", 0);
            m["away_team_logo"] = t2.value("teamIconUrl", "");
        }
        else {
            m["away_team"] = "Unknown";
            m["away_team_id"] = 0;
            m["away_team_logo"] = "";
        }

        // League info
        m["league"] = raw.value("leagueName", "");

        // Determine league country/region
        std::string shortcut = raw.value("leagueShortcut", "");
        if (shortcut == "bl1" || shortcut == "bl2" || shortcut == "bl3") {
            m["league_country"] = "Germany";
        }
        else if (shortcut == "PL") {
            m["league_country"] = "England";
        }
        else if (shortcut == "SA") {
            m["league_country"] = "Italy";
        }
        else if (shortcut == "PD") {
            m["league_country"] = "Spain";
        }
        else if (shortcut == "lg1") {
            m["league_country"] = "France";
        }
        else if (shortcut == "erl") {
            m["league_country"] = "Netherlands";
        }
        else if (shortcut == "cl" || shortcut == "el") {
            m["league_country"] = "Europe";
        }
        else {
            m["league_country"] = "International";
        }

        // League logo (construct from ID if available)
        if (raw.contains("leagueId") && raw["leagueId"].is_number()) {
            m["league_logo"] = "https://upload.wikimedia.org/wikipedia/commons/thumb/8/82/Soccer_ball.svg/120px-Soccer_ball.svg.png";
        }

        // Scores - get from matchResults or goals
        int home_score = 0;
        int away_score = 0;
        int home_ht = 0;
        int away_ht = 0;

        if (raw.contains("matchResults") && raw["matchResults"].is_array() && !raw["matchResults"].empty()) {
            // Full-time score (last result)
            auto latest = raw["matchResults"].back();
            home_score = latest.value("pointsTeam1", 0);
            away_score = latest.value("pointsTeam2", 0);

            // Half-time score (first result if exists)
            if (raw["matchResults"].size() >= 2) {
                auto ht = raw["matchResults"][0];
                home_ht = ht.value("pointsTeam1", 0);
                away_ht = ht.value("pointsTeam2", 0);
            }
        }
        else if (raw.contains("goals") && raw["goals"].is_array() && !raw["goals"].empty()) {
            // Get score from last goal
            auto last_goal = raw["goals"].back();
            home_score = last_goal.value("scoreTeam1", 0);
            away_score = last_goal.value("scoreTeam2", 0);
        }

        m["home_score"] = home_score;
        m["away_score"] = away_score;
        m["home_score_ht"] = home_ht;
        m["away_score_ht"] = away_ht;

        // Status
        auto [statusId, statusName] = deriveStatus(raw);
        m["statusId"] = statusId;
        m["statusName"] = statusName;

        // Live minute (from last goal)
        if (statusId == 2 && raw.contains("goals") && raw["goals"].is_array() && !raw["goals"].empty()) {
            auto last_goal = raw["goals"].back();
            int minute = last_goal.value("matchMinute", 0);
            m["time"] = std::to_string(minute) + "'";
        }

        // Last update time
        m["last_update"] = raw.value("lastUpdateDateTime", "");

    }
    catch (const std::exception& e) {
        std::cerr << "⚠️  Error formatting match: " << e.what() << std::endl;
    }

    return m;
}

// Make OpenLigaDB request (no auth needed!)
cpr::Response makeOpenLigaRequest(const std::string& path) {
    auto response = cpr::Get(
        cpr::Url{ OPENLIGA_BASE + path },
        cpr::Timeout{ 15000 }
    );

    {
        std::lock_guard<std::mutex> lock(counter_mutex);
        api_calls_today++;
    }

    return response;
}

// Refresh fixture cache from OpenLigaDB
void refreshFixtureCache() {
    auto now = std::chrono::system_clock::now();
    auto minutes_since_fetch = std::chrono::duration_cast<std::chrono::minutes>(
        now - last_fixture_fetch
    ).count();

    // Minimum cache duration: 5 minutes
    if (minutes_since_fetch < 5 && !cached_fixtures.empty()) {
        std::cout << "💾 Using cached fixtures (last fetch: " << minutes_since_fetch << " min ago)" << std::endl;
        return;
    }

    std::cout << "\n🔄 Refreshing OpenLigaDB cache..." << std::endl;

    json allFixtures;
    allFixtures["data"] = json::array();

    int total_matches = 0;

    for (const auto& shortcut : LEAGUES) {
        std::cout << "📡 Fetching " << shortcut << "..." << std::endl;

        auto response = makeOpenLigaRequest("/getmatchdata/" + shortcut);

        if (response.status_code != 200) {
            std::cerr << "   ❌ HTTP " << response.status_code << std::endl;
            continue;
        }

        try {
            auto data = json::parse(response.text);

            if (!data.is_array()) {
                std::cerr << "   ⚠️  Unexpected response format" << std::endl;
                continue;
            }

            int count = 0;
            for (const auto& raw_match : data) {
                allFixtures["data"].push_back(formatOpenLigaMatch(raw_match));
                count++;
            }

            total_matches += count;
            std::cout << "   ✅ Added " << count << " matches" << std::endl;

        }
        catch (const std::exception& e) {
            std::cerr << "   ❌ Parse error: " << e.what() << std::endl;
        }

        // Small delay between league fetches
        std::this_thread::sleep_for(std::chrono::milliseconds(500));
    }

    {
        std::lock_guard<std::mutex> lock(cache_mutex);
        cached_fixtures = allFixtures;
        last_fixture_fetch = now;
    }

    std::cout << "✅ Cache refreshed: " << total_matches << " total matches from " << LEAGUES.size() << " leagues" << std::endl;
}

// Count live matches
int countLiveMatches() {
    std::lock_guard<std::mutex> lock(cache_mutex);
    int count = 0;

    if (cached_fixtures.contains("data") && cached_fixtures["data"].is_array()) {
        for (const auto& match : cached_fixtures["data"]) {
            if (match.contains("statusId") && match["statusId"] == 2) {
                count++;
            }
        }
    }

    return count;
}

// Background polling thread
void backgroundPoller() {
    while (true) {
        refreshFixtureCache();

        int liveMatches = countLiveMatches();

        {
            std::lock_guard<std::mutex> lock(counter_mutex);
            live_match_count = liveMatches;
        }

        // Adaptive polling: faster when live matches exist
        int pollInterval = (liveMatches > 0) ? POLL_INTERVAL_WITH_LIVE : POLL_INTERVAL_NO_LIVE;

        std::cout << "\n📊 Status:" << std::endl;
        std::cout << "   API Calls Today: " << api_calls_today << std::endl;
        std::cout << "   Live Matches: " << liveMatches << std::endl;
        std::cout << "   ⏳ Next poll: " << pollInterval << " minutes" << std::endl;

        std::this_thread::sleep_for(std::chrono::minutes(pollInterval));
    }
}

int main() {
    crow::SimpleApp app;

    std::cout << "⚡ OpenLigaDB Sports API - FREE & UNLIMITED!" << std::endl;
    std::cout << "📡 Leagues: Bundesliga, Champions League, Premier League, La Liga, Serie A, Ligue 1, Eredivisie" << std::endl;

    // Initialize cache
    cached_fixtures["data"] = json::array();
    last_fixture_fetch = std::chrono::system_clock::now() - std::chrono::hours(1);

    std::cout << "\n🚀 Starting background poller..." << std::endl;
    std::cout << "   Poll interval (No Live): " << POLL_INTERVAL_NO_LIVE << " min" << std::endl;
    std::cout << "   Poll interval (Live): " << POLL_INTERVAL_WITH_LIVE << " min" << std::endl;

    // Start background polling
    std::thread poller(backgroundPoller);
    poller.detach();

    // Health check endpoint
    CROW_ROUTE(app, "/")
        ([]() {
        crow::response res;
        res.set_header("Content-Type", "application/json");
        res.set_header("Access-Control-Allow-Origin", "*");
        res.write("{\n  \"message\": \"RealSSA Sports API - OpenLigaDB Edition\",\n  \"status\": \"online\",\n  \"version\": \"10.0.0 - Free Forever\",\n  \"provider\": \"OpenLigaDB\"\n}");
        return res;
            });

    // Scores endpoint
    CROW_ROUTE(app, "/scores")
        ([]() {
        // Ensure cache is reasonably fresh
        auto now = std::chrono::system_clock::now();
        auto minutes_since = std::chrono::duration_cast<std::chrono::minutes>(
            now - last_fixture_fetch
        ).count();

        // Only force refresh if cache is very old
        if (minutes_since >= 10) {
            refreshFixtureCache();
        }

        json response;
        {
            std::lock_guard<std::mutex> lock(cache_mutex);
            response = cached_fixtures;
        }

        // Add metadata
        response["meta"] = {
            {"provider", "OpenLigaDB"},
            {"free", true},
            {"no_quota_limits", true},
            {"last_updated", minutes_since},
            {"total_matches", response["data"].size()},
            {"live_matches", live_match_count}
        };

        crow::response res;
        res.set_header("Content-Type", "application/json");
        res.set_header("Access-Control-Allow-Origin", "*");
        res.write(response.dump(2));
        return res;
            });

    // Health endpoint with stats
    CROW_ROUTE(app, "/health")
        ([]() {
        json health;
        health["status"] = "online";
        health["engine"] = "RealSSA OpenLigaDB Engine";
        health["api_provider"] = "OpenLigaDB (api.openligadb.de)";
        health["version"] = "10.0.0 - Free Forever";
        health["today"] = getTodayDate();

        // No quota - unlimited!
        health["quota"] = {
            {"unlimited", true},
            {"free_forever", true},
            {"api_calls_today", api_calls_today}
        };

        // Match stats
        int total = 0;
        {
            std::lock_guard<std::mutex> lock(cache_mutex);
            if (cached_fixtures.contains("data")) {
                total = cached_fixtures["data"].size();
            }
        }

        health["matches"] = {
            {"live", live_match_count},
            {"total_cached", total}
        };

        // Coverage
        health["leagues"] = LEAGUES;

        // System info
        health["polling"] = {
            {"interval_no_live_min", POLL_INTERVAL_NO_LIVE},
            {"interval_with_live_min", POLL_INTERVAL_WITH_LIVE}
        };

        health["server_time"] = std::chrono::system_clock::to_time_t(std::chrono::system_clock::now());
        health["features"] = { "free_unlimited", "smart_caching", "adaptive_polling", "multi_league" };

        crow::response res;
        res.set_header("Content-Type", "application/json");
        res.set_header("Access-Control-Allow-Origin", "*");
        res.write(health.dump(2));
        return res;
            });

    // Get port from environment or default to 8080
    const char* port_env = std::getenv("PORT");
    int port = port_env ? std::stoi(port_env) : 8080;

    std::cout << "\n🚀 Server starting on Port " << port << std::endl;
    std::cout << "🌍 Endpoints:" << std::endl;
    std::cout << "   GET / - Health check" << std::endl;
    std::cout << "   GET /scores - All matches" << std::endl;
    std::cout << "   GET /health - Detailed stats" << std::endl;

    app.port(port).multithreaded().run();

    return 0;
}