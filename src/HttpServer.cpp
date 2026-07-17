#include "us/HttpServer.hpp"
#include <boost/algorithm/string.hpp>
#include <boost/asio/ssl/verify_context.hpp>
#include <fstream>
#include <sstream>
#include <iomanip>

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

/* SSL initialization exception */
class SSLInitException: public HttpServerException {
    public:
        explicit SSLInitException(const std::string& msg)
        : HttpServerException(msg) {}
};

/* Failed to create IO_CONTEXT */
class CreateObjectFailed: public HttpServerException {
    public:
        explicit CreateObjectFailed(const std::string& msg)
        : HttpServerException(msg) {}
};

// Session class - handles individual HTTP/HTTPS connections
class HttpServer::Session : public std::enable_shared_from_this<Session> {
public:
    // HTTP constructor (without SSL) - 使用移动语义
    Session(tcp::socket&& socket, HttpServer* server)
        : socket_(std::move(socket)), 
          server_(server), 
          ssl_enabled_(false) {
    }
    
    // HTTPS constructor (with SSL) - 使用移动语义
    Session(tcp::socket&& socket, asio::ssl::context& ssl_ctx, HttpServer* server)
        : socket_(std::move(socket)), 
          server_(server), 
          ssl_enabled_(true) {
        // 在构造函数中初始化 SSL stream，使用已移动的 socket
        ssl_stream_ = std::make_unique<asio::ssl::stream<tcp::socket>>(
            std::move(socket_), ssl_ctx);
    }
    
    ~Session() {
        close();
    }
    
    void run() {
        if (ssl_enabled_) {
            doSSLHandshake();
        } else {
            doRead();
        }
    }
    
private:
    void close() {
        beast::error_code ec;
        if (ssl_enabled_ && ssl_stream_) {
            ssl_stream_->lowest_layer().shutdown(tcp::socket::shutdown_send, ec);
            ssl_stream_->lowest_layer().close(ec);
        } else if (socket_.is_open()) {
            socket_.shutdown(tcp::socket::shutdown_send, ec);
            socket_.close(ec);
        }
    }
    
    void doSSLHandshake() {
        auto self = shared_from_this();
        ssl_stream_->async_handshake(
            asio::ssl::stream_base::server,
            [this, self](beast::error_code ec) {
                if (!ec) {
                    doRead();
                } else {
                    std::cerr << "SSL handshake error: " << ec.message() << std::endl;
                }
            });
    }
    
    void doRead() {
        auto self = shared_from_this();
        
        if (ssl_enabled_ && ssl_stream_) {
            http::async_read(*ssl_stream_, buffer_, request_,
                [this, self](beast::error_code ec, std::size_t bytes_transferred) {
                    handleRead(ec);
                });
        } else {
            http::async_read(socket_, buffer_, request_,
                [this, self](beast::error_code ec, std::size_t bytes_transferred) {
                    handleRead(ec);
                });
        }
    }
    
    void handleRead(beast::error_code ec) {
        if (!ec) {
            // 复制请求对象，避免被后续请求覆盖
            http::request<http::string_body> req_copy = std::move(request_);
            request_ = http::request<http::string_body>();
            processRequest(std::move(req_copy));
        } else if (ec != http::error::end_of_stream) {
            std::cerr << "Read error: " << ec.message() << std::endl;
        }
    }
    
    void processRequest(http::request<http::string_body> req) {
        try {
            auto response = std::make_shared<http::response<http::string_body>>(
                server_->handleRequest(req)
            );
            doWrite(response);
        } catch (const std::exception& e) {
            std::cerr << "Error processing request: " << e.what() << std::endl;
            auto error_response = std::make_shared<http::response<http::string_body>>();
            error_response->version(req.version());
            error_response->result(http::status::internal_server_error);
            error_response->set(http::field::content_type, "text/plain");
            error_response->body() = "500 Internal Server Error\n";
            error_response->prepare_payload();
            doWrite(error_response);
        }
    }
    
    void doWrite(std::shared_ptr<http::response<http::string_body>> response) {
        auto self = shared_from_this();
        
        auto write_callback = [this, self, response](beast::error_code ec, std::size_t) {
            if (ec) {
                std::cerr << "Write error: " << ec.message() << std::endl;
                return;
            }
            
            bool keep_alive = response->keep_alive();
            if (!keep_alive) {
                close();
            } else {
                doRead();
            }
        };
        
        if (ssl_enabled_ && ssl_stream_) {
            http::async_write(*ssl_stream_, *response, write_callback);
        } else {
            http::async_write(socket_, *response, write_callback);
        }
    }
    
    // HTTP socket (non-SSL) - 注意：对于SSL连接，这个socket会被移动到ssl_stream_中
    tcp::socket socket_;
    
    // SSL stream (SSL) - 使用unique_ptr管理
    std::unique_ptr<asio::ssl::stream<tcp::socket>> ssl_stream_;
    
    beast::flat_buffer buffer_;
    http::request<http::string_body> request_;
    HttpServer* server_;
    bool ssl_enabled_;
};

// Listener class - accepts new connections
class HttpServer::Listener : public std::enable_shared_from_this<Listener> {
public:
    // HTTP listener
    Listener(asio::io_context& ioc, tcp::endpoint endpoint, HttpServer* server)
        : ioc_(ioc), acceptor_(ioc), server_(server), ssl_enabled_(false) {
        initAcceptor(endpoint);
    }
    
    // HTTPS listener
    Listener(asio::io_context& ioc, tcp::endpoint endpoint, 
             asio::ssl::context& ssl_ctx, HttpServer* server)
        : ioc_(ioc), acceptor_(ioc), ssl_ctx_(&ssl_ctx), 
          server_(server), ssl_enabled_(true) {
        initAcceptor(endpoint);
    }
    
    void run() {
        if (acceptor_.is_open()) {
            doAccept();
        }
    }
    
private:
    void initAcceptor(tcp::endpoint endpoint) {
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
    
    void doAccept() {
        acceptor_.async_accept(
            [this, self = shared_from_this()](beast::error_code ec, tcp::socket socket) {
                if (!ec) {
                    if (ssl_enabled_ && ssl_ctx_) {
                        std::make_shared<Session>(std::move(socket), *ssl_ctx_, server_)->run();
                    } else {
                        std::make_shared<Session>(std::move(socket), server_)->run();
                    }
                } else {
                    std::cerr << "Accept error: " << ec.message() << std::endl;
                }
                doAccept();
            });
    }
    
    asio::io_context& ioc_;
    tcp::acceptor acceptor_;
    asio::ssl::context* ssl_ctx_{nullptr};
    HttpServer* server_;
    bool ssl_enabled_;
};

// HttpServer implementation - HTTP constructor
HttpServer::HttpServer(const std::string& address, unsigned short port, unsigned int threads)
    : address_(address), port_(port), threads_(threads), ssl_enabled_(false),
      ioc_(std::make_unique<asio::io_context>(threads)) {
    if (!ioc_) {
        throw CreateObjectFailed("Failed to create io_context");
    }
}

// HttpServer implementation - HTTPS constructor
HttpServer::HttpServer(const std::string& address, unsigned short port, 
                       const std::string& cert_file, const std::string& key_file,
                       unsigned int threads)
    : address_(address), port_(port), threads_(threads), 
      ssl_enabled_(true), cert_file_(cert_file), key_file_(key_file),
      ioc_(std::make_unique<asio::io_context>(threads)) {
    if (!ioc_) {
        throw CreateObjectFailed("Failed to create io_context");
    }
    initSSLContext();
}

HttpServer::~HttpServer() {
    stop();
}

void HttpServer::initSSLContext() {
    if (!ssl_enabled_) return;
    
    try {
        ssl_ctx_ = std::make_unique<asio::ssl::context>(asio::ssl::context::tlsv12);
        
        // 设置SSL选项
        ssl_ctx_->set_options(
            asio::ssl::context::default_workarounds |
            asio::ssl::context::no_sslv2 |
            asio::ssl::context::no_sslv3 |
            asio::ssl::context::single_dh_use);
        
        // 加载证书文件
        ssl_ctx_->use_certificate_chain_file(cert_file_);
        
        // 加载私钥文件
        ssl_ctx_->use_private_key_file(key_file_, asio::ssl::context::pem);
        
        std::cout << "SSL/TLS initialized successfully" << std::endl;
    } catch (const std::exception& e) {
        std::cerr << "SSL initialization failed: " << e.what() << std::endl;
        throw SSLInitException("Failed to initialize SSL context");
    }
}

void HttpServer::start() {
    if (running_) {
        return;
    }
    
    try {
        // Create endpoint
        asio::ip::address ip_address = asio::ip::make_address(address_);
        tcp::endpoint endpoint(ip_address, port_);
        
        // Create listener based on SSL configuration
        if (ssl_enabled_ && ssl_ctx_) {
            listener_ = std::make_shared<Listener>(*ioc_, endpoint, *ssl_ctx_, this);
        } else {
            listener_ = std::make_shared<Listener>(*ioc_, endpoint, this);
        }
        
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
        
        std::cout << (ssl_enabled_ ? "HTTPS" : "HTTP") << " Server started on " 
                  << address_ << ":" << port_ << " with " << threads_ << " threads" << std::endl;
        if (ssl_enabled_) {
            std::cout << "SSL Certificate: " << cert_file_ << std::endl;
            std::cout << "SSL Private Key: " << key_file_ << std::endl;
        }
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
    
    // 关闭所有会话
    closeAllSessions();
    
    if (ioc_) {
        ioc_->stop();
    }
    
    for (auto& thread : worker_threads_) {
        if (thread.joinable()) {
            thread.join();
        }
    }
    worker_threads_.clear();
    
    if (listener_) {
        listener_.reset();
    }
    
    std::cout << (ssl_enabled_ ? "HTTPS" : "HTTP") << " Server stopped" << std::endl;
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

bool HttpServer::isSSLEnabled() const {
    return ssl_enabled_;
}

void HttpServer::run() {
    start();
    if (ioc_) {
        ioc_->run();
    }
}

void HttpServer::showStatus() const {
    std::cout << "=== " << (ssl_enabled_ ? "HTTPS" : "HTTP") << " Server Status ===" << std::endl;
    std::cout << "Protocol: " << (ssl_enabled_ ? "HTTPS (SSL/TLS)" : "HTTP") << std::endl;
    std::cout << "Address: " << address_ << std::endl;
    std::cout << "Port: " << port_ << std::endl;
    std::cout << "Threads: " << threads_ << std::endl;
    std::cout << "Running: " << (running_ ? "Yes" : "No") << std::endl;
    if (ssl_enabled_) {
        std::cout << "Certificate: " << cert_file_ << std::endl;
        std::cout << "Private Key: " << key_file_ << std::endl;
    }
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

void HttpServer::addSession(const std::shared_ptr<Session>& session) {
    std::lock_guard<std::mutex> lock(sessions_mutex_);
    sessions_.erase(
        std::remove_if(sessions_.begin(), sessions_.end(),
            [](const std::weak_ptr<Session>& wp) { return wp.expired(); }),
        sessions_.end()
    );
    sessions_.push_back(session);
}

void HttpServer::removeSession(const Session* session) {
    std::lock_guard<std::mutex> lock(sessions_mutex_);
    sessions_.erase(
        std::remove_if(sessions_.begin(), sessions_.end(),
            [session](const std::weak_ptr<Session>& wp) {
                auto sp = wp.lock();
                return !sp || sp.get() == session;
            }),
        sessions_.end()
    );
}

void HttpServer::closeAllSessions() {
    std::lock_guard<std::mutex> lock(sessions_mutex_);
    for (auto& weak_session : sessions_) {
        auto session = weak_session.lock();
        if (session) {
            // 会话会在析构时自动关闭socket
        }
    }
    sessions_.clear();
}

void HttpServer::debug::print_all_header_fields(const http::request<http::string_body>& req) {
    for (auto const& field: req) {
        std::cout << "Name: " << field.name_string()
                  << ", Value: " << field.value() << std::endl;
    }
}

} // namespace net
} // namespace pmc
