#include <string>
#include <sstream>
#include <fstream>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <netdb.h>
#include <unistd.h>
#include <cstring>
#include <cstdlib>
#include <cstdio>

/* Forward declaration - error handling is done in c_ncnn.cpp */
extern std::string g_last_error;
extern void set_error(const std::string& msg);

/* Simple HTTP-only client (no HTTPS support) */
static bool parse_url(const std::string& url, std::string& host, std::string& path, int& port) {
    if (url.length() < 7 || url.substr(0, 7) != "http://") {
        set_error("Invalid URL: must start with http://");
        return false;
    }
    
    std::string url_part = url.substr(7);
    size_t slash_pos = url_part.find('/');
    size_t colon_pos = url_part.find(':');
    
    if (colon_pos != std::string::npos && (slash_pos == std::string::npos || colon_pos < slash_pos)) {
        // Has port
        host = url_part.substr(0, colon_pos);
        std::string port_str = url_part.substr(colon_pos + 1);
        if (slash_pos != std::string::npos) {
            port_str = port_str.substr(0, slash_pos - colon_pos - 1);
            path = url_part.substr(slash_pos);
        } else {
            path = "/";
        }
        port = std::stoi(port_str);
    } else {
        // No port, default to 80
        port = 80;
        if (slash_pos != std::string::npos) {
            host = url_part.substr(0, slash_pos);
            path = url_part.substr(slash_pos);
        } else {
            host = url_part;
            path = "/";
        }
    }
    
    return !host.empty();
}

static int connect_to_host(const std::string& host, int port) {
    struct addrinfo hints, *result, *rp;
    int sockfd;
    
    memset(&hints, 0, sizeof(hints));
    hints.ai_family = AF_INET;
    hints.ai_socktype = SOCK_STREAM;
    
    char port_str[16];
    snprintf(port_str, sizeof(port_str), "%d", port);
    
    int ret = getaddrinfo(host.c_str(), port_str, &hints, &result);
    if (ret != 0) {
        set_error("Failed to resolve host: " + host);
        return -1;
    }
    
    for (rp = result; rp != NULL; rp = rp->ai_next) {
        sockfd = socket(rp->ai_family, rp->ai_socktype, rp->ai_protocol);
        if (sockfd == -1) {
            continue;
        }
        
        if (connect(sockfd, rp->ai_addr, rp->ai_addrlen) != -1) {
            break;
        }
        
        close(sockfd);
    }
    
    freeaddrinfo(result);
    
    if (rp == NULL) {
        return -1;
    }
    
    return sockfd;
}

static bool http_download(const std::string& url, const std::string& output_path) {
    std::string host, path;
    int port;
    
    if (!parse_url(url, host, path, port)) {
        return false;
    }
    
    int sockfd = connect_to_host(host, port);
    if (sockfd < 0) {
        set_error("Failed to connect to " + host + ":" + std::to_string(port));
        return false;
    }
    
    // Build HTTP GET request
    std::ostringstream request;
    request << "GET " << path << " HTTP/1.1\r\n";
    request << "Host: " << host;
    if (port != 80) {
        request << ":" << port;
    }
    request << "\r\n";
    request << "Connection: close\r\n";
    request << "User-Agent: c_ncnn/1.0\r\n";
    request << "\r\n";
    
    std::string request_str = request.str();
    if (send(sockfd, request_str.c_str(), request_str.length(), 0) < 0) {
        close(sockfd);
        return false;
    }
    
    // Read response
    std::ofstream file(output_path, std::ios::binary);
    if (!file.is_open()) {
        close(sockfd);
        return false;
    }
    
    char buffer[8192];
    bool headers_done = false;
    int content_length = -1;
    std::string header_buffer;
    
    while (true) {
        ssize_t nread = recv(sockfd, buffer, sizeof(buffer), 0);
        if (nread <= 0) {
            break;
        }
        
        if (!headers_done) {
            header_buffer.append(buffer, nread);
            size_t header_end = header_buffer.find("\r\n\r\n");
            if (header_end != std::string::npos) {
                headers_done = true;
                std::string headers = header_buffer.substr(0, header_end);
                
                // Check status code
                if (headers.substr(0, 9) != "HTTP/1.1 " && headers.substr(0, 9) != "HTTP/1.0 ") {
                    // Try HTTP/1.0 format
                    if (headers.substr(0, 9) != "HTTP/1.0 ") {
                        close(sockfd);
                        file.close();
                        unlink(output_path.c_str());
                        return false;
                    }
                }
                
                std::string status_line = headers.substr(0, headers.find("\r\n"));
                if (status_line.find("200") == std::string::npos) {
                    close(sockfd);
                    file.close();
                    unlink(output_path.c_str());
                    return false;
                }
                
                // Parse headers
                size_t cl_pos = headers.find("Content-Length:");
                if (cl_pos != std::string::npos) {
                    size_t cl_start = cl_pos + 15;
                    while (cl_start < headers.length() && (headers[cl_start] == ' ' || headers[cl_start] == '\t')) {
                        cl_start++;
                    }
                    size_t cl_end = headers.find("\r\n", cl_start);
                    if (cl_end != std::string::npos) {
                        content_length = std::stoi(headers.substr(cl_start, cl_end - cl_start));
                    }
                }
                
                // Note: Chunked transfer encoding is not fully implemented
                // Content-Length is preferred for simplicity
                
                // Write remaining data after headers
                size_t body_start = header_end + 4;
                if (body_start < header_buffer.length()) {
                    file.write(header_buffer.c_str() + body_start, header_buffer.length() - body_start);
                }
            }
        } else {
            // Write body data
            file.write(buffer, nread);
            
            if (content_length > 0) {
                // Check if we've read all content
                file.seekp(0, std::ios::end);
                if (file.tellp() >= content_length) {
                    break;
                }
            }
        }
    }
    
    file.close();
    close(sockfd);
    
    return true;
}

/* Replace download_file to use custom HTTP client */
bool download_file_custom(const std::string& url, const std::string& output_path) {
    return http_download(url, output_path);
}

