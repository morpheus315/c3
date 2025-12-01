#include "../include/GameClient.h"
#include "../include/chess-game.h"
#include <iostream>

// Constructor: Register all callbacks and start the timeout thread
Client::Client(lanp2p::LanP2PNode& node)
    : _node(node)
{
    // Register callbacks from the network node to our private methods
    _node.setOnPeerDiscovered([this](const lanp2p::PeerInfo& p) { this->onPeerDiscovered(p); });
    _node.setOnMatchRequest([this](const lanp2p::PeerInfo& p, const std::string& mid) { this->onMatchRequest(p, mid); });
    _node.setOnMatchResponse([this](const lanp2p::PeerInfo& p, bool a, const std::string& mid) { this->onMatchResponse(p, a, mid); });
    _node.setOnMatchInterrupted([this](const lanp2p::PeerInfo& p, const std::string& mid) { this->onMatchInterrupted(p, mid); });
    _node.setOnGameMove([this](const lanp2p::PeerInfo& p, int x, int y, int z) { this->onGameMove(p, x, y, z); });

    // Start the background thread for handling request timeouts
    startTimeoutThread();
}

// Destructor: Clean up resources
Client::~Client()
{
    // Stop game loop first if running
    _gameRunning = false;
    
    // Stop timeout thread
    stopTimeoutThread();
    
    // End match if active
    {
        std::lock_guard<std::mutex> lk(_matchMutex);
        if (_match.inMatch)
        {
            // Don't call endMatch() here to avoid deadlock, just send interrupt
            _node.interruptMatch(_match.peer.ip, _match.peer.tcpPort, _match.matchId);
            _match = MatchState{};
        }
    }
    
    // Clean up game state
    cleanupGameState();
    
    // Clear all callbacks to prevent dangling pointer access
    _node.setOnPeerDiscovered(nullptr);
    _node.setOnMatchRequest(nullptr);
    _node.setOnMatchResponse(nullptr);
    _node.setOnMatchInterrupted(nullptr);
    _node.setOnGameMove(nullptr);
}

// --- Public Methods ---

void Client::startDiscovery()
{
    std::cout << "[Client] Starting UDP discovery for 10 seconds..." << std::endl;
    _node.startUdpListen();
    std::this_thread::sleep_for(std::chrono::seconds(10));
    _node.stopUdpListen();
    std::cout << "[Client] UDP discovery stopped." << std::endl;
}

std::vector<lanp2p::PeerInfo> Client::getAvailablePeers()
{
    return _node.getPeersSnapshot();
}

bool Client::requestMatch(const lanp2p::PeerInfo& peer)
{
    {
        std::lock_guard<std::mutex> lk(_matchMutex);
        if (_match.inMatch)
        {
            std::cout << "Already in a match. End it first." << std::endl;
            return false;
        }
    }
    
    std::string mid = lanp2p::LanP2PNode::generateMatchId();
    std::cout << "[Client] Sending REQ: to " << peer.ip << ":" << peer.tcpPort
              << ", matchId=" << mid << ", fromId=" << _node.getNodeId() << std::endl;
    if (_node.sendMatchRequest(peer.ip, peer.tcpPort, mid))
    {
        std::cout << "Match request sent to "
                  << (peer.name.empty() ? peer.id : peer.name)
                  << " with matchId=" << mid
                  << ". Waiting for response..." << std::endl;
        
        // Don't set _iAmMatchInitiator here - wait for response
        return true;
    }
    else
    {
        std::cout << "Failed to send match request." << std::endl;
        return false;
    }
}

void Client::handlePendingRequests()
{
    while (true)
    {
        PendingRequest pr;
        bool hasOne = false;
        {
            std::lock_guard<std::mutex> lk(_pendingMutex);
            if (!_pendingQueue.empty())
            {
                pr = _pendingQueue.front();
                _pendingQueue.pop_front();
                hasOne = true;
            }
        }
        if (!hasOne)
        {
            std::cout << "No more pending requests." << std::endl;
            break;
        }
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
        
        _node.respondToMatch(pr.ip, pr.port, pr.matchId, accept);
        if (accept)
        {
            std::lock_guard<std::mutex> lk(_matchMutex);
            _match.inMatch = true;
            _match.peer = pr.peer;
            _match.matchId = pr.matchId;
            _iAmMatchInitiator = false;  // Mark as match responder
            std::cout << "Accepted. Match started." << std::endl;
        }
        else
        {
            std::cout << "Rejected." << std::endl;
        }
    }
}

bool Client::isInMatch() const
{
    std::lock_guard<std::mutex> lk(_matchMutex);
    return _match.inMatch;
}

void Client::startGame()
{
    {
        std::lock_guard<std::mutex> lk(_matchMutex);
        if (!_match.inMatch) return;
    }

    std::cout << "\n\n===== GAME STARTED =====\n";
    
    lanp2p::PeerInfo opponent;
    {
        std::lock_guard<std::mutex> lk(_matchMutex);
        opponent = _match.peer;
    }
    std::cout << "You are playing against " << (opponent.name.empty() ? opponent.id : opponent.name) << std::endl;
    
    initGameState();
    gameLoop();
    cleanupGameState();

    std::cout << "===== GAME OVER =====\n\n";
    
    std::lock_guard<std::mutex> lk(_matchMutex);
    _match = MatchState{}; // Reset match state
}

void Client::endMatch()
{
    std::string matchId;
    lanp2p::PeerInfo peer;
    bool wasInMatch = false;
    
    {
        std::lock_guard<std::mutex> lk(_matchMutex);
        if (!_match.inMatch) return;
        
        matchId = _match.matchId;
        peer = _match.peer;
        wasInMatch = true;
        _match = MatchState{};
    }
    
    if (wasInMatch)
    {
        std::cout << "[Client] Sending INT to " << peer.ip << ":" << peer.tcpPort
                  << ", matchId=" << matchId << std::endl;
        _node.interruptMatch(peer.ip, peer.tcpPort, matchId);
        _gameRunning = false; // Ensure game loop terminates
        std::cout << "Match ended (interrupt sent)." << std::endl;
    }
}

std::string Client::getMatchId() const
{
    std::lock_guard<std::mutex> lk(_matchMutex);
    return _match.matchId;
}

lanp2p::PeerInfo Client::getMatchPeer() const
{
    std::lock_guard<std::mutex> lk(_matchMutex);
    return _match.peer;
}


// --- Private Methods ---

void Client::onPeerDiscovered(const lanp2p::PeerInfo& p)
{
    std::cout << "[Client DBG] Peer discovered: name=" << (p.name.empty() ? "<noname>" : p.name)
              << " id=" << p.id << " at " << p.ip << ":" << p.tcpPort << std::endl;
}

void Client::onMatchRequest(const lanp2p::PeerInfo& p, const std::string& matchId)
{
    PendingRequest pr;
    pr.has = true;
    pr.ip = p.ip;
    pr.port = p.tcpPort;
    pr.matchId = matchId;
    pr.peer = p;
    pr.ts = std::chrono::steady_clock::now();
    {
        std::lock_guard<std::mutex> lk(_pendingMutex);
        _pendingQueue.push_back(std::move(pr));
    }
    std::cout << "\n[Incoming] match request from "
              << (p.name.empty() ? p.id : p.name)
              << ". Hint: choose option in menu to handle." << std::endl;
}

void Client::onMatchResponse(const lanp2p::PeerInfo& p, bool accepted, const std::string& matchId)
{
    std::cout << "[Response] from "
              << (p.name.empty() ? p.id : p.name)
              << " match=" << matchId
              << " accepted=" << (accepted ? "true" : "false") << std::endl;
    if (accepted)
    {
        std::lock_guard<std::mutex> lk(_matchMutex);
        _match.inMatch = true;
        _match.peer = p;
        _match.matchId = matchId;
        _iAmMatchInitiator = true;  // Mark as match initiator only when accepted
    }
}

void Client::onMatchInterrupted(const lanp2p::PeerInfo& p, const std::string& matchId)
{
    std::cout << "[Interrupt] from "
              << (p.name.empty() ? p.id : p.name)
              << " match=" << matchId << std::endl;
    
    std::lock_guard<std::mutex> lk(_matchMutex);
    if (_match.inMatch && _match.matchId == matchId && _match.peer.id == p.id)
    {
        _match = MatchState{};
        _gameRunning = false; // Stop the game loop
    }
}

void Client::onGameMove(const lanp2p::PeerInfo& p, int x, int y, int z)
{
    bool shouldProcess = false;
    {
        std::lock_guard<std::mutex> lk(_matchMutex);
        shouldProcess = (_match.inMatch && p.id == _match.peer.id);
    }
    
    if (shouldProcess)
    {
        std::lock_guard<std::mutex> lk(_moveMutex);
        _opponentMove[0] = x;
        _opponentMove[1] = y;
        _opponentMove[2] = z;
        _opponentMoved = true;
    }
}

void Client::timeoutThreadLoop()
{
    while (_timeoutThreadRunning.load())
    {
        PendingRequest pr;
        bool expired = false;
        {
            std::lock_guard<std::mutex> lk(_pendingMutex);
            if (!_pendingQueue.empty())
            {
                auto& front = _pendingQueue.front();
                if (std::chrono::steady_clock::now() - front.ts > _requestTimeout)
                {
                    pr = front;
                    _pendingQueue.pop_front();
                    expired = true;
                }
            }
        }
        if (expired && pr.has)
        {
            _node.respondToMatch(pr.ip, pr.port, pr.matchId, false);
            std::cout << "[Auto] Rejected (timeout) match " << pr.matchId << " for peer "
                      << (pr.peer.name.empty() ? pr.peer.id : pr.peer.name) << std::endl;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(200));
    }
}

void Client::startTimeoutThread()
{
    _timeoutThreadRunning = true;
    _timeoutThread = std::thread(&Client::timeoutThreadLoop, this);
}

void Client::stopTimeoutThread()
{
    _timeoutThreadRunning = false;
    if (_timeoutThread.joinable())
    {
        _timeoutThread.join();
    }
}

void Client::initGameState()
{
    cleanupGameState(); // Ensure no old board exists
    if (!OnlineInitChessBoard(&_chessBoard, _boardSize))
    {
        std::cout << "FATAL: Could not initialize chess board." << std::endl;
        endMatch();
        return;
    }
    
    std::string matchId;
    {
        std::lock_guard<std::mutex> lk(_matchMutex);
        matchId = _match.matchId;
    }
    
    // Determine if matchId first character is odd
    bool matchIdIsOdd = false;
    if (!matchId.empty())
    {
        char firstChar = matchId[0];
        if (firstChar >= '0' && firstChar <= '9')
            matchIdIsOdd = ((firstChar - '0') % 2 == 1);
        else if (firstChar >= 'a' && firstChar <= 'f')
            matchIdIsOdd = ((firstChar - 'a') % 2 == 1);
        else if (firstChar >= 'A' && firstChar <= 'F')
            matchIdIsOdd = ((firstChar - 'A') % 2 == 1);
    }
    
    // Determine who goes first based on initiator status and matchId parity
    // Initiator + odd = first player, Initiator + even = second player
    // Responder + odd = second player, Responder + even = first player
    bool iAmFirstPlayer = (_iAmMatchInitiator && matchIdIsOdd) || (!_iAmMatchInitiator && !matchIdIsOdd);
    
    _myPlayer = iAmFirstPlayer ? '1' : '2';
    _myTurn = (_myPlayer == '1');
    _gameRunning = true;
    _opponentMoved = false;

    std::cout << "Board initialized. You are Player " << _myPlayer 
              << " (" << (_iAmMatchInitiator ? "Initiator" : "Responder") 
              << ", matchId: " << matchId.substr(0, 4) << "...)." << std::endl;
    if (_myTurn) {
        std::cout << "It's your turn." << std::endl;
    } else {
        std::cout << "Waiting for opponent's move..." << std::endl;
    }
}

void Client::cleanupGameState()
{
    if (_chessBoard)
    {
        free(_chessBoard);
        _chessBoard = nullptr;
    }
}

void Client::gameLoop()
{
    while (_gameRunning.load())
    {
        if (_myTurn)
        {
            int coords[3];
            NativeGetChessPosition(coords);
            if (UpdateBoardState(_boardSize, _chessBoard, coords, _myPlayer))
            {
                // Get opponent info safely
                lanp2p::PeerInfo opponent;
                {
                    std::lock_guard<std::mutex> lk(_matchMutex);
                    opponent = _match.peer;
                }
                
                _node.sendGameMove(opponent.ip, opponent.tcpPort, coords[0], coords[1], coords[2]);
                if (CheckWin(_boardSize, _chessBoard, coords, _myPlayer))
                {
                    std::cout << "Congratulations! You win!" << std::endl;
                    _gameRunning = false;
                    break;
                }
                _myTurn = false;
                std::cout << "Waiting for opponent's move..." << std::endl;
            }
            else
            {
                std::cout << "Invalid move, please try again." << std::endl;
            }
        }
        else // Opponent's turn
        {
            bool moved = false;
            {
                std::lock_guard<std::mutex> lk(_moveMutex);
                if (_opponentMoved)
                {
                    char opponentPlayer = (_myPlayer == '1') ? '2' : '1';
                    UpdateBoardState(_boardSize, _chessBoard, _opponentMove, opponentPlayer);
                    if (CheckWin(_boardSize, _chessBoard, _opponentMove, opponentPlayer))
                    {
                        std::cout << "You lose. Better luck next time!" << std::endl;
                        _gameRunning = false;
                    }
                    _opponentMoved = false;
                    moved = true;
                }
            }
            if (moved)
            {
                _myTurn = true;
                std::cout << "It's your turn." << std::endl;
            }
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }
}
