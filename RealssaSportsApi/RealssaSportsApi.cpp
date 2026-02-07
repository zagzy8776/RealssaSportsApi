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

// Global variables for thread-safe data sharing
std::string latest_match_data = "{\"status\": \"initializing\"}";
std::mutex data_mutex;

void fetchAndStoreMatches(const std::string& apiKey) {
    while (true) {
        std::cout << "\n--- Realssa Engine: Fetching Global Updates ---" << std::endl;

        cpr::Response r = cpr::Get(
            cpr::Url{ "https://v3.football.api-sports.io/fixtures?live=all" },
            cpr::Header{ {"x-apisports-key", apiKey}, {"x-rapidapi-host", "v3.football.api-sports.io"} }
        );

        if (r.status_code == 200) {
            try {
                // Parse and update the global string safely
                auto data = json::parse(r.text);
                std::lock_guard<std::mutex> lock(data_mutex);
                latest_match_data = data.dump();
                std::cout << "Successfully updated scores. Count: " << data["results"] << std::endl;
            }
            catch (const std::exception& e) {
                std::cerr << "JSON Parse Error: " << e.what() << std::endl;
            }
        }
        else {
            std::cerr << "API Error: " << r.status_code << " - " << r.text << std::endl;
        }

        std::cout << "Waiting 15 minutes for next update..." << std::endl;
        std::this_thread::sleep_for(std::chrono::minutes(15));
    }
}

int main() {
    // Hidden in Environment Variables for Railway safety
    const char* envKey = std::getenv("SPORTS_API_KEY");
    std::string myApiKey = envKey ? envKey : "fbfe74b149da8cac8b38fece2220b528";

    // 1. Start the Background Worker Thread to fetch data
    std::thread worker(fetchAndStoreMatches, myApiKey);
    worker.detach(); // Let it run independently

    // 2. Start the HTTP Server on the main thread
    httplib::Server svr;

    // Route for Vercel to get scores
    svr.Get("/scores", [](const httplib::Request&, httplib::Response& res) {
        std::lock_guard<std::mutex> lock(data_mutex);
        res.set_header("Access-Control-Allow-Origin", "*"); // CRITICAL: Allows Vercel to access
        res.set_content(latest_match_data, "application/json");
        });

    // Health check route
    svr.Get("/", [](const httplib::Request&, httplib::Response& res) {
        res.set_content("Realssa C++ Engine is Online", "text/plain");
        });

    int port = 8080;
    std::cout << "Realssa API starting on port " << port << "..." << std::endl;

    // Listen on 0.0.0.0 is required for Docker/Railway
    if (!svr.listen("0.0.0.0", port)) {
        std::cerr << "Could not start server on port " << port << std::endl;
        return 1;
    }

    return 0;
}