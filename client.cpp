#include <iostream>
#include <string>
#include <thread>
#include <atomic>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <unistd.h>

using namespace std;

class AuctionClient {
private:
    int sock;
    atomic<bool> running{true};
    atomic<bool> connected{false};
    
public:
    bool connect(const string& ip, int port) {
        sock = socket(AF_INET, SOCK_STREAM, 0);
        if (sock < 0) return false;
        
        sockaddr_in addr{AF_INET, htons(port)};
        if (inet_pton(AF_INET, ip.c_str(), &addr.sin_addr) <= 0) return false;
        if (::connect(sock, (sockaddr*)&addr, sizeof(addr)) < 0) return false;
        
        cout << "Connected to " << ip << ":" << port << endl;
        connected = true;
        return true;
    }
    
    void start() {
        if (!connected) {
            cout << "Not connected to server\n";
            return;
        }
        
        thread(&AuctionClient::receive, this).detach();
        
        string input;
        while (running && connected && getline(cin, input)) {
            if (input == "quit") break;
            input += "\n";
            int result = send(sock, input.c_str(), input.length(), 0);
            if (result < 0) {
                cout << "Connection lost\n";
                break;
            }
        }
        running = false;
    }
    
    void receive() {
        char buffer[1024];
        while (running && connected) {
            int bytes = recv(sock, buffer, sizeof(buffer)-1, 0);
            if (bytes <= 0) {
                cout << "\nConnection closed by server\n";
                running = false;
                connected = false;
                break;
            }
            buffer[bytes] = '\0';
            cout << buffer;
            cout.flush();
        }
    }
    
    ~AuctionClient() { 
        if (connected) {
            close(sock); 
        }
    }
};

int main(int argc, char* argv[]) {
    string ip = argc > 1 ? argv[1] : "127.0.0.1";
    int port = argc > 2 ? stoi(argv[2]) : 8080;
    
    cout << "=== AUCTION CLIENT ===\n";
    cout << "You will be asked for your username and budget upon connection.\n";
    cout << "Commands: list, start <id>, bid <id> <amount>, add <name> <price>, quit\n";
    cout << "Note: Bidding above your budget or being outbid will disconnect you.\n\n";
    
    AuctionClient client;
    if (!client.connect(ip, port)) {
        cout << "Connection failed\n";
        return -1;
    }
    
    client.start();
    return 0;
}