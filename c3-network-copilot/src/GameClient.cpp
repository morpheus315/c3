#include "../include/GameClient.h"
#include "../include/chess-game.h"
#include <iostream>

// 构造函数：注册网络回调并启动待处理请求的超时线程
Client::Client(lanp2p::LanP2PNode &node)
	: _node(node)
{
	// 向底层节点注册回调，绑定到本类的私有成员函数
	_node.setOnPeerDiscovered([this](const lanp2p::PeerInfo &p)
	{
		this->onPeerDiscovered(p);
	});
	_node.setOnMatchRequest([this](const lanp2p::PeerInfo &p, const std::string &mid)
	{
		this->onMatchRequest(p, mid);
	});
	_node.setOnMatchResponse([this](const lanp2p::PeerInfo &p, bool a, const std::string &mid)
	{
		this->onMatchResponse(p, a, mid);
	});
	_node.setOnMatchInterrupted([this](const lanp2p::PeerInfo &p, const std::string &mid)
	{
		this->onMatchInterrupted(p, mid);
	});
	_node.setOnGameMove([this](const lanp2p::PeerInfo &p, int x, int y, int z)
	{
		this->onGameMove(p, x, y, z);
	});

	// 启动后台线程以处理匹配请求的自动超时拒绝
	startTimeoutThread();
}

// 析构函数：优先停止底层节点，再清理线程与状态，避免回调竞态
Client::~Client()
{
	// 先停止节点所有网络线程，防止后续仍有回调进入
	_node.stop();

	// 及早清空回调，避免回调访问失效对象
	_node.setOnPeerDiscovered(nullptr);
	_node.setOnMatchRequest(nullptr);
	_node.setOnMatchResponse(nullptr);
	_node.setOnMatchInterrupted(nullptr);
	_node.setOnGameMove(nullptr);

	// 停止游戏循环
	_gameRunning = false;

	// 结束超时线程
	stopTimeoutThread();

	// 清空比赛状态（节点已停止，发送中断无意义，直接重置）
	{
		std::lock_guard<std::mutex> lk(_matchMutex);
		_match = MatchState{};
	}

	// 释放棋盘内存
	cleanupGameState();
}

// --- 公有方法 ---

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

bool Client::requestMatch(const lanp2p::PeerInfo &peer)
{
	// 防止在已处于对局时再次发起匹配
	{
		std::lock_guard<std::mutex> lk(_matchMutex);
		if (_match.inMatch)
		{
			std::cout << "Already in a match. End it first." << std::endl;
			return false;
		}
	}

	// 生成随机匹配ID并发送请求
	std::string mid = lanp2p::LanP2PNode::generateMatchId();
	std::cout << "[Client] Sending REQ: to " << peer.ip << ":" << peer.tcpPort
	          << ", matchId=" << mid << ", fromId=" << _node.getNodeId() << std::endl;
	if (_node.sendMatchRequest(peer.ip, peer.tcpPort, mid))
	{
		std::cout << "Match request sent to "
		          << (peer.name.empty() ? peer.id : peer.name)
		          << " with matchId=" << mid
		          << ". Waiting for response..." << std::endl;

		// 发起者身份在收到对方接受后再标记
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
	// 逐个处理本地排队的来访匹配请求
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
			// 接受后建立对局本地状态，并标记自身为“响应者”
			std::lock_guard<std::mutex> lk(_matchMutex);
			_match.inMatch = true;
			_match.peer = pr.peer;
			_match.matchId = pr.matchId;
			_iAmMatchInitiator = false;
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
		if (!_match.inMatch)
			return;
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
	_match = MatchState{}; // 重置对局状态
}

void Client::endMatch()
{
	// 主动结束比赛：读取并清空匹配状态，然后发送中断
	std::string matchId;
	lanp2p::PeerInfo peer;
	bool wasInMatch = false;

	{
		std::lock_guard<std::mutex> lk(_matchMutex);
		if (!_match.inMatch)
			return;

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
		_gameRunning = false; // 确保游戏循环退出
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


// --- 私有方法（回调与线程） ---

void Client::onPeerDiscovered(const lanp2p::PeerInfo &p)
{
	std::cout << "[Client DBG] Peer discovered: name=" << (p.name.empty() ? "<noname>" : p.name)
	          << " id=" << p.id << " at " << p.ip << ":" << p.tcpPort << std::endl;
}

void Client::onMatchRequest(const lanp2p::PeerInfo &p, const std::string &matchId)
{
	// 收到对方发起的匹配请求，入队等待用户处理
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

void Client::onMatchResponse(const lanp2p::PeerInfo &p, bool accepted, const std::string &matchId)
{
	std::cout << "[Response] from "
	          << (p.name.empty() ? p.id : p.name)
	          << " match=" << matchId
	          << " accepted=" << (accepted ? "true" : "false") << std::endl;
	if (accepted)
	{
		// 我方作为请求发起者时，对方接受后建立本地对局状态，并标记“发起者”身份
		std::lock_guard<std::mutex> lk(_matchMutex);
		_match.inMatch = true;
		_match.peer = p;
		_match.matchId = matchId;
		_iAmMatchInitiator = true;
	}
}

void Client::onMatchInterrupted(const lanp2p::PeerInfo &p, const std::string &matchId)
{
	std::cout << "[Interrupt] from "
	          << (p.name.empty() ? p.id : p.name)
	          << " match=" << matchId << std::endl;

	std::lock_guard<std::mutex> lk(_matchMutex);
	if (_match.inMatch && _match.matchId == matchId && _match.peer.id == p.id)
	{
		_match = MatchState{};
		_gameRunning = false; // 停止游戏循环
	}
}

void Client::onGameMove(const lanp2p::PeerInfo &p, int x, int y, int z)
{
	// 仅在对局中且消息来自当前对手时才缓存其落子
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
	// 周期扫描未处理的匹配请求，超时自动拒绝
	while (_timeoutThreadRunning.load())
	{
		PendingRequest pr;
		bool expired = false;
		{
			std::lock_guard<std::mutex> lk(_pendingMutex);
			if (!_pendingQueue.empty())
			{
				auto &front = _pendingQueue.front();
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
	// 初始化棋盘（确保旧棋盘已释放）
	cleanupGameState();
	if (!OnlineInitChessBoard(&_chessBoard, _boardSize))
	{
		std::cout << "FATAL: Could not initialize chess board." << std::endl;
		endMatch();
		return;
	}

	// 读取匹配ID并根据规则决定先后手
	std::string matchId;
	{
		std::lock_guard<std::mutex> lk(_matchMutex);
		matchId = _match.matchId;
	}

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

	// 先后手规则：发起者+奇数先手；响应者+偶数先手
	bool iAmFirstPlayer = (_iAmMatchInitiator && matchIdIsOdd) || (!_iAmMatchInitiator && !matchIdIsOdd);

	_myPlayer = iAmFirstPlayer ? '1' : '2';
	_myTurn = (_myPlayer == '1');
	_gameRunning = true;
	_opponentMoved = false;

	std::cout << "Board initialized. You are Player " << _myPlayer
	          << " (" << (_iAmMatchInitiator ? "Initiator" : "Responder")
	          << ", matchId: " << matchId.substr(0, 4) << "...)." << std::endl;
	if (_myTurn)
	{
		std::cout << "It's your turn." << std::endl;
	}
	else
	{
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
	// 主循环：根据回合决定本地落子或处理对手落子
	while (_gameRunning.load())
	{
		if (_myTurn)
		{
			int coords[3];
			NativeGetChessPosition(coords);
			if (UpdateBoardState(_boardSize, _chessBoard, coords, _myPlayer))
			{
				// 读取对手信息，发送我方落子给对手
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
		else // 轮到对手
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
