#pragma once

#include <string>
#include <vector>
#include <deque>
#include <mutex>
#include <atomic>
#include <thread>
#include <chrono>
#include <functional>
#include "LanP2PNode.h"

class Client
{
public:
	Client(lanp2p::LanP2PNode& node);
	~Client();

	// Disable copy and move
	Client(const Client&) = delete;
	Client& operator=(const Client&) = delete;

	void startDiscovery();
	std::vector<lanp2p::PeerInfo> getAvailablePeers();
	bool requestMatch(const lanp2p::PeerInfo& peer);
	void handlePendingRequests();

	bool isInMatch() const;
	void startGame();
	void endMatch();

	std::string getMatchId() const;
	lanp2p::PeerInfo getMatchPeer() const;

private:
	lanp2p::LanP2PNode& _node;

	struct MatchState
	{
		bool inMatch{ false };
		std::string matchId;
		lanp2p::PeerInfo peer;
	};
	MatchState _match;
	mutable std::mutex _matchMutex; // Protects _match access

	struct PendingRequest
	{
		bool has{ false };
		std::string ip;
		uint16_t port{ 0 };
		std::string matchId;
		lanp2p::PeerInfo peer;
		std::chrono::steady_clock::time_point ts;
	};

	std::mutex _pendingMutex;
	std::deque<PendingRequest> _pendingQueue;
	std::atomic<bool> _timeoutThreadRunning{ false };
	std::thread _timeoutThread;
	const std::chrono::seconds _requestTimeout{ 30 };

	char* _chessBoard{ nullptr };
	const int _boardSize{ 20 };
	bool _myTurn{ false };
	char _myPlayer{ '1' };
	std::atomic<bool> _gameRunning{ false };
	bool _iAmMatchInitiator{ false };  // Track if I initiated the match

	std::mutex _moveMutex;
	bool _opponentMoved{ false };
	int _opponentMove[3]{ 0,0,0 };

	void onPeerDiscovered(const lanp2p::PeerInfo& p);
	void onMatchRequest(const lanp2p::PeerInfo& p, const std::string& matchId);
	void onMatchResponse(const lanp2p::PeerInfo& p, bool accepted, const std::string& matchId);
	void onMatchInterrupted(const lanp2p::PeerInfo& p, const std::string& matchId);
	void onGameMove(const lanp2p::PeerInfo& p, int x, int y, int z);

	void timeoutThreadLoop();
	void startTimeoutThread();
	void stopTimeoutThread();

	void gameLoop();
	void initGameState();
	void cleanupGameState();
};