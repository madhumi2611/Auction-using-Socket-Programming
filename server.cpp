#include <bits/stdc++.h>
#include <thread>
#include <mutex>
#include <chrono>
#include <sys/socket.h>
#include <netinet/in.h>
#include <unistd.h>

using namespace std;

enum Status { PENDING, ACTIVE, SOLD, EXPIRED };

struct Item {
    int id;
    string name, desc;
    double basePrice, currentBid;
    string highestBidder;
    Status status;
    
    Item(int i, string n, string d, double p) 
        : id(i), name(n), desc(d), basePrice(p), currentBid(p), status(PENDING) {}
};

struct Client {
    int socket;
    string username;
};

class AuctionServer {
private:
    int serverSocket;
    vector<Item> items;
    vector<Client> clients;
    mutex mtx;
    int nextId = 1;
    bool running = true;

public:
    AuctionServer() {
        items.push_back(Item(nextId++, "Watch", "Vintage pocket watch", 100.0));
        items.push_back(Item(nextId++, "Painting", "Oil painting", 250.0));
        items.push_back(Item(nextId++, "Book", "Rare first edition", 75.0));
    }
    
    bool start(int port) {
        serverSocket = socket(AF_INET, SOCK_STREAM, 0);
        if (serverSocket < 0) return false;
        
        int opt = 1;
        setsockopt(serverSocket, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));
        
        sockaddr_in addr{AF_INET, htons(port), {INADDR_ANY}};
        if (bind(serverSocket, (sockaddr*)&addr, sizeof(addr)) < 0) return false;
        if (listen(serverSocket, 10) < 0) return false;
        
        cout << "Server started on port " << port << endl;
        return true;
    }
    
    void acceptClients() {
        while (running) {
            sockaddr_in clientAddr{};
            socklen_t size = sizeof(clientAddr);
            int clientSock = accept(serverSocket, (sockaddr*)&clientAddr, &size);
            if (clientSock >= 0) {
                thread(&AuctionServer::handleClient, this, clientSock).detach();
            }
        }
    }
    
    void handleClient(int sock) {
        char buffer[1024];
        
        send(sock, "Username: ", 10, 0);
        int bytes = recv(sock, buffer, sizeof(buffer)-1, 0);
        if (bytes <= 0) { close(sock); return; }
        
        buffer[bytes] = '\0';
        string username(buffer);
        username.erase(username.find_last_not_of(" \n\r\t") + 1);
        
        {
            lock_guard<mutex> lock(mtx);
            clients.push_back({sock, username});
        }
        
        send(sock, ("Welcome " + username + "!\n").c_str(), username.length() + 10, 0);
        
        while (running) {
            bytes = recv(sock, buffer, sizeof(buffer)-1, 0);
            if (bytes <= 0) break;
            buffer[bytes] = '\0';
            processCommand(sock, username, string(buffer));
        }
        
        {
            lock_guard<mutex> lock(mtx);
            clients.erase(remove_if(clients.begin(), clients.end(),
                [sock](const Client& c) { return c.socket == sock; }), clients.end());
        }
        close(sock);
    }
    
    void processCommand(int sock, const string& user, const string& cmd) {
        istringstream iss(cmd);
        string action;
        iss >> action;
        
        if (action == "list") {
            string response = "\n=== ITEMS ===\n";
            lock_guard<mutex> lock(mtx);
            for (const auto& item : items) {
                response += "ID:" + to_string(item.id) + " " + item.name + 
                           " $" + to_string(item.currentBid);
                if (!item.highestBidder.empty()) 
                    response += " (" + item.highestBidder + ")";
                response += " Status:" + to_string(item.status) + "\n";
            }
            send(sock, response.c_str(), response.length(), 0);
        }
        else if (action == "start") {
            int id;
            if (iss >> id) startAuction(sock, id);
        }
        else if (action == "bid") {
            int id;
            double amount;
            if (iss >> id >> amount) placeBid(sock, user, id, amount);
        }
        else if (action == "add") {
            string name, desc;
            double price;
            if (iss >> name >> desc >> price) {
                lock_guard<mutex> lock(mtx);
                items.push_back(Item(nextId++, name, desc, price));
                send(sock, "Item added!\n", 12, 0);
            }
        }
        else {
            send(sock, "Commands: list, start <id>, bid <id> <amount>, add <n> <d> <p>\n", 60, 0);
        }
    }
    
    void startAuction(int sock, int id) {
        lock_guard<mutex> lock(mtx);
        for (auto& item : items) {
            if (item.id == id && item.status == PENDING) {
                item.status = ACTIVE;
                send(sock, ("Started auction for " + item.name + "\n").c_str(), 
                     item.name.length() + 20, 0);
                broadcast("AUCTION: " + item.name + " started!, Price: " + to_string(item.basePrice));
                
                thread([this, id]() {
                    this_thread::sleep_for(chrono::minutes(1)); // Shortened for demo
                    lock_guard<mutex> lock(mtx);
                    for (auto& item : items) {
                        if (item.id == id && item.status == ACTIVE) {
                            item.status = item.highestBidder.empty() ? EXPIRED : SOLD;
                            broadcast("ENDED: " + item.name + 
                                    (item.status == SOLD ? " sold to " + item.highestBidder : " expired"));
                        }
                    }
                }).detach();
                return;
            }
        }
        send(sock, "Cannot start auction\n", 21, 0);
    }
    
    void placeBid(int sock, const string& user, int id, double amount) {
        lock_guard<mutex> lock(mtx);
        for (auto& item : items) {
            if (item.id == id && item.status == ACTIVE && amount > item.currentBid) {
                item.currentBid = amount;
                item.highestBidder = user;
                send(sock, "Bid accepted!\n", 14, 0);
                broadcast("BID: " + item.name + " $" + to_string(amount) + " by " + user);
                return;
            }
        }
        send(sock, "Invalid bid\n", 12, 0);
    }
    
    void broadcast(const string& msg) {
        string fullMsg = "[" + msg + "]\n";
        for (const auto& client : clients) {
            send(client.socket, fullMsg.c_str(), fullMsg.length(), 0);
        }
    }
    
    void stop() { running = false; close(serverSocket); }
};

int main() {
    AuctionServer server;
    if (!server.start(8080)) return -1;
    
    thread(&AuctionServer::acceptClients, &server).detach();
    cout << "Press Enter to stop...\n";
    cin.get();
    server.stop();
    return 0;
}