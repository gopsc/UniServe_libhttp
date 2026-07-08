#ifndef PMC_HTTP_CLIENT_HPP
#define PMC_HTTP_CLIENT_HPP

#include <string>
#include <functional>
#include <curl/curl.h>
#include <iostream>

namespace pmc::net {

class HttpClient {
public:
    HttpClient() {
        std::cout << "HttpClient constructor: start initialization" << std::endl;
        curl_ = curl_easy_init();
        if (!curl_) {
            throw std::runtime_error("Failed to initialize CURL");
        }
        std::cout << "HttpClient constructor: initialization complete" << std::endl;
    }
    
    ~HttpClient() {
        std::cout << "HttpClient destructor: start cleanup" << std::endl;
        if (curl_) {
            curl_easy_cleanup(curl_);
        }
        if (headers_) {
            curl_slist_free_all(headers_);
        }
        std::cout << "HttpClient destructor: cleanup complete" << std::endl;
    }
    
    void set_header(const std::string& header) {
        std::cout << "set_header: add header: " << header << std::endl;
        headers_ = curl_slist_append(headers_, header.c_str());
    }
    
    // Synchronous streaming request
    void post_stream_sync(const std::string& url, const std::string& data, std::function<void(const std::string&)> callback) {
        std::cout << "post_stream_sync: start streaming request to " << url << std::endl;
        
        if (!curl_) {
            throw std::runtime_error("CURL not initialized");
        }
        
        // Reset CURL handle
        curl_easy_reset(curl_);
        
        // Set URL
        curl_easy_setopt(curl_, CURLOPT_URL, url.c_str());
        
        // Set POST request
        curl_easy_setopt(curl_, CURLOPT_POST, 1L);
        curl_easy_setopt(curl_, CURLOPT_POSTFIELDS, data.c_str());
        curl_easy_setopt(curl_, CURLOPT_POSTFIELDSIZE, data.size());
        
        // Set request headers
        curl_easy_setopt(curl_, CURLOPT_HTTPHEADER, headers_);
        
        // Set streaming callback
        std::string buffer;
        auto data_pair = std::make_pair(&buffer, &callback);
        std::cout << "post_stream_sync: data pair created" << std::endl;
        
        curl_easy_setopt(curl_, CURLOPT_WRITEFUNCTION, stream_write_callback);
        curl_easy_setopt(curl_, CURLOPT_WRITEDATA, &data_pair);
        
        // Set timeout and connection options
        curl_easy_setopt(curl_, CURLOPT_TIMEOUT, 30L);
        curl_easy_setopt(curl_, CURLOPT_CONNECTTIMEOUT, 10L);
        
        // Execute request
        std::cout << "post_stream_sync: executing request" << std::endl;
        CURLcode res = curl_easy_perform(curl_);
        std::cout << "post_stream_sync: data pair cleanup complete" << std::endl;
        
        if (res != CURLE_OK) {
            std::cout << "post_stream_sync: CURL error: " << curl_easy_strerror(res) << std::endl;
            throw std::runtime_error(std::string("CURL error: ") + curl_easy_strerror(res));
        }
        
        std::cout << "post_stream_sync: request successful" << std::endl;
    }

private:
    CURL* curl_ = nullptr;
    struct curl_slist* headers_ = nullptr;
    
    static size_t stream_write_callback(char* ptr, size_t size, size_t nmemb, void* userdata) {
        // userdata points to std::pair<std::string*, std::function<void(const std::string&)>*>
        auto* data_pair = static_cast<std::pair<std::string*, std::function<void(const std::string&)>*>*>(userdata);
        if (!data_pair || !data_pair->first || !data_pair->second) {
            std::cout << "stream_write_callback: invalid user data" << std::endl;
            return 0;
        }
        
        std::string& buffer = *(data_pair->first);
        std::function<void(const std::string&)>& callback = *(data_pair->second);
        size_t total_size = size * nmemb;
        
        std::cout << "stream_write_callback: received " << total_size << " bytes data" << std::endl;
        
        // Append data to buffer
        buffer.append(ptr, total_size);
        
        // Print raw data for debugging
        std::string raw_data(ptr, total_size);
        std::cout << "stream_write_callback: raw data: " << raw_data << std::endl;
        
        // Process streaming data (line by line for Server-Sent Events format)
        size_t pos = 0;
        size_t newline_pos;
        
        while ((newline_pos = buffer.find('\n', pos)) != std::string::npos) {
            std::string line = buffer.substr(pos, newline_pos - pos);
            pos = newline_pos + 1;
            
            std::cout << "stream_write_callback: processing line: " << line << std::endl;
            
            // Process SSE format data (starts with "data: ")
            if (line.rfind("data: ", 0) == 0) {
                std::string json_str = line.substr(6);
                
                if (json_str == "[DONE]") {
                    std::cout << "stream_write_callback: received [DONE]" << std::endl;
                    callback("[[Stream transmission complete]]");
                    continue;
                }
                
                // Call callback to process JSON data
                std::cout << "stream_write_callback: calling callback, data length: " << json_str.length() << std::endl;
                callback(json_str);
            } else if (!line.empty()) {
                // If not SSE format, pass directly to callback (may be error message)
                std::cout << "stream_write_callback: non-SSE data, passing directly: " << line << std::endl;
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

#endif // PMC_HTTP_CLIENT_HPP