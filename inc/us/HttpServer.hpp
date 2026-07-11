// HttpServer.hpp
#ifndef HTTPSERVER_HPP
#define HTTPSERVER_HPP

#include <boost/asio.hpp>
#include <boost/beast.hpp>
#include <boost/beast/http.hpp>
#include <boost/beast/version.hpp>
#include <boost/asio/ip/tcp.hpp>
#include <boost/asio/strand.hpp>
#include <boost/config.hpp>
#include <memory>
#include <string>
#include <functional>
#include <unordered_map>
#include <vector>
#include <iostream>
#include <thread>
#include <atomic>
#include <mutex>
#include <chrono>
#include <regex>

namespace pmc {
namespace net {

namespace beast = boost::beast;
namespace http = beast::http;
namespace asio = boost::asio;
using tcp = boost::asio::ip::tcp;

/**
 * @brief HTTP request handler type definition
 * 
 * Handles HTTP requests and returns responses
 */
using HttpRequestHandler = std::function<http::response<http::string_body>(
    const http::request<http::string_body>&,
    const std::unordered_map<std::string, std::string>&)>;

/**
 * @brief Middleware handler type definition
 * 
 * Used for request chain processing
 */
using MiddlewareHandler = std::function<bool(
    const http::request<http::string_body>&,
    http::response<http::string_body>&,
    const std::unordered_map<std::string, std::string>&)>;

/**
 * @brief Route entry structure
 */
struct RouteEntry {
    std::string path;
    std::regex pattern;
    HttpRequestHandler handler;
    bool is_wildcard;
};

/**
 * @brief HTTP Server class (enhanced merged version)
 * 
 * Asynchronous HTTP server based on boost::asio and boost::beast
 * Combines ease of use from original version with functional completeness
 * of the fixed version
 */
class HttpServer {
public:
    /**
     * @brief Constructor (specifies listening address)
     * @param address Listening IP address (e.g., "0.0.0.0" or "127.0.0.1")
     * @param port Listening port
     * @param threads Number of worker threads
     */
    explicit HttpServer(const std::string& address, unsigned short port, unsigned int threads = 1);
    
    /**
     * @brief Destructor
     */
    ~HttpServer();
    
    /**
     * @brief Start the server
     */
    void start();
    
    /**
     * @brief Stop the server (do not call from worker threads)
     */
    void stop();
    
    /**
     * @brief Register GET request handler
     * @param path Request path (supports wildcards: * and ?)
     * @param handler Request handler
     */
    void get(const std::string& path, HttpRequestHandler handler);
    
    /**
     * @brief Register POST request handler
     * @param path Request path (supports wildcards: * and ?)
     * @param handler Request handler
     */
    void post(const std::string& path, HttpRequestHandler handler);
    
    /**
     * @brief Register PUT request handler
     * @param path Request path (supports wildcards: * and ?)
     * @param handler Request handler
     */
    void put(const std::string& path, HttpRequestHandler handler);
    
    /**
     * @brief Register DELETE request handler
     * @param path Request path (supports wildcards: * and ?)
     * @param handler Request handler
     */
    void del(const std::string& path, HttpRequestHandler handler);
    
    /**
     * @brief Register middleware
     * @param middleware Middleware handler
     */
    void use(MiddlewareHandler middleware);
    
    /**
     * @brief Set static file directory
     * @param path Static file directory path
     */
    void setStaticDirectory(const std::string& path);
    
    /**
     * @brief Get server status
     * @return Whether the server is running
     */
    bool isRunning() const;
    
    /**
     * @brief Get server port
     * @return Server listening port
     */
    unsigned short getPort() const;
    
    /**
     * @brief Get server listening address
     * @return Server listening address
     */
    std::string getAddress() const;
    
    /**
     * @brief Run the server (blocking call) - inherited from original version
     */
    void run();
    
    /**
     * @brief Display server status information - inherited from original version
     */
    void showStatus() const;

    /* debug tools */
    class debug {
    public:
        static void print_all_header_fields(const http::request<http::string_body>& req);
    };

private:
    // Internal class declarations
    class Session;
    class Listener;
    
    // Server configuration
    std::string address_;
    unsigned short port_;
    unsigned int threads_;
    std::atomic<bool> running_{false};
    
    // Boost.Asio context
    std::unique_ptr<asio::io_context> ioc_;
    
    // Network components
    std::shared_ptr<Listener> listener_;
    std::vector<std::thread> worker_threads_;
    
    // Route tables (supports wildcards)
    std::vector<RouteEntry> get_handlers_;
    std::vector<RouteEntry> post_handlers_;
    std::vector<RouteEntry> put_handlers_;
    std::vector<RouteEntry> delete_handlers_;
    
    // Middleware chain
    std::vector<MiddlewareHandler> middlewares_;
    
    // Static file directory
    std::string static_directory_;
    
    // Session management
    std::vector<std::weak_ptr<Session>> sessions_;
    std::mutex sessions_mutex_;
    
    // Internal methods
    std::unordered_map<std::string, std::string> parseQueryParams(const std::string& query);
    
    // Wildcard matching
    std::regex wildcardToRegex(const std::string& pattern);
    bool matchPath(const std::string& path, const std::regex& pattern);
    
    // Request handling
    http::response<http::string_body> handleRequest(const http::request<http::string_body>& req);
    
    // Route finding
    HttpRequestHandler findRoute(const std::string& method, const std::string& path);
    
    // Session management
    void addSession(const std::shared_ptr<Session>& session);
    void removeSession(const Session* session);
    void closeAllSessions();

};

} // namespace net
} // namespace pmc

#endif // HTTPSERVER_HPP
