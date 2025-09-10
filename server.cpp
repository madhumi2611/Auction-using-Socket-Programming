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
    string name;
    double basePrice, currentBid;
    string highestBidder;
    int highestBidderSocket;  // Track socket of highest bidder
    Status status;
    
    Item(int i, string n, double p) 
        : id(i), name(n), basePrice(p), currentBid(p), status(PENDING), highestBidderSocket(-1) {}
};

struct Client {
    int socket;
    string username;
    double budget;
    double totalSpent;  // Track total amount spent by client
    
    Client(int s, string u, double b) : socket(s), username(u), budget(b), totalSpent(0.0) {}
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
        items.push_back(Item(nextId++, "Watch", 100.0));
        items.push_back(Item(nextId++, "Painting", 250.0));
        items.push_back(Item(nextId++, "Book", 75.0));
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
        
        // Get username
        send(sock, "Username: ", 10, 0);
        int bytes = recv(sock, buffer, sizeof(buffer)-1, 0);
        if (bytes <= 0) { close(sock); return; }
        
        buffer[bytes] = '\0';
        string username(buffer);
        username.erase(username.find_last_not_of(" \n\r\t") + 1);
        
        // Get budget
        send(sock, "Budget: $", 9, 0);
        bytes = recv(sock, buffer, sizeof(buffer)-1, 0);
        if (bytes <= 0) { close(sock); return; }
        
        buffer[bytes] = '\0';
        double budget = 0.0;
        try {
            budget = stod(string(buffer));
            if (budget <= 0) {
                send(sock, "Invalid budget. Disconnecting.\n", 32, 0);
                close(sock);
                return;
            }
        } catch (const exception& e) {
            send(sock, "Invalid budget format. Disconnecting.\n", 39, 0);
            close(sock);
            return;
        }
        
        {
            lock_guard<mutex> lock(mtx);
            clients.push_back(Client(sock, username, budget));
        }
        
        string welcomeMsg = "Welcome " + username + "! Budget: $" + to_string(budget) + "\n";
        send(sock, welcomeMsg.c_str(), welcomeMsg.length(), 0);
        
        while (running) {
            bytes = recv(sock, buffer, sizeof(buffer)-1, 0);
            if (bytes <= 0) break;
            buffer[bytes] = '\0';
            
            if (!processCommand(sock, username, string(buffer))) {
                break; // Client should be disconnected
            }
        }
        
        // Clean up client and handle their active bids
        {
            lock_guard<mutex> lock(mtx);
            // Reset any items where this client was highest bidder
            for (auto& item : items) {
                if (item.highestBidderSocket == sock) {
                    item.currentBid = item.basePrice;
                    item.highestBidder = "";
                    item.highestBidderSocket = -1;
                    broadcast("RESET: " + item.name + " bid reset to $" + to_string(item.basePrice));
                }
            }
            
            clients.erase(remove_if(clients.begin(), clients.end(),
                [sock](const Client& c) { return c.socket == sock; }), clients.end());
        }
        close(sock);
    }
    
    bool processCommand(int sock, const string& user, const string& cmd) {
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
            
            // Show client's budget info
            for (const auto& client : clients) {
                if (client.socket == sock) {
                    response += "\nYour Budget: $" + to_string(client.budget) + 
                               " | Spent: $" + to_string(client.totalSpent) + 
                               " | Remaining: $" + to_string(client.budget - client.totalSpent) + "\n";
                    break;
                }
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
            if (iss >> id >> amount) {
                return placeBid(sock, user, id, amount);
            }
        }
        else if (action == "add") {
            string name;
            double price;
            if (iss >> name >> price) {
                lock_guard<mutex> lock(mtx);
                items.push_back(Item(nextId++, name, price));
                send(sock, "Item added!\n", 12, 0);
            }
        }
        else {
            send(sock, "Commands: list, start <id>, bid <id> <amount>, add <name> <price>\n", 66, 0);
        }
        return true;
    }
    
    void startAuction(int sock, int id) {
        lock_guard<mutex> lock(mtx);
        for (auto& item : items) {
            if (item.id == id && item.status == PENDING) {
                item.status = ACTIVE;
                send(sock, ("Started auction for " + item.name + "\n").c_str(), 
                     item.name.length() + 20, 0);
                broadcast("AUCTION: " + item.name + " started! Price: $" + to_string(item.basePrice));
                
                thread([this, id]() {
                    this_thread::sleep_for(chrono::minutes(1)); // Shortened for demo
                    lock_guard<mutex> lock(mtx);
                    for (auto& item : items) {
                        if (item.id == id && item.status == ACTIVE) {
                            item.status = item.highestBidder.empty() ? EXPIRED : SOLD;
                            string endMsg = "ENDED: " + item.name;
                            if (item.status == SOLD) {
                                endMsg += " sold to " + item.highestBidder + " for $" + to_string(item.currentBid);
                                // Update winner's total spent
                                for (auto& client : clients) {
                                    if (client.socket == item.highestBidderSocket) {
                                        client.totalSpent += item.currentBid;
                                        break;
                                    }
                                }
                            } else {
                                endMsg += " expired";
                            }
                            broadcast(endMsg);
                        }
                    }
                }).detach();
                return;
            }
        }
        send(sock, "Cannot start auction\n", 21, 0);
    }
    
    bool placeBid(int sock, const string& user, int id, double amount) {
        lock_guard<mutex> lock(mtx);
        
        // Find the client
        Client* bidder = nullptr;
        for (auto& client : clients) {
            if (client.socket == sock) {
                bidder = &client;
                break;
            }
        }
        
        if (!bidder) {
            send(sock, "Client not found\n", 17, 0);
            return false;
        }
        
        // Check if bid exceeds budget
        if (amount > bidder->budget) {
            string msg = "Bid exceeds your budget of $" + to_string(bidder->budget) + ". You are being disconnected.\n";
            send(sock, msg.c_str(), msg.length(), 0);
            broadcast("DISCONNECTED: " + user + " exceeded budget limit");
            return false; // Signal to disconnect client
        }
        
        // Find the item
        Item* item = nullptr;
        for (auto& itm : items) {
            if (itm.id == id) {
                item = &itm;
                break;
            }
        }
        
        if (!item || item->status != ACTIVE || amount <= item->currentBid) {
            send(sock, "Invalid bid\n", 12, 0);
            return true;
        }
        
        // Handle previous highest bidder disconnection
        int previousHighestBidderSocket = item->highestBidderSocket;
        string previousHighestBidder = item->highestBidder;
        
        
        // Update bid
        item->currentBid = amount;
        item->highestBidder = user;
        item->highestBidderSocket = sock;
        
        send(sock, "Bid accepted!\n", 14, 0);
        broadcast("BID: " + item->name + " $" + to_string(amount) + " by " + user);
        /*
        // Disconnect previous highest bidder if exists
        if (previousHighestBidderSocket != -1 && previousHighestBidderSocket != sock) {
            string disconnectMsg = "You have been outbid on " + item->name + " by " + user + 
                                 " ($" + to_string(amount) + "). You are being disconnected.\n";
            send(previousHighestBidderSocket, disconnectMsg.c_str(), disconnectMsg.length(), 0);
            
            broadcast("DISCONNECTED: " + previousHighestBidder + " was outbid on " + item->name);
            
            // Close the previous bidder's socket in a separate thread to avoid blocking
            thread([previousHighestBidderSocket]() {
                this_thread::sleep_for(chrono::milliseconds(100)); // Small delay to ensure message is sent
                close(previousHighestBidderSocket);
            }).detach();
            
            // Remove the disconnected client from clients vector
            clients.erase(remove_if(clients.begin(), clients.end(),
                [previousHighestBidderSocket](const Client& c) { 
                    return c.socket == previousHighestBidderSocket; 
                }), clients.end());

        }
 */       
        return true;
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