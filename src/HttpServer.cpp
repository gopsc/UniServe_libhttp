// HttpServer.cpp
#include "us/HttpServer.hpp"
#include <boost/algorithm/string.hpp>
#include <fstream>
#include <sstream>

namespace pmc {
namespace net {

/* Base exception class for all HttpServer exceptions */
class HttpServerException: public std::runtime_error {
    public:
        HttpServerException(const std::string& msg): runtime_error(msg) {}
};

/* Base exception class for all HttpListener exceptions */
class HttpListenerException: public HttpServerException {
    public:
        explicit HttpListenerException(const std::string& msg)
        : HttpServerException(msg) {}
};

/* Failed to create IO_CONTEXT */
class CreateObjectFailed: public HttpServerException {
    public:
        explicit CreateObjectFailed(const std::string& msg)
        : HttpServerException(msg) {}
};


// Session class - handles individual HTTP connections
class HttpServer::Session : public std::enable_shared_from_this<Session> {
public:
    Session(tcp::socket&& socket, HttpServer* server)
        : socket_(std::move(socket)), server_(server) {
    }
    
    void run() {
        doRead();
    }
    
private:
    void doRead() {
        auto self = shared_from_this();
        http::async_read(socket_, buffer_, request_,
            [this, self](beast::error_code ec, std::size_t) {
                if (!ec) {
                    processRequest();
                } else if (ec != http::error::end_of_stream) {
                    // Error handling
                }
            });
    }
    
    void processRequest() {
        try {
            // Create response object
            auto response = std::make_shared<http::response<http::string_body>>(
                server_->handleRequest(request_)
            );
            doWrite(response);
        } catch (const std::exception& e) {
            std::cerr << "Error processing request: " << e.what() << std::endl;
            // Send error response
            auto error_response = std::make_shared<http::response<http::string_body>>();
            error_response->version(request_.version());
            error_response->result(http::status::internal_server_error);
            error_response->set(http::field::content_type, "text/plain");
            error_response->body() = "500 Internal Server Error\n";
            error_response->prepare_payload();
            doWrite(error_response);
        }
    }
    
    void doWrite(std::shared_ptr<http::response<http::string_body>> response) {
        auto self = shared_from_this();
        http::async_write(socket_, *response,
            [this, self, response](beast::error_code ec, std::size_t) {
                if (!ec) {
                    // Check if connection should be kept alive
                    if (!response->keep_alive()) {
                        socket_.shutdown(tcp::socket::shutdown_send, ec);
                    }
                }
                // Continue reading next request
                doRead();
            });
    }
    
    tcp::socket socket_;
    beast::flat_buffer buffer_;
    http::request<http::string_body> request_;
    HttpServer* server_;
};

// Listener class - accepts new connections
class HttpServer::Listener : public std::enable_shared_from_this<Listener> {
public:
    Listener(asio::io_context& ioc, tcp::endpoint endpoint, HttpServer* server)
        : ioc_(ioc), acceptor_(ioc), server_(server) {
        beast::error_code ec;
        acceptor_.open(endpoint.protocol(), ec);
        
        if (!ec) {
            acceptor_.set_option(asio::socket_base::reuse_address(true), ec);
            acceptor_.bind(endpoint, ec);
            if (!ec) {
                acceptor_.listen(asio::socket_base::max_listen_connections, ec);
            }
        }
        if (ec) {
                std::string errmsg = "Listener error: ";
                errmsg += ec.message();
                errmsg += "\n";
                throw HttpListenerException(errmsg);
        }
    }
    
    void run() {
        if (acceptor_.is_open()) {
            doAccept();
        }
    }

    
private:
    void doAccept() {
        acceptor_.async_accept(
            [this, self = shared_from_this()](beast::error_code ec, tcp::socket socket) {
                if (!ec) {
                    std::make_shared<Session>(std::move(socket), server_)->run();
                }
                doAccept();
            });
    }
    
    asio::io_context& ioc_;
    tcp::acceptor acceptor_;
    HttpServer* server_;
};

// HttpServer implementation - constructor (specifies listening address and thread count)
HttpServer::HttpServer(const std::string& address, unsigned short port, unsigned int threads)
    : address_(address), port_(port), threads_(threads), 
      ioc_(std::make_unique<asio::io_context>(threads)) {
    if (!ioc_) {
        throw CreateObjectFailed("Failed to create io_context");
    }
}

HttpServer::~HttpServer() {
    stop();
}

void HttpServer::start() {
    if (running_) {
        return;
    }
    
    try {
        // Create endpoint (using specified address and port)
        asio::ip::address ip_address = asio::ip::make_address(address_);
        tcp::endpoint endpoint(ip_address, port_);
        
        // Create listener
        listener_ = std::make_shared<Listener>(*ioc_, endpoint, this);

        if (!listener_) {
            throw CreateObjectFailed("Failed to create listener");
        }
        listener_->run();
        
        running_ = true;
        
        // Start worker threads
        for (unsigned int i = 0; i < threads_; ++i) {
            worker_threads_.emplace_back([this]() {
                try {
                    ioc_->run();
                } catch (const std::exception& e) {
                    std::cerr << "Thread exception: " << e.what() << std::endl;
                }
            });
        }
        
        std::cout << "HTTP Server started on " << address_ << ":" << port_ 
                  << " with " << threads_ << " threads" << std::endl;
    } catch (const std::exception& e) {
        std::cerr << "Failed to start server: " << e.what() << std::endl;
        running_ = false;
    }
}

void HttpServer::stop() {
    if (!running_) {
        return;
    }
    
    running_ = false;
    
    if (ioc_) {
        ioc_->stop();
    }
    
    for (auto& thread : worker_threads_) {
        if (thread.joinable()) {
            thread.join();
        }
    }
    worker_threads_.clear();
    
    std::cout << "HTTP Server stopped" << std::endl;
}

// Convert wildcard pattern to regex
std::regex HttpServer::wildcardToRegex(const std::string& pattern) {
    std::string regex_pattern;
    regex_pattern.reserve(pattern.size() * 2);
    
    for (char c : pattern) {
        if (c == '*') {
            regex_pattern += ".*";
        } else if (c == '?') {
            regex_pattern += ".";
        } else if (c == '.' || c == '+' || c == '(' || c == ')' || 
                   c == '[' || c == ']' || c == '{' || c == '}' || 
                   c == '^' || c == '$' || c == '|' || c == '\\') {
            regex_pattern += '\\';
            regex_pattern += c;
        } else {
            regex_pattern += c;
        }
    }
    
    return std::regex(regex_pattern, std::regex::icase);
}

// Match path against regex pattern
bool HttpServer::matchPath(const std::string& path, const std::regex& pattern) {
    return std::regex_match(path, pattern);
}

// Find matching route
HttpRequestHandler HttpServer::findRoute(const std::string& method, const std::string& path) {
    const std::vector<RouteEntry>* handlers = nullptr;
    
    if (method == "GET") {
        handlers = &get_handlers_;
    } else if (method == "POST") {
        handlers = &post_handlers_;
    } else if (method == "PUT") {
        handlers = &put_handlers_;
    } else if (method == "DELETE") {
        handlers = &delete_handlers_;
    } else {
        return nullptr;
    }
    
    // First try exact match
    for (const auto& entry : *handlers) {
        if (!entry.is_wildcard && entry.path == path) {
            return entry.handler;
        }
    }
    
    // Then try wildcard match
    for (const auto& entry : *handlers) {
        if (entry.is_wildcard && matchPath(path, entry.pattern)) {
            return entry.handler;
        }
    }
    
    return nullptr;
}

// Register handlers with wildcard support
void HttpServer::get(const std::string& path, HttpRequestHandler handler) {
    RouteEntry entry;
    entry.path = path;
    entry.handler = handler;
    entry.is_wildcard = (path.find('*') != std::string::npos || path.find('?') != std::string::npos);
    if (entry.is_wildcard) {
        entry.pattern = wildcardToRegex(path);
    }
    get_handlers_.push_back(entry);
}

void HttpServer::post(const std::string& path, HttpRequestHandler handler) {
    RouteEntry entry;
    entry.path = path;
    entry.handler = handler;
    entry.is_wildcard = (path.find('*') != std::string::npos || path.find('?') != std::string::npos);
    if (entry.is_wildcard) {
        entry.pattern = wildcardToRegex(path);
    }
    post_handlers_.push_back(entry);
}

void HttpServer::put(const std::string& path, HttpRequestHandler handler) {
    RouteEntry entry;
    entry.path = path;
    entry.handler = handler;
    entry.is_wildcard = (path.find('*') != std::string::npos || path.find('?') != std::string::npos);
    if (entry.is_wildcard) {
        entry.pattern = wildcardToRegex(path);
    }
    put_handlers_.push_back(entry);
}

void HttpServer::del(const std::string& path, HttpRequestHandler handler) {
    RouteEntry entry;
    entry.path = path;
    entry.handler = handler;
    entry.is_wildcard = (path.find('*') != std::string::npos || path.find('?') != std::string::npos);
    if (entry.is_wildcard) {
        entry.pattern = wildcardToRegex(path);
    }
    delete_handlers_.push_back(entry);
}

void HttpServer::use(MiddlewareHandler middleware) {
    middlewares_.push_back(middleware);
}

void HttpServer::setStaticDirectory(const std::string& path) {
    static_directory_ = path;
}

bool HttpServer::isRunning() const {
    return running_;
}

unsigned short HttpServer::getPort() const {
    return port_;
}

std::string HttpServer::getAddress() const {
    return address_;
}

void HttpServer::run() {
    start();
    if (ioc_) {
        ioc_->run();
    }
}

void HttpServer::showStatus() const {
    std::cout << "=== HTTP Server Status ===" << std::endl;
    std::cout << "Address: " << address_ << std::endl;
    std::cout << "Port: " << port_ << std::endl;
    std::cout << "Threads: " << threads_ << std::endl;
    std::cout << "Running: " << (running_ ? "Yes" : "No") << std::endl;
    std::cout << "Static Directory: " << (static_directory_.empty() ? "Not set" : static_directory_) << std::endl;
    std::cout << "Registered Routes:" << std::endl;
    std::cout << "  GET: " << get_handlers_.size() << " handler(s)" << std::endl;
    for (const auto& entry : get_handlers_) {
        std::cout << "    GET " << entry.path << (entry.is_wildcard ? " (wildcard)" : "") << std::endl;
    }
    std::cout << "  POST: " << post_handlers_.size() << " handler(s)" << std::endl;
    for (const auto& entry : post_handlers_) {
        std::cout << "    POST " << entry.path << (entry.is_wildcard ? " (wildcard)" : "") << std::endl;
    }
    std::cout << "  PUT: " << put_handlers_.size() << " handler(s)" << std::endl;
    for (const auto& entry : put_handlers_) {
        std::cout << "    PUT " << entry.path << (entry.is_wildcard ? " (wildcard)" : "") << std::endl;
    }
    std::cout << "  DELETE: " << delete_handlers_.size() << " handler(s)" << std::endl;
    for (const auto& entry : delete_handlers_) {
        std::cout << "    DELETE " << entry.path << (entry.is_wildcard ? " (wildcard)" : "") << std::endl;
    }
    std::cout << "Middlewares: " << middlewares_.size() << std::endl;
    std::cout << "==========================" << std::endl;
}

std::unordered_map<std::string, std::string> HttpServer::parseQueryParams(const std::string& query) {
    std::unordered_map<std::string, std::string> params;
    
    if (query.empty()) {
        return params;
    }
    
    std::vector<std::string> pairs;
    boost::split(pairs, query, boost::is_any_of("&"));
    
    for (const auto& pair : pairs) {
        std::vector<std::string> key_value;
        boost::split(key_value, pair, boost::is_any_of("="));
        
        if (key_value.size() == 2) {
            params[key_value[0]] = key_value[1];
        } else if (key_value.size() == 1) {
            params[key_value[0]] = "";
        }
    }
    
    return params;
}

http::response<http::string_body> HttpServer::handleRequest(const http::request<http::string_body>& req) {
    // Convert target() to string
    std::string target_path = std::string(req.target().data(), req.target().size());
    std::string query_string;
    
    // Split path and query parameters
    auto query_pos = target_path.find('?');
    if (query_pos != std::string::npos) {
        query_string = target_path.substr(query_pos + 1);
        target_path = target_path.substr(0, query_pos);
    }
    
    // Parse query parameters
    auto query_params = parseQueryParams(query_string);
    
    // Prepare response object
    http::response<http::string_body> response;
    response.version(req.version());
    response.set(http::field::server, "Boost.Beast HttpServer");
    response.set(http::field::content_type, "text/plain");
    response.keep_alive(req.keep_alive());
    
    // Execute middleware chain
    bool middleware_handled = false;
    for (const auto& middleware : middlewares_) {
        if (!middleware(req, response, query_params)) {
            middleware_handled = true;
            break;
        }
    }
    
    // If middleware handled the request, return directly
    if (middleware_handled) {
        response.prepare_payload();
        return response;
    }
    
    // Get HTTP method as string
    std::string method_str = http::to_string(req.method());
    
    // Find route handler (supports wildcards)
    HttpRequestHandler handler = findRoute(method_str, target_path);
    
    // Handle static files
    if (!handler && !static_directory_.empty()) {
        std::string file_path = static_directory_ + target_path;
        std::ifstream file(file_path, std::ios::binary);
        
        if (file.is_open()) {
            std::stringstream buffer;
            buffer << file.rdbuf();
            response.result(http::status::ok);
            response.body() = buffer.str();
            
            // Set Content-Type based on file extension
            if (boost::algorithm::ends_with(file_path, ".html")) {
                response.set(http::field::content_type, "text/html");
            } else if (boost::algorithm::ends_with(file_path, ".css")) {
                response.set(http::field::content_type, "text/css");
            } else if (boost::algorithm::ends_with(file_path, ".js")) {
                response.set(http::field::content_type, "application/javascript");
            } else if (boost::algorithm::ends_with(file_path, ".json")) {
                response.set(http::field::content_type, "application/json");
            } else if (boost::algorithm::ends_with(file_path, ".png")) {
                response.set(http::field::content_type, "image/png");
            } else if (boost::algorithm::ends_with(file_path, ".jpg") || 
                       boost::algorithm::ends_with(file_path, ".jpeg")) {
                response.set(http::field::content_type, "image/jpeg");
            }
            
            response.prepare_payload();
            return response;
        }
    }
    
    // If handler found, call it
    if (handler) {
        try {
            return handler(req, query_params);
        } catch (const std::exception& e) {
            std::cerr << "Handler exception: " << e.what() << std::endl;
            response.result(http::status::internal_server_error);
            response.body() = "500 Internal Server Error\n";
            response.prepare_payload();
            return response;
        }
    }
    
    // 404 Not Found
    response.result(http::status::not_found);
    response.body() = "404 Not Found\n";
    response.prepare_payload();
    return response;
}

void HttpServer::debug::print_all_header_fields(const http::request<http::string_body>& req) {
    for (auto const& field: req) {
        std::cout << "Name: " << field.name_string()
                  << ", Value: " << field.value() << std::endl;
    }
}

} // namespace net
} // namespace pmc
