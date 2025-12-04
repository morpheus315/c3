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

// 析构函数
Client::~Client()
{
	// 若在对局中，先发送中断消息通知对方
	{
		std::lock_guard<std::mutex> lk(_matchMutex);
		if (_match.inMatch)
		{
			_node.interruptMatch(_match.peer.ip, _match.peer.tcpPort, _match.matchId);
			_match = MatchState{};
		}
	}

	// 停止游戏循环
	_gameRunning = false;

	// 先停止节点及其所有线程，阻止后续回调触发
	_node.stop();

	// 清空回调（避免回调到已失效对象）
	_node.setOnPeerDiscovered(nullptr);
	_node.setOnMatchRequest(nullptr);
	_node.setOnMatchResponse(nullptr);
	_node.setOnMatchInterrupted(nullptr);
	_node.setOnGameMove(nullptr);

	// 清理超时线程
	stopTimeoutThread();

	// 释放棋盘内存
	cleanupGameState();
}


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
			std::cout << "已经在对局中" << std::endl;
			return false;
		}
	}

	// 生成随机匹配ID并发送请求
	std::string mid = lanp2p::LanP2PNode::generateMatchId();
	std::cout << "[Client] 发送请求： " << peer.ip << ":" << peer.tcpPort
	          << ", matchId=" << mid << ", fromId=" << _node.getNodeId() << std::endl;
	if (_node.sendMatchRequest(peer.ip, peer.tcpPort, mid))
	{
		std::cout << "发送请求："
		          << (peer.name.empty() ? peer.id : peer.name)
		          << "，matchId=" << mid
		          << std::endl;
		return true;
	}
	else
	{
		std::cout << "发送失败" << std::endl;
		return false;
	}
}

void Client::handlePendingRequests()
{
	//依次处理所有排队等待的匹配请求
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
			std::cout << "没有待处理的请求" << std::endl;
			break;
		}
		std::cout << "\n====== 匹配请求 ======\n";
		std::cout << "from: "
		          << (pr.peer.name.empty() ? pr.peer.id : pr.peer.name)
		          << " (id=" << pr.peer.id << ") "
		          << pr.ip << ":" << pr.port
		          << " matchId=" << pr.matchId
		          << "\n同意? (y/n): ";
		std::string resp;
		if (!std::getline(std::cin, resp))
			resp.clear();
		bool accept = (!resp.empty() && (resp[0] == 'y' || resp[0] == 'Y'));

		_node.respondToMatch(pr.ip, pr.port, pr.matchId, accept);
		if (accept)
		{
			//接受后进入对局并更新状态，我方角色为"应答者"。
			std::lock_guard<std::mutex> lk(_matchMutex);
			_match.inMatch = true;
			_match.peer = pr.peer;
			_match.matchId = pr.matchId;
			_iAmMatchInitiator = false;
			//注册心跳检测
			_node.markMatchActive(pr.ip, pr.port, pr.peer.id, pr.matchId);
			std::cout << "对方同意，游戏开始" << std::endl;
		}
		else
		{
			std::cout << "对方拒绝" << std::endl;
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

	std::cout << "\n\n=====游戏开始=====\n";

	lanp2p::PeerInfo opponent;
	{
		std::lock_guard<std::mutex> lk(_matchMutex);
		opponent = _match.peer;
	}
	std::cout << "你的对手：" << (opponent.name.empty() ? opponent.id : opponent.name) << std::endl;

	initGameState();
	gameLoop();
	cleanupGameState();

	std::cout << "=====游戏结束=====\n\n";

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
		std::cout << "[Client] 发送INT信息到" << peer.ip << ":" << peer.tcpPort
		          << ", matchId=" << matchId << std::endl;
		_node.interruptMatch(peer.ip, peer.tcpPort, matchId);
		_gameRunning = false; // 确保游戏循环退出
		std::cout << "游戏结束（主动打断）" << std::endl;
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
	std::cout << "\n[请求]来自："
	          << (p.name.empty() ? p.id : p.name)
	          << ".在主界面选择处理选项" << std::endl;
}

void Client::onMatchResponse(const lanp2p::PeerInfo &p, bool accepted, const std::string &matchId)
{
	std::cout << "[回复]来自："
	          << (p.name.empty() ? p.id : p.name)
	          << " match=" << matchId
	          << " accepted=" << (accepted ? "true" : "false") << std::endl;
	if (accepted)
	{
		//我方作为请求发起者时，对方接受后建立本地对局状态，并标记“发起者”身份
		std::lock_guard<std::mutex> lk(_matchMutex);
		_match.inMatch = true;
		_match.peer = p;
		_match.matchId = matchId;
		_iAmMatchInitiator = true;
	}
}

void Client::onMatchInterrupted(const lanp2p::PeerInfo &p, const std::string &matchId)
{
	std::cout << "[打断] 来自："
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
	//仅在对局中且消息来自当前对手时才缓存其落子
	bool shouldProcess = false;
	{
		std::lock_guard<std::mutex> lk(_matchMutex);
		shouldProcess = (_match.inMatch && p.id == _match.peer.id);
	}

	if (shouldProcess)
	{
		// 前置校验坐标范围，忽略非法坐标
		if (x < 1 || x > _boardSize || y < 1 || y > _boardSize || z < 1 || z > _boardSize)
			return;

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
		std::cout << "FATAL:无法初始化棋盘" << std::endl;
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

	std::cout << "棋盘初始化完成，你是玩家" << _myPlayer
	          << " (" << (_iAmMatchInitiator ? "发起者" : "应答者")
	          << ", matchId: " << matchId.substr(0, 4) << "...)." << std::endl;
	if (_myTurn)
	{
		std::cout << "你的回合" << std::endl;
	}
	else
	{
		std::cout << "等待对面下棋" << std::endl;
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
					std::cout << "你赢了" << std::endl;
					_gameRunning = false;
					break;
				}
				_myTurn = false;
				std::cout << "等待对手下棋" << std::endl;
			}
			else
			{
				std::cout << "无效输入" << std::endl;
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
						std::cout << "你个废物" << std::endl;
						_gameRunning = false;
					}
					_opponentMoved = false;
					moved = true;
				}
			}
			if (moved)
			{
				_myTurn = true;
				std::cout << "你的回合" << std::endl;
			}
		}
		std::this_thread::sleep_for(std::chrono::milliseconds(100));
	}
}
