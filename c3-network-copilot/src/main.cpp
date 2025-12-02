#include "../include/LanP2PNode.h"
#include "../include/GameClient.h"
#include <iostream>
#include <string>

static void printMenu()
{
    std::cout << "\n===== 3D Gomoku Online =====\n";
    std::cout << "1. Start discovery (listen 10s)\n";
    std::cout << "2. List peers\n";
    std::cout << "3. Request match\n";
    std::cout << "4. Handle pending requests\n";
    std::cout << "5. Show match info\n";
    std::cout << "6. End match\n";
    std::cout << "7. Exit\n";
    std::cout << "Select: ";
}

int main()
{
    using namespace lanp2p;

    LanP2PNode node(37000, 0);
    node.setPeerStaleMs(15000);
    node.startBroadcastOnly();

    Client client(node);

    std::cout << "Enter your display name (optional): ";
    std::string name;
    std::getline(std::cin, name);
    if (!name.empty()) node.setNodeName(name);

    std::cout << "Initialized. Your ID: " << node.getNodeId()
              << ", TCP: " << node.getTcpPort()
              << ", DISC: " << node.getDiscoveryPort() << std::endl;

    bool running = true;
    while (running)
    {
        // Auto-start game when matched
        if (client.isInMatch())
        {
            client.startGame();
            continue;
        }

        printMenu();
        std::string line;
        if (!std::getline(std::cin, line)) break;
        int choice = 0;
        try { choice = std::stoi(line); } catch (...) { std::cout << "Invalid input." << std::endl; continue; }

        switch (choice)
        {
            case 1:
                client.startDiscovery();
                break;
            case 2:
            {
                auto peers = client.getAvailablePeers();
                if (peers.empty()) { std::cout << "No peers." << std::endl; break; }
                std::cout << "Peers:" << std::endl;
                for (size_t i = 0; i < peers.size(); ++i)
                {
                    const auto& p = peers[i];
                    std::cout << i+1 << ". " << (p.name.empty()?p.id:p.name)
                              << " (" << p.ip << ":" << p.tcpPort << ")" << std::endl;
                }
                break;
            }
            case 3:
            {
                auto peers = client.getAvailablePeers();
                if (peers.empty()) { std::cout << "No peers to match." << std::endl; break; }
                std::cout << "Select peer index: ";
                std::string sel; std::getline(std::cin, sel);
                int idx = 0; try { idx = std::stoi(sel); } catch (...) { std::cout << "Invalid." << std::endl; break; }
                if (idx < 1 || idx > (int)peers.size()) { std::cout << "Out of range." << std::endl; break; }
                client.requestMatch(peers[(size_t)idx-1]);
                break;
            }
            case 4:
                client.handlePendingRequests();
                break;
            case 5:
            {
                if (!client.isInMatch()) { std::cout << "Not in a match." << std::endl; break; }
                auto peer = client.getMatchPeer();
                std::cout << "Match ID: " << client.getMatchId() << ", Opponent: "
                          << (peer.name.empty()?peer.id:peer.name) << " (" << peer.ip << ":" << peer.tcpPort << ")" << std::endl;
                break;
            }
            case 6:
                client.endMatch();
                break;
            case 7:
                running = false;
                break;
            default:
                std::cout << "Invalid choice." << std::endl;
        }
    }

    node.stop();
    return 0;
}
