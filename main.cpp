#include <iostream>
#include <unordered_map>
#include <list>
#include <thread>
#include <mutex>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <unistd.h>
#include <cstring>
#include <cstdlib>
#include <netdb.h>
#include <vector>

#define PORT 8080
#define CACHE_SIZE 5
#define BUFFER_SIZE 4096

std::mutex cacheMutex;

class DNSCache {
public:
    std::unordered_map<std::string, std::string> cache;

    std::string resolveDomain(const std::string& domain) {
        if (cache.find(domain) != cache.end()) {
            return cache[domain];
        }

        // Simulate DNS lookup with delay and possible failure
        std::this_thread::sleep_for(std::chrono::milliseconds(rand() % 200 + 100));
        if (rand() % 5 == 0) {
            throw std::runtime_error("DNS lookup failed!");
        }

        struct hostent* host = gethostbyname(domain.c_str());
        if (host == nullptr) {
            throw std::runtime_error("DNS lookup failed!");
        }

        std::string ip = inet_ntoa(*(struct in_addr*)host->h_addr);
        std::cout << "got ip address for domain: " << domain << " " << ip << "\n";
        cache[domain] = ip;

        std::cout << "values of cache currently \n";
        for(auto i: cache) std::cout << i.first << " " << i.second << "\n";

        return ip;
    }
};

class Cache {
    std::list<std::string> pages; // Stores URLs
    std::unordered_map<std::string, std::string> pageContent; // URL -> Page content
    size_t capacity;

public:
    Cache(size_t capacity) : capacity(capacity) {}

    std::string get(const std::string& url) {
        std::lock_guard<std::mutex> guard(cacheMutex);
        if (pageContent.find(url) != pageContent.end()) {
            // Move accessed page to front
            pages.remove(url);
            pages.push_front(url);
            return pageContent[url];
        }
        return "";
    }

    void put(const std::string& url, const std::string& content) {
        std::lock_guard<std::mutex> guard(cacheMutex);
        if (pageContent.find(url) != pageContent.end()) {
            // Move accessed page to front
            pages.remove(url);
        } else {
            if (pages.size() == capacity) {
                std::string lru = pages.back();
                pages.pop_back();
                pageContent.erase(lru);
            }
        }
        pages.push_front(url);
        pageContent[url] = content;
    }

    void display() {
        std::lock_guard<std::mutex> guard(cacheMutex);
        // for (const auto& url : pages) {
        //     std::cout << url << "\n" <<  << std::endl;
        // }
        for(auto i: pageContent){
            std::cout << i.first << "\n" << i.second << "\n\n";
        }
    }
};

std::string fetchFromServer(const std::string& ip, const std::string& path, const std::string& host) {
    int sock = socket(AF_INET, SOCK_STREAM, 0);
    if (sock < 0) {
        throw std::runtime_error("Socket creation error");
    }

    struct sockaddr_in serv_addr;
    serv_addr.sin_family = AF_INET;
    serv_addr.sin_port = htons(80); // HTTP port

    if (inet_pton(AF_INET, ip.c_str(), &serv_addr.sin_addr) <= 0) {
        throw std::runtime_error("Invalid address/ Address not supported");
    }

    if (connect(sock, (struct sockaddr*)&serv_addr, sizeof(serv_addr)) < 0) {
        throw std::runtime_error("Connection Failed");
    }

    std::string request = "GET " + path + " HTTP/1.1\r\nHost: " + host + "\r\nConnection: close\r\n\r\n";
    send(sock, request.c_str(), request.size(), 0);

    char buffer[BUFFER_SIZE];
    std::string response;
    int bytesRead;
    while ((bytesRead = read(sock, buffer, BUFFER_SIZE)) > 0) {
        response.append(buffer, bytesRead);
    }

    close(sock);
    return response;
}

void handleClient(int clientSocket, DNSCache& dnsCache, Cache& cache) {
    char buffer[BUFFER_SIZE] = {0};
    read(clientSocket, buffer, BUFFER_SIZE);

    std::string request(buffer);

    // Extract the full URL from the request
    size_t pos1 = request.find("GET ") + 4;
    size_t pos2 = request.find(" ", pos1);
    if (pos1 == std::string::npos || pos2 == std::string::npos) {
        std::cerr << "Invalid request format" << std::endl;
        close(clientSocket);
        return;
    }

    std::string fullUrl = request.substr(pos1, pos2 - pos1);

    // Extract the domain and path
    size_t protocolEnd = fullUrl.find("://");
    std::string url = (protocolEnd != std::string::npos) ? fullUrl.substr(protocolEnd + 3) : fullUrl;

    size_t pathStart = url.find('/');
    std::string domain, path;

    if (pathStart != std::string::npos) {
        domain = url.substr(0, pathStart);
        path = url.substr(pathStart);
    } else {
        domain = url;
        path = "/";
    }

    std::string pageContent = cache.get(fullUrl);
    
    if (pageContent.empty()) {
        try {
            std::string ip = dnsCache.resolveDomain(domain);
            pageContent = fetchFromServer(ip, path, domain);
            cache.put(fullUrl, pageContent);
        } catch (const std::exception& e) {
            std::cerr << "Error: " << e.what() << std::endl;
            close(clientSocket);
            return;
        }
    }

    send(clientSocket, pageContent.c_str(), pageContent.size(), 0);
    close(clientSocket);

    std::cout << "DISPLAYING CACHE CONTENT:\n";
    cache.display();
}

int main() {
    srand(time(0));

    int server_fd, clientSocket;
    struct sockaddr_in address;
    int opt = 1;
    int addrlen = sizeof(address);

    if ((server_fd = socket(AF_INET, SOCK_STREAM, 0)) == 0) {
        perror("Socket failed");
        exit(EXIT_FAILURE);
    }

    if (setsockopt(server_fd, SOL_SOCKET, SO_REUSEADDR | SO_REUSEPORT, &opt, sizeof(opt))) {
        perror("Setsockopt failed");
        exit(EXIT_FAILURE);
    }

    address.sin_family = AF_INET;
    address.sin_addr.s_addr = INADDR_ANY;
    address.sin_port = htons(PORT);

    if (bind(server_fd, (struct sockaddr*)&address, sizeof(address)) < 0) {
        perror("Bind failed");
        exit(EXIT_FAILURE);
    }

    if (listen(server_fd, 3) < 0) {
        perror("Listen failed");
        exit(EXIT_FAILURE);
    }

    DNSCache dnsCache;
    Cache cache(CACHE_SIZE);

    while (true) {
        if ((clientSocket = accept(server_fd, (struct sockaddr*)&address, (socklen_t*)&addrlen)) < 0) {
            perror("Accept failed");
            exit(EXIT_FAILURE);
        }

        std::thread clientThread(handleClient, clientSocket, std::ref(dnsCache), std::ref(cache));
        clientThread.detach();
    }

    return 0;
}
