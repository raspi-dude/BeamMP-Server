// BeamMP, the BeamNG.drive multiplayer mod.
// Copyright (C) 2024 BeamMP Ltd., BeamMP team and contributors.
//
// BeamMP Ltd. can be contacted by electronic mail via contact@beammp.com.
//
// This program is free software: you can redistribute it and/or modify
// it under the terms of the GNU Affero General Public License as published
// by the Free Software Foundation, either version 3 of the License, or
// (at your option) any later version.
//
// This program is distributed in the hope that it will be useful,
// but WITHOUT ANY WARRANTY; without even the implied warranty of
// MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
// GNU Affero General Public License for more details.
//
// You should have received a copy of the GNU Affero General Public License
// along with this program.  If not, see <https://www.gnu.org/licenses/>.

#include "Http.h"

#include "Common.h"
#include "CustomAssert.h"

#include <curl/easy.h>
#include <atomic>
#include <chrono>
#include <map>
#include <nlohmann/json.hpp>
#include <thread>

using json = nlohmann::json;

/**
 * Cancel a transfer when the caller set its abort flag. curl polls this at
 * least once per second even while waiting on a slow or idle response, which
 * is what makes a blocked long-poll interruptible.
 */
static int CurlAbortCallback(void* clientp, curl_off_t, curl_off_t, curl_off_t, curl_off_t) {
    const auto* Abort = static_cast<const std::atomic<bool>*>(clientp);
    return (Abort != nullptr && Abort->load(std::memory_order_acquire)) ? 1 : 0;
}

static size_t CurlWriteCallback(void* contents, size_t size, size_t nmemb, void* userp) {
    std::string* Result = reinterpret_cast<std::string*>(userp);
    std::string NewContents(reinterpret_cast<char*>(contents), size * nmemb);
    *Result += NewContents;
    return size * nmemb;
}

struct CurlDeleter {
    void operator()(CURL* c) const noexcept {
        if (c)
            curl_easy_cleanup(c);
    }
};

static std::mutex gCurlPoolMutex;
static std::map<CURL*, bool> gCurlPool; // false = free, true = in use
constexpr size_t MAX_CURL_POOL_SIZE = 128;

static CURL* AcquireCurl() {
    std::unique_lock Lock(gCurlPoolMutex);
    for (auto& [Curl, InUse] : gCurlPool) {
        if (!InUse) {
            InUse = true;
            curl_easy_reset(Curl);
            return Curl;
        }
    }

    // none found, if we're under the limit add one!
    if (gCurlPool.size() >= MAX_CURL_POOL_SIZE) {
        beammp_debugf("Ran out of curl handles for network requests, skipping this request");
        return nullptr;
    }
    
    CURL* Curl = curl_easy_init();
    if (!Curl) {
        // failed, ignore
        return nullptr;
    }
    
    gCurlPool[Curl] = true;
    return Curl;
}

static void ReleaseCurl(CURL* curl) {
    if (!curl) {
        return;
    }

    std::unique_lock Lock(gCurlPoolMutex);
    auto It = gCurlPool.find(curl);
    if (It != gCurlPool.end()) {
        It->second = false;
    }
}

/// RAII container for curl handles to ensure correct release
class CurlLease {
private:
    CURL* mHandle = nullptr;
public:

    CurlLease() : mHandle(AcquireCurl()) {}
    ~CurlLease() {
        ReleaseCurl(mHandle);
    }
    CurlLease(const CurlLease&) = delete;
    CurlLease& operator=(const CurlLease&) = delete;

    CurlLease(CurlLease&& other) noexcept
        : mHandle(other.mHandle) {
        other.mHandle = nullptr;
    }

    CurlLease& operator=(CurlLease&& other) noexcept {
        if (this != &other) {
            ReleaseCurl(mHandle);
            mHandle = other.mHandle;
            other.mHandle = nullptr;
        }
        return *this;
    }

    CURL* GetHandle() const noexcept {
        return mHandle;
    }
};

std::string Http::GET(const std::string& url, unsigned int* status) {
    std::string Ret;
    CurlLease Lease{};
    CURL* curl = Lease.GetHandle();
    if (curl) {
        CURLcode res;
        char errbuf[CURL_ERROR_SIZE];
        curl_easy_setopt(curl, CURLOPT_URL, url.c_str());
        curl_easy_setopt(curl, CURLOPT_IPRESOLVE, CURL_IPRESOLVE_V4);
        curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, CurlWriteCallback);
        curl_easy_setopt(curl, CURLOPT_WRITEDATA, reinterpret_cast<void*>(&Ret));

        // ensure we dont keep connections open for long
        curl_easy_setopt(curl, CURLOPT_MAXCONNECTS, 8L);
        curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT, 10L);
        curl_easy_setopt(curl, CURLOPT_TIMEOUT, 30L);
        curl_easy_setopt(curl, CURLOPT_NOSIGNAL, 1L);
        curl_easy_setopt(curl, CURLOPT_MAXAGE_CONN, 60L);
        curl_easy_setopt(curl, CURLOPT_MAXLIFETIME_CONN, 300L);

        curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 1L);
        curl_easy_setopt(curl, CURLOPT_ERRORBUFFER, errbuf);
        errbuf[0] = 0;
        res = curl_easy_perform(curl);
        if (res != CURLE_OK) {
            beammp_error("GET to " + url + " failed: " + std::string(curl_easy_strerror(res)));
            beammp_error("Curl error: " + std::string(errbuf));
            return Http::ErrorString;
        }

        if (status) {
            curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, status);
        }

    } else {
        beammp_error("Curl easy init failed");
        return Http::ErrorString;
    }
    return Ret;
}

std::string Http::POST(const std::string& url, const std::string& body, const std::string& ContentType, unsigned int* status, const std::map<std::string, std::string>& headers, const std::atomic<bool>* Abort) {
    std::string Ret;
    CurlLease Lease{};
    CURL* curl = Lease.GetHandle();
    if (curl) {
        CURLcode res;
        char errbuf[CURL_ERROR_SIZE];
        curl_easy_setopt(curl, CURLOPT_URL, url.c_str());
        curl_easy_setopt(curl, CURLOPT_IPRESOLVE, CURL_IPRESOLVE_V4);
        curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, CurlWriteCallback);
        curl_easy_setopt(curl, CURLOPT_WRITEDATA, reinterpret_cast<void*>(&Ret));
        curl_easy_setopt(curl, CURLOPT_POST, 1L);
        curl_easy_setopt(curl, CURLOPT_POSTFIELDS, body.c_str());
        curl_easy_setopt(curl, CURLOPT_POSTFIELDSIZE, body.size());
        struct curl_slist* list = nullptr;
        list = curl_slist_append(list, ("Content-Type: " + ContentType).c_str());

        for (auto [header, value] : headers) {
            list = curl_slist_append(list, (header + ": " + value).c_str());
        }

        curl_easy_setopt(curl, CURLOPT_HTTPHEADER, list);

        // ensure we dont keep connections open for long
        curl_easy_setopt(curl, CURLOPT_MAXCONNECTS, 8L);
        curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT, 10L);
        curl_easy_setopt(curl, CURLOPT_TIMEOUT, 30L);
        curl_easy_setopt(curl, CURLOPT_NOSIGNAL, 1L);
        curl_easy_setopt(curl, CURLOPT_MAXAGE_CONN, 60L);
        curl_easy_setopt(curl, CURLOPT_MAXLIFETIME_CONN, 300L);

        curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 1L);
        curl_easy_setopt(curl, CURLOPT_ERRORBUFFER, errbuf);
        if (Abort != nullptr) {
            curl_easy_setopt(curl, CURLOPT_NOPROGRESS, 0L);
            curl_easy_setopt(curl, CURLOPT_XFERINFOFUNCTION, CurlAbortCallback);
            curl_easy_setopt(curl, CURLOPT_XFERINFODATA, const_cast<std::atomic<bool>*>(Abort));
        }
        errbuf[0] = 0;
        res = curl_easy_perform(curl);
        curl_slist_free_all(list);
        if (res != CURLE_OK) {
            // A cancelled request is the caller's own doing, not a failure.
            // Status is zeroed so callers never see a stale sentinel value.
            if (res == CURLE_ABORTED_BY_CALLBACK) {
                if (status) {
                    *status = 0;
                }
                return Http::ErrorString;
            }
            beammp_error("POST to " + url + " failed: " + std::string(curl_easy_strerror(res)));
            beammp_error("Curl error: " + std::string(errbuf));
            return Http::ErrorString;
        }

        if (status) {
            curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, status);
        }

    } else {
        beammp_error("Curl easy init failed");
        return Http::ErrorString;
    }
    return Ret;
}

// RFC 2616, RFC 7231
static std::map<size_t, const char*> Map = {
    { -1, "Invalid Response Code" },
    { 100, "Continue" },
    { 101, "Switching Protocols" },
    { 102, "Processing" },
    { 103, "Early Hints" },
    { 200, "OK" },
    { 201, "Created" },
    { 202, "Accepted" },
    { 203, "Non-Authoritative Information" },
    { 204, "No Content" },
    { 205, "Reset Content" },
    { 206, "Partial Content" },
    { 207, "Multi-Status" },
    { 208, "Already Reported" },
    { 226, "IM Used" },
    { 300, "Multiple Choices" },
    { 301, "Moved Permanently" },
    { 302, "Found" },
    { 303, "See Other" },
    { 304, "Not Modified" },
    { 305, "Use Proxy" },
    { 306, "(Unused)" },
    { 307, "Temporary Redirect" },
    { 308, "Permanent Redirect" },
    { 400, "Bad Request" },
    { 401, "Unauthorized" },
    { 402, "Payment Required" },
    { 403, "Forbidden" },
    { 404, "Not Found" },
    { 405, "Method Not Allowed" },
    { 406, "Not Acceptable" },
    { 407, "Proxy Authentication Required" },
    { 408, "Request Timeout" },
    { 409, "Conflict" },
    { 410, "Gone" },
    { 411, "Length Required" },
    { 412, "Precondition Failed" },
    { 413, "Payload Too Large" },
    { 414, "URI Too Long" },
    { 415, "Unsupported Media Type" },
    { 416, "Range Not Satisfiable" },
    { 417, "Expectation Failed" },
    { 421, "Misdirected Request" },
    { 422, "Unprocessable Entity" },
    { 423, "Locked" },
    { 424, "Failed Dependency" },
    { 425, "Too Early" },
    { 426, "Upgrade Required" },
    { 428, "Precondition Required" },
    { 429, "Too Many Requests" },
    { 431, "Request Header Fields Too Large" },
    { 451, "Unavailable For Legal Reasons" },
    { 500, "Internal Server Error" },
    { 501, "Not Implemented" },
    { 502, "Bad Gateway" },
    { 503, "Service Unavailable" },
    { 504, "Gateway Timeout" },
    { 505, "HTTP Version Not Supported" },
    { 506, "Variant Also Negotiates" },
    { 507, "Insufficient Storage" },
    { 508, "Loop Detected" },
    { 510, "Not Extended" },
    { 511, "Network Authentication Required" },
    // cloudflare status codes
    { 520, "(CDN) Web Server Returns An Unknown Error" },
    { 521, "(CDN) Web Server Is Down" },
    { 522, "(CDN) Connection Timed Out" },
    { 523, "(CDN) Origin Is Unreachable" },
    { 524, "(CDN) A Timeout Occurred" },
    { 525, "(CDN) SSL Handshake Failed" },
    { 526, "(CDN) Invalid SSL Certificate" },
    { 527, "(CDN) Railgun Listener To Origin Error" },
    { 530, "(CDN) 1XXX Internal Error" },
};

static const char Magic[] = {
    0x20, 0x2f, 0x5c, 0x5f,
    0x2f, 0x5c, 0x0a, 0x28,
    0x20, 0x6f, 0x2e, 0x6f,
    0x20, 0x29, 0x0a, 0x20,
    0x3e, 0x20, 0x5e, 0x20,
    0x3c, 0x0a, 0x00
};

std::string Http::Status::ToString(int Code) {
    if (Map.find(Code) != Map.end()) {
        return Map.at(Code);
    } else {
        return std::to_string(Code);
    }
}

TEST_CASE("Http::Status::ToString") {
    CHECK(Http::Status::ToString(200) == "OK");
    CHECK(Http::Status::ToString(696969) == "696969");
    CHECK(Http::Status::ToString(-1) == "Invalid Response Code");
}

TEST_CASE("Http::POST aborts a slow request when the abort flag is set") {
    // Stands in for the relay's long-poll: it holds the request open far longer
    // than the test is willing to wait.
    httplib::Server SlowServer;
    SlowServer.Post("/slow", [](const httplib::Request&, httplib::Response& res) {
        std::this_thread::sleep_for(std::chrono::seconds(5));
        res.set_content("too late", "text/plain");
    });
    const int Port = SlowServer.bind_to_any_port("127.0.0.1");
    REQUIRE(Port > 0);
    std::thread Listener([&SlowServer] { SlowServer.listen_after_bind(); });

    std::atomic<bool> Abort { false };
    std::thread Canceller([&Abort] {
        std::this_thread::sleep_for(std::chrono::milliseconds(300));
        Abort.store(true, std::memory_order_release);
    });

    const auto Start = std::chrono::steady_clock::now();
    unsigned int Status = 1;
    const std::string Result = Http::POST("http://127.0.0.1:" + std::to_string(Port) + "/slow",
        "{}", "application/json", &Status, {}, &Abort);
    const auto Elapsed = std::chrono::steady_clock::now() - Start;

    Canceller.join();
    SlowServer.stop();
    Listener.join();

    CHECK(Result == Http::ErrorString);
    CHECK(Status == 0);
    // curl calls the progress callback about once a second, so this must still
    // be well below the 5s the server would have taken.
    CHECK(Elapsed < std::chrono::seconds(3));
}

Http::Server::THttpServerInstance::THttpServerInstance() {
    Application::SetSubsystemStatus("HTTPServer", Application::Status::Starting);
    mThread = std::thread(&Http::Server::THttpServerInstance::operator(), this);
    mThread.detach();
}

void Http::Server::THttpServerInstance::operator()() try {
    std::unique_ptr<httplib::Server> HttpLibServerInstance;
    HttpLibServerInstance = std::make_unique<httplib::Server>();
    // todo: make this IP agnostic so people can set their own IP
    HttpLibServerInstance->Get("/", [](const httplib::Request&, httplib::Response& res) {
        res.set_content("<!DOCTYPE html><article><h1>Hello World!</h1><section><p>BeamMP Server can now serve HTTP requests!</p></section></article></html>", "text/html");
    });
    HttpLibServerInstance->Get("/health", [](const httplib::Request&, httplib::Response& res) {
        size_t SystemsGood = 0;
        size_t SystemsBad = 0;
        auto Statuses = Application::GetSubsystemStatuses();
        for (const auto& NameStatusPair : Statuses) {
            switch (NameStatusPair.second) {
            case Application::Status::Starting:
            case Application::Status::ShuttingDown:
            case Application::Status::Shutdown:
            case Application::Status::Good:
                SystemsGood++;
                break;
            case Application::Status::Bad:
                SystemsBad++;
                break;
            default:
                beammp_assert_not_reachable();
            }
        }
        res.set_content(
            json {
                { "ok", SystemsBad == 0 },
            }
                .dump(),
            "application/json");
        res.status = 200;
    });
    // magic endpoint
    HttpLibServerInstance->Get({ 0x2f, 0x6b, 0x69, 0x74, 0x74, 0x79 }, [](const httplib::Request&, httplib::Response& res) {
        res.set_content(std::string(Magic), "text/plain");
    });
    HttpLibServerInstance->set_logger([](const httplib::Request& Req, const httplib::Response& Res) {
        beammp_debug("Http Server: " + Req.method + " " + Req.target + " -> " + std::to_string(Res.status));
    });
    Application::SetSubsystemStatus("HTTPServer", Application::Status::Good);
} catch (const std::exception& e) {
    beammp_error("Failed to start http server. Please ensure the http server is configured properly in the ServerConfig.toml, or turn it off if you don't need it. Error: " + std::string(e.what()));
}
