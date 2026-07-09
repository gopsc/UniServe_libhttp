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

namespace beast = boost::beast;
namespace http = beast::http;
namespace asio = boost::asio;
using tcp = boost::asio::ip::tcp;

namespace pmc {
namespace net {

/**
 * @brief HTTP request handler type definition
 * 
 * Handles HTTP requests and returns responses
 * 
 * @param req The HTTP request object. Interface:
 *            - http::request<http::string_body> (template type)
 *            - method() -> http::verb: Returns the HTTP method (GET, POST, etc.)
 *            - target() -> std::string: Returns the request URI path
 *            - version() -> unsigned: Returns HTTP version (11 for 1.1, 10 for 1.0)
 *            - body() -> std::string&: Returns the request body content
 *            - body() const -> const std::string&: Returns the request body content (const)
 *            - base() -> http::header&: Access to raw header fields
 *            - base() const -> const http::header&: Access to raw header fields (const)
 *            - at(string) -> std::string&: Access specific header field by name
 *            - at(string) const -> const std::string&: Access specific header field by name (const)
 *            - find(string) -> iterator: Find header field by name
 *            - find(string) const -> const_iterator: Find header field by name (const)
 *            - count(string) -> size_t: Check if header field exists (returns 0 or 1)
 *            - begin() -> iterator: Returns iterator to the beginning of headers
 *            - end() -> iterator: Returns iterator to the end of headers
 *            - cbegin() const -> const_iterator: Returns const iterator to the beginning
 *            - cend() const -> const_iterator: Returns const iterator to the end
 * @param params A map of URL query parameters parsed from the request target.
 *               Interface: std::unordered_map<std::string, std::string>
 *               - Keys are parameter names (decoded)
 *               - Values are the corresponding decoded values
 *               - Example: "?id=123&name=test" -> {"id":"123", "name":"test"}
 *               - Empty map if no query parameters exist
 * @return http::response<http::string_body> The HTTP response to be sent back to the client.
 *         Interface:
 *         - http::response<http::string_body> (template type)
 *         - result() -> http::status: Sets or gets the HTTP status code
 *         - result(http::status) -> void: Sets the HTTP status code
 *         - reason() -> std::string&: Returns the reason phrase
 *         - reason(string) -> void: Sets the reason phrase
 *         - body() -> std::string&: Returns the response body content
 *         - body() const -> const std::string&: Returns the response body content (const)
 *         - body(string) -> void: Sets the response body content
 *         - set(string, string) -> void: Sets a header field (name, value)
 *         - at(string) -> std::string&: Access specific header field by name
 *         - at(string) const -> const std::string&: Access specific header field by name (const)
 *         - find(string) -> iterator: Find header field by name
 *         - find(string) const -> const_iterator: Find header field by name (const)
 *         - count(string) -> size_t: Check if header field exists
 *         - begin() -> iterator: Returns iterator to the beginning of headers
 *         - end() -> iterator: Returns iterator to the end of headers
 *         - cbegin() const -> const_iterator: Returns const iterator to the beginning
 *         - cend() const -> const_iterator: Returns const iterator to the end
 *         - prepare_payload() -> void: MUST be called after setting body and headers.
 *           This function calculates and sets the Content-Length and Content-MD5 fields.
 *         - keep_alive() -> bool: Returns true if the connection should be kept alive
 *         - need_eof() -> bool: Returns true if the connection should be closed after sending
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
     * @param path Request path
     * @param handler Request handler
     */
    void get(const std::string& path, HttpRequestHandler handler);
    
    /**
     * @brief Register POST request handler
     * @param path Request path
     * @param handler Request handler
     */
    void post(const std::string& path, HttpRequestHandler handler);
    
    /**
     * @brief Register PUT request handler
     * @param path Request path
     * @param handler Request handler
     */
    void put(const std::string& path, HttpRequestHandler handler);
    
    /**
     * @brief Register DELETE request handler
     * @param path Request path
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
    
    // Route tables
    std::unordered_map<std::string, HttpRequestHandler> get_handlers_;
    std::unordered_map<std::string, HttpRequestHandler> post_handlers_;
    std::unordered_map<std::string, HttpRequestHandler> put_handlers_;
    std::unordered_map<std::string, HttpRequestHandler> delete_handlers_;
    
    // Middleware chain
    std::vector<MiddlewareHandler> middlewares_;
    
    // Static file directory
    std::string static_directory_;
    
    // Session management
    std::vector<std::weak_ptr<Session>> sessions_;
    std::mutex sessions_mutex_;
    
    // Internal methods
    std::unordered_map<std::string, std::string> parseQueryParams(const std::string& query);
    
    // Request handling
    http::response<http::string_body> handleRequest(const http::request<http::string_body>& req);
    
    // Session management
    void addSession(const std::shared_ptr<Session>& session);
    void removeSession(const Session* session);
    void closeAllSessions();
};

} // namespace net
} // namespace pmc

#endif // HTTPSERVER_HPP
