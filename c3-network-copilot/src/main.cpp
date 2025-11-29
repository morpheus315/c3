#include <iostream>
#include <atomic>
#include <chrono>
#include <thread>
#include <mutex>
#include <deque>
#include <string>
#include <vector>
#include <random>
#include <sstream>
#include "../include/LanP2PNode.h"

struct PendingRequest
{
	bool has{false};
	std::string ip;
	uint16_t port{0};
	std::string matchId;
	lanp2p::PeerInfo peer;
	std::chrono::steady_clock::time_point ts;
};

struct MatchState
{
	bool inMatch{false};
	std::string matchId;
	lanp2p::PeerInfo peer; // opponent
};

static std::string genMatchId()
{
	std::random_device rd;
	std::mt19937_64 rng(rd());
	std::uniform_int_distribution<uint64_t> dist;
	uint64_t v = dist(rng);
	char buf[17];
	std::snprintf(buf, sizeof(buf), "%016llx", (unsigned long long)v);
	return std::string(buf);
}

static void printMenu()
{
	std::cout << "\n===== LAN P2P Client Menu =====\n";
	std::cout << "1. Start discovery (send broadcasts, listen for 10s)\n";
	std::cout << "2. List discovered peers\n";
	std::cout << "3. Request match to a discovered peer (auto matchId)\n";
	std::cout << "4. Show current match info\n";
	std::cout << "5. Exit\n";
	std::cout << "6. Handle pending incoming requests\n";
	std::cout << "7. End current match (send interrupt)\n";
	std::cout << "Select: ";
}

int main()
{
	using namespace lanp2p;

	// Pass tcpPort=0 to let the node choose a free port automatically
	LanP2PNode node(37000, 0); // discovery UDP port, TCP control port (ephemeral)
	node.setPeerStaleMs(15000); // Configure peer timeout to 15 seconds

	// Ask user for a display name to differentiate clients
	std::cout << "Enter your display name (optional, default empty): ";
	std::string myName;
	std::getline(std::cin, myName);
	if (!myName.empty())
		node.setNodeName(myName);

	std::mutex pendingMutex;
	std::deque<PendingRequest> pendingQueue;

	std::atomic<bool> run{true};
	const auto timeout = std::chrono::seconds(30);

	MatchState match;
	bool broadcastStarted = false;

	// Enable discovery logging
	node.setOnPeerDiscovered([](const PeerInfo& p)
	{
		std::cout << "[DBG] Peer discovered: name=" << (p.name.empty() ? "<noname>" : p.name)
		          << " id=" << p.id << " at " << p.ip << ":" << p.tcpPort << std::endl;
	});

	node.setOnMatchRequest([&](const PeerInfo& p, const std::string& matchId)
	{
		PendingRequest pr;
		pr.has = true;
		pr.ip = p.ip;
		pr.port = p.tcpPort;
		pr.matchId = matchId;
		pr.peer = p;
		pr.ts = std::chrono::steady_clock::now();
		{
			std::lock_guard<std::mutex> lk(pendingMutex);
			pendingQueue.push_back(std::move(pr));
		}
		std::cout << "\n[Incoming] match request from "
		          << (p.name.empty() ? p.id : p.name)
		          << " (id=" << p.id << ") "
		          << p.ip << ":" << p.tcpPort
		          << " match=" << matchId << std::endl;
		std::cout << "Hint: choose 6 in menu to handle pending requests." << std::endl;
	});

	node.setOnMatchResponse([&](const PeerInfo& p, bool acc, const std::string& matchId)
	{
		std::cout << "[Response] from "
		          << (p.name.empty() ? p.id : p.name)
		          << " (id=" << p.id << ") match=" << matchId
		          << " accepted=" << (acc ? "true" : "false") << std::endl;
		if (acc)
		{
			match.inMatch = true;
			match.peer = p;
			match.matchId = matchId;
		}
	});

	node.setOnMatchInterrupted([&](const PeerInfo& p, const std::string& matchId)
	{
		std::cout << "[Interrupt] from "
		          << (p.name.empty() ? p.id : p.name)
		          << " (id=" << p.id << ") match=" << matchId << std::endl;
		if (match.inMatch && match.matchId == matchId && match.peer.id == p.id)
		{
			match = MatchState{};
		}
	});

	// Start broadcasting and TCP listening
	node.startBroadcastOnly();
	broadcastStarted = true;

	// Give listeners a moment to start
	std::this_thread::sleep_for(std::chrono::milliseconds(150));
	std::cout << "[DBG] Initialized. id=" << node.getNodeId()
	          << ", name=" << (myName.empty() ? std::string("<noname>") : myName)
	          << ", tcpPort=" << node.getTcpPort()
	          << ", discoveryPort=" << node.getDiscoveryPort()
	          << ", broadcast=ON, udpListen=OFF (press 1 to scan)" << std::endl;
	std::cout << "[DBG] Peer timeout set to 15s, auto cleaning enabled" << std::endl;

	// Background auto-timeout thread for pending requests
	std::thread timeoutThread([&]
	{
		while (run.load())
		{
			PendingRequest pr;
			bool expired = false;
			{
				std::lock_guard<std::mutex> lk(pendingMutex);
				if (!pendingQueue.empty())
				{
					auto& front = pendingQueue.front();
					if (std::chrono::steady_clock::now() - front.ts > timeout)
					{
						pr = front;
						pendingQueue.pop_front();
						expired = true;
					}
				}
			}
			if (expired && pr.has)
			{
				node.respondToMatch(pr.ip, pr.port, pr.matchId, false);
				std::cout << "[Auto] Rejected (timeout) match " << pr.matchId << " for peer "
				          << (pr.peer.name.empty() ? pr.peer.id : pr.peer.name) << std::endl;
			}
			std::this_thread::sleep_for(std::chrono::milliseconds(200));
		}
	});

	bool running = true;
	while (running)
	{
		printMenu();
		std::string line;
		if (!std::getline(std::cin, line))
			break;
		if (line.empty())
		{
			std::cout << "Invalid choice. Please try again." << std::endl;
			continue;
		}
		int choice = 0;
		try
		{
			choice = std::stoi(line);
		}
		catch (...)
		{
			std::cout << "Invalid choice. Please try again." << std::endl;
			continue;
		}

		switch (choice)
		{
			case 1:
			{
				std::cout << "[DBG] Start UDP discovery listening for 10s..." << std::endl;
				node.startUdpListen();
				std::this_thread::sleep_for(std::chrono::seconds(10));
				node.stopUdpListen();
				std::cout << "[DBG] UDP discovery listening stopped." << std::endl;
				break;
			}
			case 2:
			{
				auto peers = node.getPeersSnapshot();
				if (peers.empty())
					std::cout << "No peers discovered in last scan." << std::endl;
				else
				{
					std::cout << "Discovered peers (auto cleaned after 15s inactive):" << std::endl;
					for (size_t i = 0; i < peers.size(); ++i)
					{
						std::cout << i+1 << ". Name: " << (peers[i].name.empty() ? "<noname>" : peers[i].name)
						          << ", ID: " << peers[i].id
						          << ", IP: " << peers[i].ip
						          << ", Port: " << peers[i].tcpPort << std::endl;
					}
				}
				break;
			}
			case 3:
			{
				if (!broadcastStarted)
				{
					std::cout << "Broadcast not started." << std::endl;
					break;
				}
				if (match.inMatch)
				{
					std::cout << "Already in a match. End it first (7)." << std::endl;
					break;
				}
				auto peers = node.getPeersSnapshot();
				if (peers.empty())
				{
					std::cout << "No peers to request a match." << std::endl;
					break;
				}
				std::cout << "Select a peer to request a match:" << std::endl;
				for (size_t i = 0; i < peers.size(); ++i)
					std::cout << i+1 << ". " << (peers[i].name.empty() ? peers[i].id : peers[i].name)
					          << " (" << peers[i].ip << ":" << peers[i].tcpPort << ")" << std::endl;
				std::string sel;
				if (!std::getline(std::cin, sel) || sel.empty())
				{
					std::cout << "Invalid choice." << std::endl;
					break;
				}
				int idx = 0;
				try
				{
					idx = std::stoi(sel);
				}
				catch (...)
				{
					std::cout << "Invalid choice." << std::endl;
					break;
				}
				if (idx < 1 || idx > (int)peers.size())
				{
					std::cout << "Invalid choice." << std::endl;
					break;
				}
				std::string mid = genMatchId();
				auto& peer = peers[(size_t)idx-1];
				std::cout << "[DBG] Sending REQ: to " << peer.ip << ":" << peer.tcpPort
				          << ", matchId=" << mid << ", fromId=" << node.getNodeId() << std::endl;
				if (node.sendMatchRequest(peer.ip, peer.tcpPort, mid))
				{
					std::cout << "Match request sent to "
					          << (peer.name.empty() ? peer.id : peer.name)
					          << " with matchId=" << mid
					          << ". Waiting for response..." << std::endl;
					match.matchId = mid;
					match.peer = peer;
				}
				else
					std::cout << "Failed to send match request." << std::endl;
				break;
			}
			case 4:
			{
				if (!match.inMatch)
					std::cout << "Not in a match." << std::endl;
				else
				{
					std::cout << "In match with ID: " << match.matchId
					          << " vs: " << (match.peer.name.empty() ? match.peer.id : match.peer.name)
					          << " (" << match.peer.ip << ":" << match.peer.tcpPort << ")" << std::endl;
				}
				break;
			}
			case 5:
			{
				running = false;
				break;
			}
			case 6:
			{
				while (true)
				{
					PendingRequest pr;
					bool hasOne = false;
					{
						std::lock_guard<std::mutex> lk(pendingMutex);
						if (!pendingQueue.empty())
						{
							pr = pendingQueue.front();
							pendingQueue.pop_front();
							hasOne = true;
						}
					}
					if (!hasOne)
						break;
					std::cout << "\n====== MATCH REQUEST ======\n";
					std::cout << "from: "
					          << (pr.peer.name.empty() ? pr.peer.id : pr.peer.name)
					          << " (id=" << pr.peer.id << ") "
					          << pr.ip << ":" << pr.port
					          << " matchId=" << pr.matchId
					          << "\nAccept? (y/n): ";
					std::string resp;
					if (!std::getline(std::cin, resp))
						resp.clear();
					bool accept = (!resp.empty() && (resp[0] == 'y' || resp[0] == 'Y'));
					std::cout << "[DBG] Sending RESP: to " << pr.ip << ":" << pr.port
					          << ", matchId=" << pr.matchId << ", accept=" << (accept ? "1" : "0") << std::endl;
					node.respondToMatch(pr.ip, pr.port, pr.matchId, accept);
					if (accept)
					{
						match.inMatch = true;
						match.peer = pr.peer;
						match.matchId = pr.matchId;
						std::cout << "Accepted." << std::endl;
					}
					else
						std::cout << "Rejected." << std::endl;
				}
				break;
			}
			case 7:
			{
				if (!match.inMatch)
				{
					std::cout << "Not currently in a match." << std::endl;
				}
				else
				{
					std::cout << "[DBG] Sending INT to " << match.peer.ip << ":" << match.peer.tcpPort
					          << ", matchId=" << match.matchId << std::endl;
					node.interruptMatch(match.peer.ip, match.peer.tcpPort, match.matchId);
					match = MatchState{};
					std::cout << "Match ended (interrupt sent)." << std::endl;
				}
				break;
			}
			default:
				std::cout << "Invalid choice. Please try again." << std::endl;
		}
	}

	run.store(false);
	if (timeoutThread.joinable())
		timeoutThread.join();

	// Clean up on exit
	if (match.inMatch)
	{
		std::cout << "[DBG] Exiting: sending interrupt to " << match.peer.ip << ":" << match.peer.tcpPort
		          << ", matchId=" << match.matchId << std::endl;
		node.interruptMatch(match.peer.ip, match.peer.tcpPort, match.matchId);
	}

	node.stop();
	return 0;
}
