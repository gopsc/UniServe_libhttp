#pragma once
#include <string>
#include <functional>
#include <map>
#include <curl/curl.h>

namespace pmc::net {

enum class HttpMethod {
    GET,
    POST,
    PUT,
    DELETE,
    PATCH,
    HEAD,
    OPTIONS
};

class HttpClient {
public:
    HttpClient() {
        curl_ = curl_easy_init();
        if (!curl_) {
            throw std::runtime_error("Failed to initialize CURL");
        }
    }
    
    ~HttpClient() {
        if (curl_) {
            curl_easy_cleanup(curl_);
        }
        if (headers_) {
            curl_slist_free_all(headers_);
        }
    }
    
    void set_header(const std::string& header) {
        headers_ = curl_slist_append(headers_, header.c_str());
    }
    
    void clear_headers() {
        if (headers_) {
            curl_slist_free_all(headers_);
            headers_ = nullptr;
        }
    }
    
    // Get response headers after request
    const std::map<std::string, std::string>& get_response_headers() const {
        return response_headers_;
    }
    
    // Get HTTP response code
    long get_response_code() const {
        return response_code_;
    }
    
    // Synchronous streaming request with specified method
    void request_stream_sync(
        HttpMethod method,
        const std::string& url,
        const std::string& data,
        std::function<void(const std::string&)> callback
    ) {
        if (!curl_) {
            throw std::runtime_error("CURL not initialized");
        }
        
        // Reset response data
        response_headers_.clear();
        response_code_ = 0;
        
        // Reset CURL handle
        curl_easy_reset(curl_);
        
        // Set URL
        curl_easy_setopt(curl_, CURLOPT_URL, url.c_str());
        
        // Set HTTP method
        set_http_method(method, data);
        
        // Set request headers
        curl_easy_setopt(curl_, CURLOPT_HTTPHEADER, headers_);
        
        // Set header callback to capture response headers
        curl_easy_setopt(curl_, CURLOPT_HEADERFUNCTION, header_callback);
        curl_easy_setopt(curl_, CURLOPT_HEADERDATA, this);
        
        // Set streaming callback
        std::string buffer;
        auto data_pair = std::make_pair(&buffer, &callback);
        
        curl_easy_setopt(curl_, CURLOPT_WRITEFUNCTION, stream_write_callback);
        curl_easy_setopt(curl_, CURLOPT_WRITEDATA, &data_pair);
        
        // Set timeout and connection options
        curl_easy_setopt(curl_, CURLOPT_TIMEOUT, 30L);
        curl_easy_setopt(curl_, CURLOPT_CONNECTTIMEOUT, 10L);
        
        // Execute request
        CURLcode res = curl_easy_perform(curl_);
        
        if (res != CURLE_OK) {
            throw std::runtime_error(std::string("CURL error: ") + curl_easy_strerror(res));
        }
        
        // Get response code
        curl_easy_getinfo(curl_, CURLINFO_RESPONSE_CODE, &response_code_);
    }
    
    // Convenience methods for common HTTP methods
    void get_stream_sync(const std::string& url, std::function<void(const std::string&)> callback) {
        request_stream_sync(HttpMethod::GET, url, "", callback);
    }
    
    void post_stream_sync(const std::string& url, const std::string& data, std::function<void(const std::string&)> callback) {
        request_stream_sync(HttpMethod::POST, url, data, callback);
    }
    
    void put_stream_sync(const std::string& url, const std::string& data, std::function<void(const std::string&)> callback) {
        request_stream_sync(HttpMethod::PUT, url, data, callback);
    }
    
    void delete_stream_sync(const std::string& url, std::function<void(const std::string&)> callback) {
        request_stream_sync(HttpMethod::DELETE, url, "", callback);
    }

private:
    CURL* curl_ = nullptr;
    struct curl_slist* headers_ = nullptr;
    std::map<std::string, std::string> response_headers_;
    long response_code_ = 0;
    
    void set_http_method(HttpMethod method, const std::string& data) {
        switch (method) {
            case HttpMethod::GET:
                curl_easy_setopt(curl_, CURLOPT_HTTPGET, 1L);
                break;
                
            case HttpMethod::POST:
                curl_easy_setopt(curl_, CURLOPT_POST, 1L);
                if (!data.empty()) {
                    curl_easy_setopt(curl_, CURLOPT_POSTFIELDS, data.c_str());
                    curl_easy_setopt(curl_, CURLOPT_POSTFIELDSIZE, data.size());
                }
                break;
                
            case HttpMethod::PUT:
                curl_easy_setopt(curl_, CURLOPT_CUSTOMREQUEST, "PUT");
                if (!data.empty()) {
                    curl_easy_setopt(curl_, CURLOPT_POSTFIELDS, data.c_str());
                    curl_easy_setopt(curl_, CURLOPT_POSTFIELDSIZE, data.size());
                }
                break;
                
            case HttpMethod::DELETE:
                curl_easy_setopt(curl_, CURLOPT_CUSTOMREQUEST, "DELETE");
                break;
                
            case HttpMethod::PATCH:
                curl_easy_setopt(curl_, CURLOPT_CUSTOMREQUEST, "PATCH");
                if (!data.empty()) {
                    curl_easy_setopt(curl_, CURLOPT_POSTFIELDS, data.c_str());
                    curl_easy_setopt(curl_, CURLOPT_POSTFIELDSIZE, data.size());
                }
                break;
                
            case HttpMethod::HEAD:
                curl_easy_setopt(curl_, CURLOPT_NOBODY, 1L);
                break;
                
            case HttpMethod::OPTIONS:
                curl_easy_setopt(curl_, CURLOPT_CUSTOMREQUEST, "OPTIONS");
                break;
        }
    }
    
    static size_t header_callback(char* buffer, size_t size, size_t nitems, void* userdata) {
        size_t total_size = size * nitems;
        HttpClient* client = static_cast<HttpClient*>(userdata);
        
        std::string header_line(buffer, total_size);
        
        // Parse header line (format: "Key: Value\r\n")
        size_t colon_pos = header_line.find(':');
        if (colon_pos != std::string::npos) {
            std::string key = header_line.substr(0, colon_pos);
            std::string value = header_line.substr(colon_pos + 1);
            
            // Trim whitespace
            key.erase(0, key.find_first_not_of(" \t"));
            key.erase(key.find_last_not_of(" \t") + 1);
            value.erase(0, value.find_first_not_of(" \t"));
            value.erase(value.find_last_not_of(" \t\r\n") + 1);
            
            if (!key.empty() && !value.empty()) {
                client->response_headers_[key] = value;
            }
        }
        
        return total_size;
    }
    
    static size_t stream_write_callback(char* ptr, size_t size, size_t nmemb, void* userdata) {
        auto* data_pair = static_cast<std::pair<std::string*, std::function<void(const std::string&)>*>*>(userdata);
        if (!data_pair || !data_pair->first || !data_pair->second) {
            return 0;
        }
        
        std::string& buffer = *(data_pair->first);
        std::function<void(const std::string&)>& callback = *(data_pair->second);
        size_t total_size = size * nmemb;
        
        // Append data to buffer
        buffer.append(ptr, total_size);
        
        // Process streaming data (line by line for Server-Sent Events format)
        size_t pos = 0;
        size_t newline_pos;
        
        while ((newline_pos = buffer.find('\n', pos)) != std::string::npos) {
            std::string line = buffer.substr(pos, newline_pos - pos);
            pos = newline_pos + 1;
            
            // Process SSE format data (starts with "data: ")
            if (line.rfind("data: ", 0) == 0) {
                std::string json_str = line.substr(6);
                
                if (json_str == "[DONE]") {
                    callback("[[Stream transmission complete]]");
                    continue;
                }
                
                // Call callback to process JSON data
                callback(json_str);
            } else if (!line.empty()) {
                // If not SSE format, pass directly to callback (may be error message)
                callback(line);
            }
        }
        
        // Remove processed data
        if (pos > 0) {
            buffer.erase(0, pos);
        }
        
        return total_size;
    }
};

} // namespace pmc::net
