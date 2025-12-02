#include "../include/LanP2PNode.h"

#define _WINSOCK_DEPRECATED_NO_WARNINGS
#include <winsock2.h>
#include <ws2tcpip.h>
#pragma comment(lib, "Ws2_32.lib")

#include <chrono>
#include <random>
#include <sstream>
#include <cstdio>
#include <tuple>

namespace lanp2p
{

	// 关闭套接字（统一封装）
	static void closesock(uintptr_t s)
	{
		closesocket(static_cast<SOCKET>(s));
	}

	// 设置可重用地址（Windows下搭配关闭独占）
	static bool setReuse(uintptr_t s)
	{
		int yes = 1;
		BOOL no = FALSE;
		setsockopt(static_cast<SOCKET>(s), SOL_SOCKET, SO_EXCLUSIVEADDRUSE, (const char *)&no, sizeof(no));
		return setsockopt(static_cast<SOCKET>(s), SOL_SOCKET, SO_REUSEADDR, (const char *)&yes, sizeof(yes)) == 0;
	}

	// 启用UDP广播
	static bool setBroadcast(uintptr_t s)
	{
		int yes = 1;
		return setsockopt(static_cast<SOCKET>(s), SOL_SOCKET, SO_BROADCAST, (const char *)&yes, sizeof(yes)) == 0;
	}

	LanP2PNode::LanP2PNode(uint16_t discoveryPort, uint16_t tcpPort)
		: _discoveryPort(discoveryPort), _tcpPort(tcpPort), _nodeId(randomId())
	{
		WSADATA wsa;
		WSAStartup(MAKEWORD(2, 2), &wsa);
	}

	LanP2PNode::~LanP2PNode()
	{
		stop();
		WSACleanup();
	}

	// 启动完整功能
	void LanP2PNode::start()
	{
		if (_running.exchange(true))
			return;
		_broadcastActive.store(true);
		_udpListenActive.store(true);
		_tcpActive.store(true);
		if (!_maintenanceActive.exchange(true))
		{
			if (!_maintenanceThread.joinable())
				_maintenanceThread = std::thread(&LanP2PNode::peersMaintenanceLoop, this);
		}
		_udpBroadcaster = std::thread(&LanP2PNode::udpBroadcastLoop, this);
		_udpListener = std::thread(&LanP2PNode::udpListenLoop, this);
		_tcpListener = std::thread(&LanP2PNode::tcpListenLoop, this);
	}

	// 仅启动广播与TCP监听
	void LanP2PNode::startBroadcastOnly()
	{
		if (!_running.exchange(true))
		{
			// 首次进入运行状态时可在此做额外初始化
		}
		if (!_broadcastActive.exchange(true))
		{
			if (!_udpBroadcaster.joinable())
				_udpBroadcaster = std::thread(&LanP2PNode::udpBroadcastLoop, this);
		}
		if (!_tcpActive.exchange(true))
		{
			if (!_tcpListener.joinable())
				_tcpListener = std::thread(&LanP2PNode::tcpListenLoop, this);
		}
		if (!_maintenanceActive.exchange(true))
		{
			if (!_maintenanceThread.joinable())
				_maintenanceThread = std::thread(&LanP2PNode::peersMaintenanceLoop, this);
		}
	}

	// 开启UDP发现监听
	void LanP2PNode::startUdpListen()
	{
		if (!_running)
			_running = true;
		if (!_udpListenActive.exchange(true))
		{
			if (!_udpListener.joinable())
				_udpListener = std::thread(&LanP2PNode::udpListenLoop, this);
		}
		if (!_maintenanceActive.exchange(true))
		{
			if (!_maintenanceThread.joinable())
				_maintenanceThread = std::thread(&LanP2PNode::peersMaintenanceLoop, this);
		}
	}

	// 停止UDP发现监听
	void LanP2PNode::stopUdpListen()
	{
		if (!_udpListenActive.exchange(false))
			return;
		// 发送本地UDP数据包唤醒阻塞的recvfrom
		uintptr_t ps = (uintptr_t)socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
		if ((SOCKET)ps != INVALID_SOCKET)
		{
			sockaddr_in a{};
			a.sin_family = AF_INET;
			a.sin_port = htons(_discoveryPort);
			a.sin_addr.s_addr = inet_addr("127.0.0.1");
			sendto(static_cast<SOCKET>(ps), "", 0, 0, (sockaddr *)&a, sizeof(a));
			closesock(ps);
		}
		if (_udpListener.joinable())
			_udpListener.join();
	}

	// 停止全部功能
	void LanP2PNode::stop()
	{
		if (!_running.exchange(false))
			return;
		_broadcastActive.store(false);
		_udpListenActive.store(false);
		_tcpActive.store(false);
		_maintenanceActive.store(false);
		// 发送本地UDP数据包以唤醒阻塞
		uintptr_t ps = (uintptr_t)socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
		if ((SOCKET)ps != INVALID_SOCKET)
		{
			sockaddr_in a{};
			a.sin_family = AF_INET;
			a.sin_port = htons(_discoveryPort);
			a.sin_addr.s_addr = inet_addr("127.0.0.1");
			sendto(static_cast<SOCKET>(ps), "", 0, 0, (sockaddr *)&a, sizeof(a));
			closesock(ps);
		}
		if (_udpBroadcaster.joinable())
			_udpBroadcaster.join();
		if (_udpListener.joinable())
			_udpListener.join();
		if (_tcpListener.joinable())
			_tcpListener.join();
		if (_maintenanceThread.joinable())
			_maintenanceThread.join();
	}

	// 设置回调
	void LanP2PNode::setOnPeerDiscovered(const std::function<void(const PeerInfo &)> &cb)
	{
		_onPeerDiscovered = cb;
	}
	void LanP2PNode::setOnMatchRequest(const std::function<void(const PeerInfo &, const std::string &matchId)> &cb)
	{
		_onMatchRequest = cb;
	}
	void LanP2PNode::setOnMatchResponse(const
	                                    std::function<void(const PeerInfo &, bool accepted, const std::string &matchId)> &cb)
	{
		_onMatchResponse = cb;
	}
	void LanP2PNode::setOnMatchInterrupted(const std::function<void(const PeerInfo &, const std::string &matchId)> &cb)
	{
		_onMatchInterrupted = cb;
	}
	void LanP2PNode::setOnGameMove(const std::function<void(const PeerInfo &, int x, int y, int z)> &cb)
	{
		_onGameMove = cb;
	}

	// 返回当前在线的对端快照（移除超时项）
	std::vector<PeerInfo> LanP2PNode::getPeersSnapshot()
	{
		std::lock_guard<std::mutex> lk(_peersMutex);
		const uint64_t now = nowMs();
		for (auto it = _peersByKey.begin(); it != _peersByKey.end(); )
		{
			if (_matchesByKey.find(it->first) != _matchesByKey.end())
			{
				++it;
				continue;
			}
			if (_peerStaleMs > 0 && (now - it->second.lastSeenMs) > _peerStaleMs)
			{
				const PeerInfo &p = it->second;
				std::printf("[LanP2PNode][DEBUG] 因超时移除 peer (DISC 快照): id=%s ip=%s port=%u lastSeenMs=%llu nowMs=%llu staleMs=%llu\n",
				            p.id.c_str(), p.ip.c_str(), (unsigned)p.tcpPort,
				            (unsigned long long)p.lastSeenMs, (unsigned long long)now, (unsigned long long)_peerStaleMs);
				it = _peersByKey.erase(it);
			}
			else
			{
				++it;
			}
		}
		std::vector<PeerInfo> v;
		v.reserve(_peersByKey.size());
		for (auto& kv : _peersByKey)
			v.push_back(kv.second);
		return v;
	}

	// UDP广播循环（周期广播自身信息）
	void LanP2PNode::udpBroadcastLoop()
	{
		uintptr_t s = (uintptr_t)socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
		if ((SOCKET)s == INVALID_SOCKET)
			return;
		setReuse(s);
		setBroadcast(s);

		// 等待TCP端口绑定完成
		for (int i = 0; i < 50 && _running && _broadcastActive && !_tcpBoundReady.load(); ++i)
			std::this_thread::sleep_for(std::chrono::milliseconds(20));

		sockaddr_in addrBC{};
		addrBC.sin_family = AF_INET;
		addrBC.sin_port = htons(_discoveryPort);
		addrBC.sin_addr.s_addr = INADDR_BROADCAST;
		sockaddr_in addrLoop{};
		addrLoop.sin_family = AF_INET;
		addrLoop.sin_port = htons(_discoveryPort);
		addrLoop.sin_addr.s_addr = inet_addr("127.0.0.1");

		// 初始快速广播5次
		for (int b = 0; b < 5 && _running && _broadcastActive; ++b)
		{
			char buf[256];
			int len;
			if (_nodeName.empty())
				len = std::snprintf(buf, sizeof(buf), "DISC|%s|%u", _nodeId.c_str(), (unsigned)_tcpPort);
			else
				len = std::snprintf(buf, sizeof(buf), "DISC|%s|%u|%s", _nodeId.c_str(), (unsigned)_tcpPort, _nodeName.c_str());
			sendto(static_cast<SOCKET>(s), buf, len, 0, (sockaddr *)&addrBC, sizeof(addrBC));
			sendto(static_cast<SOCKET>(s), buf, len, 0, (sockaddr *)&addrLoop, sizeof(addrLoop));
			std::this_thread::sleep_for(std::chrono::milliseconds(200));
		}
		while (_running && _broadcastActive)
		{
			char buf[256];
			int len;
			if (_nodeName.empty())
				len = std::snprintf(buf, sizeof(buf), "DISC|%s|%u", _nodeId.c_str(), (unsigned)_tcpPort);
			else
				len = std::snprintf(buf, sizeof(buf), "DISC|%s|%u|%s", _nodeId.c_str(), (unsigned)_tcpPort, _nodeName.c_str());
			sendto(static_cast<SOCKET>(s), buf, len, 0, (sockaddr *)&addrBC, sizeof(addrBC));
			sendto(static_cast<SOCKET>(s), buf, len, 0, (sockaddr *)&addrLoop, sizeof(addrLoop));
			for (int i = 0; i < 10 && _running && _broadcastActive; i++)
				std::this_thread::sleep_for(std::chrono::milliseconds(500));
		}
		closesock(s);
	}

	// UDP监听循环（接收DISC并更新对端表）
	void LanP2PNode::udpListenLoop()
	{
		uintptr_t s = (uintptr_t)socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
		if ((SOCKET)s == INVALID_SOCKET)
			return;
		setReuse(s);
		sockaddr_in addr{};
		addr.sin_family = AF_INET;
		addr.sin_port = htons(_discoveryPort);
		addr.sin_addr.s_addr = INADDR_ANY;
		if (bind(static_cast<SOCKET>(s), (sockaddr * )&addr, sizeof(addr)) != 0)
		{
			closesock(s);
			return;
		}
		char buf[512];
		while (_running && _udpListenActive)
		{
			sockaddr_in from{};
			int fl = sizeof(from);
			int r = recvfrom(static_cast<SOCKET>(s), buf, sizeof(buf) - 1, 0, (sockaddr *)&from, &fl);
			if (!_udpListenActive)
				break;
			if (r <= 0)
			{
				std::this_thread::sleep_for(std::chrono::milliseconds(50));
				continue;
			}
			buf[r] = 0;
			std::string line(buf);
			if (line.compare(0, 5, "DISC|") == 0)
			{
				size_t p1 = line.find('|', 5);
				size_t p2 = line.find('|', p1 + 1);
				if (p1 != std::string::npos && p2 != std::string::npos)
				{
					std::string id = line.substr(5, p1 - 5);
					uint16_t tcpPort = (uint16_t)std::stoi(line.substr(p1 + 1));
					std::string ip = inet_ntoa(from.sin_addr);
					std::string name;
					if (p2 != std::string::npos && p2 + 1 < line.size())
						name = line.substr(p2 + 1);
					if (id != _nodeId)
					{
						PeerInfo info;
						info.id = id;
						info.name = name;
						info.ip = ip;
						info.tcpPort = tcpPort;
						info.lastSeenMs = nowMs();
						std::string key = ip + ":" + std::to_string(tcpPort) + ":" + id;
						bool notify = false;
						{
							std::lock_guard<std::mutex> lk(_peersMutex);
							// 优先保留局域网IP记录，若存在则忽略相同ID的回环地址
							bool hasNonLoopForId = false;
							std::string loopKeyToErase;
							for (auto it = _peersByKey.begin(); it != _peersByKey.end(); ++it)
							{
								const PeerInfo &e = it->second;
								if (e.id == id)
								{
									if (e.ip != "127.0.0.1")
										hasNonLoopForId = true;
									else
										loopKeyToErase = it->first;
								}
							}
							if (ip == "127.0.0.1" && hasNonLoopForId)
							{
								// 忽略回环地址
							}
							else
							{
								if (ip != "127.0.0.1" && !loopKeyToErase.empty())
									_peersByKey.erase(loopKeyToErase);
								_peersByKey[key] = info;
								notify = true;
							}
						}
						if (notify && _onPeerDiscovered)
							_onPeerDiscovered(info);
					}
				}
			}
		}
		closesock(s);
	}

	// 发送落子（带重试）
	bool LanP2PNode::sendGameMove(const std::string &peerIp, uint16_t peerTcpPort, int x, int y, int z)
	{
		for (int attempt = 0; attempt < _maxSendRetries; ++attempt)
		{
			uintptr_t s = (uintptr_t)socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
			if ((SOCKET)s == INVALID_SOCKET)
				return false;
			sockaddr_in addr{};
			addr.sin_family = AF_INET;
			addr.sin_port = htons(peerTcpPort);
			addr.sin_addr.s_addr = inet_addr(peerIp.c_str());
			if (connect(static_cast<SOCKET>(s), (sockaddr * )&addr, sizeof(addr)) != 0)
			{
				closesock(s);
				std::this_thread::sleep_for(std::chrono::milliseconds(50));
				continue;
			}
			std::ostringstream oss;
			oss << "MOVE|" << x << "|" << y << "|" << z << "|";
			bool ok = tcpSendFramed(s, oss.str());
			closesock(s);
			if (ok)
				return true;
			std::this_thread::sleep_for(std::chrono::milliseconds(50));
		}
		return false;
	}

	// TCP监听循环（接受连接并分发协议）
	void LanP2PNode::tcpListenLoop()
	{
		uintptr_t s = (uintptr_t)socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
		if ((SOCKET)s == INVALID_SOCKET)
			return;
		setReuse(s);

		uint16_t chosen = 0;
		if (_tcpPort == 0)
		{
			sockaddr_in addr{};
			addr.sin_family = AF_INET;
			addr.sin_port = htons(0);
			addr.sin_addr.s_addr = INADDR_ANY;
			if (bind(static_cast<SOCKET>(s), (sockaddr * )&addr, sizeof(addr)) == 0)
			{
				int len = sizeof(addr);
				if (getsockname(static_cast<SOCKET>(s), (sockaddr * )&addr, &len) == 0)
				{
					chosen = ntohs(addr.sin_port);
				}
			}
		}
		else
		{
			for (int attempt = 0; attempt < 128; ++attempt)
			{
				uint16_t portTry = (uint16_t)(_tcpPort + attempt);
				sockaddr_in addr{};
				addr.sin_family = AF_INET;
				addr.sin_port = htons(portTry);
				addr.sin_addr.s_addr = INADDR_ANY;
				if (bind(static_cast<SOCKET>(s), (sockaddr * )&addr, sizeof(addr)) == 0)
				{
					chosen = portTry;
					break;
				}
			}
		}
		if (chosen == 0)
		{
			closesock(s);
			return;
		}
		_tcpPort = chosen;
		_tcpBoundReady.store(true);

		if (listen(static_cast<SOCKET>(s), 8) != 0)
		{
			closesock(s);
			return;
		}
		while (_running && _tcpActive)
		{
			sockaddr_in cli{};
			int cl = sizeof(cli);
			uintptr_t c = (uintptr_t)accept(static_cast<SOCKET>(s), (sockaddr *)&cli, &cl);
			if ((SOCKET)c == INVALID_SOCKET)
			{
				std::this_thread::sleep_for(std::chrono::milliseconds(50));
				continue;
			}
			std::string rip = inet_ntoa(cli.sin_addr);
			std::thread(&LanP2PNode::tcpConnectionHandler, this, c, rip).detach();
		}
		closesock(s);
	}

	// 按 ip+id 查找对端TCP端口
	uint16_t LanP2PNode::findPeerTcpPort(const std::string &ip, const std::string &id)
	{
		std::lock_guard<std::mutex> lk(_peersMutex);
		for (auto& kv : _peersByKey)
		{
			const PeerInfo &p = kv.second;
			if (p.ip == ip && p.id == id)
				return p.tcpPort;
		}
		return 0;
	}

	// TCP连接处理（解析协议并回调上层）
	void LanP2PNode::tcpConnectionHandler(uintptr_t sock, std::string remoteIp)
	{
		std::string payload;
		while (_running && tcpRecvFramed(sock, payload))
		{
			const uint64_t ts = nowMs();
			if (payload.compare(0, 4, "REQ|") == 0)
			{
				// 格式：REQ|fromId|fromPort|matchId|[toId]|
				size_t p1 = payload.find('|', 4);
				size_t p2 = (p1 != std::string::npos) ? payload.find('|', p1 + 1) : std::string::npos;
				size_t p3 = (p2 != std::string::npos) ? payload.find('|', p2 + 1) : std::string::npos;
				size_t p4 = (p3 != std::string::npos) ? payload.find('|', p3 + 1) : std::string::npos;
				if (p1 != std::string::npos && p2 != std::string::npos && p3 != std::string::npos)
				{
					std::string fromId = payload.substr(4, p1 - 4);
					uint16_t fromPort = 0;
					try
					{
						fromPort = (uint16_t)std::stoi(payload.substr(p1 + 1, p2 - (p1 + 1)));
					}
					catch (...)
					{
						fromPort = 0;
					}
					std::string matchId = payload.substr(p2 + 1, p3 - (p2 + 1));
					std::string toId;
					if (p4 != std::string::npos)
						toId = payload.substr(p3 + 1, p4 - (p3 + 1));
					if (fromId == _nodeId)
						continue; // 忽略自身请求
					if (!toId.empty() && toId != _nodeId)
						continue; // 目标不是我则忽略
					// 用消息更新/插入对端表
					PeerInfo piMsg;
					piMsg.id = fromId;
					piMsg.ip = remoteIp;
					piMsg.tcpPort = fromPort;
					piMsg.lastSeenMs = ts;
					{
						std::lock_guard<std::mutex> lk(_peersMutex);
						std::string key = piMsg.ip + ":" + std::to_string(piMsg.tcpPort) + ":" + piMsg.id;
						auto it = _peersByKey.find(key);
						if (it != _peersByKey.end())
							piMsg.name = it->second.name;
						_peersByKey[key] = piMsg;
					}
					if (_onMatchRequest)
						_onMatchRequest(piMsg, matchId);
					// 标记匹配为活跃（心跳）
					markMatchActive(remoteIp, fromPort, fromId, matchId);
				}
			}
			else if (payload.compare(0, 5, "RESP|") == 0)
			{
				// 格式：RESP|fromId|matchId|1/0|
				size_t p1 = payload.find('|', 5);
				size_t p2 = payload.find('|', p1 + 1);
				size_t p3 = payload.find('|', p2 + 1);
				if (p1 != std::string::npos && p2 != std::string::npos && p3 != std::string::npos)
				{
					std::string fromId = payload.substr(5, p1 - 5);
					std::string matchId = payload.substr(p1 + 1, p2 - (p1 + 1));
					bool accepted = payload.substr(p2 + 1, p3 - (p2 + 1)) == "1";
					uint16_t ptcp = findPeerTcpPort(remoteIp, fromId);
					PeerInfo pi;
					pi.id = fromId;
					pi.ip = remoteIp;
					pi.tcpPort = ptcp;
					pi.lastSeenMs = ts;
					{
						std::lock_guard<std::mutex> lk(_peersMutex);
						for (auto& kv : _peersByKey)
						{
							const PeerInfo &pr = kv.second;
							if (pr.ip == remoteIp && pr.id == fromId)
							{
								pi.name = pr.name;
								break;
							}
						}
					}
					if (_onMatchResponse)
						_onMatchResponse(pi, accepted, matchId);
					if (!accepted)
					{
						clearMatch(remoteIp, ptcp, fromId, matchId, false);
					}
				}
			}
			else if (payload.compare(0, 4, "INT|") == 0)
			{
				// 格式：INT|fromId|matchId|
				size_t p1 = payload.find('|', 4);
				size_t p2 = payload.find('|', p1 + 1);
				if (p1 != std::string::npos && p2 != std::string::npos)
				{
					std::string fromId = payload.substr(4, p1 - 4);
					if (fromId == _nodeId)
						continue;
					std::string matchId = payload.substr(p1 + 1, p2 - (p1 + 1));
					uint16_t ptcp = findPeerTcpPort(remoteIp, fromId);
					PeerInfo pi;
					pi.id = fromId;
					pi.ip = remoteIp;
					pi.tcpPort = ptcp;
					pi.lastSeenMs = ts;
					{
						std::lock_guard<std::mutex> lk(_peersMutex);
						for (auto& kv : _peersByKey)
						{
							const PeerInfo &pr = kv.second;
							if (pr.ip == remoteIp && pr.id == fromId)
							{
								pi.name = pr.name;
								break;
							}
						}
					}
					if (_onMatchInterrupted)
						_onMatchInterrupted(pi, matchId);
					clearMatch(remoteIp, ptcp, fromId, matchId, false);
				}
			}
			else if (payload.compare(0, 3, "HB|") == 0)
			{
				// 格式：HB|fromId|matchId|
				size_t p1 = payload.find('|', 3);
				size_t p2 = (p1 != std::string::npos) ? payload.find('|', p1 + 1) : std::string::npos;
				if (p1 != std::string::npos && p2 != std::string::npos)
				{
					std::string fromId = payload.substr(3, p1 - 3);
					std::string matchId = payload.substr(p1 + 1, p2 - (p1 + 1));
					std::string key = remoteIp + ":" + std::to_string(findPeerTcpPort(remoteIp, fromId)) + ":" + fromId;
					{
						std::lock_guard<std::mutex> lk(_peersMutex);
						auto it = _matchesByKey.find(key);
						if (it != _matchesByKey.end() && it->second.matchId == matchId)
							it->second.lastHbMs = nowMs();
					}
				}
			}
			else if (payload.compare(0, 5, "MOVE|") == 0)
			{
				// 格式：MOVE|x|y|z|
				size_t p1 = payload.find('|', 5);
				size_t p2 = (p1 != std::string::npos) ? payload.find('|', p1 + 1) : std::string::npos;
				size_t p3 = (p2 != std::string::npos) ? payload.find('|', p2 + 1) : std::string::npos;
				if (p1 != std::string::npos && p2 != std::string::npos && p3 != std::string::npos)
				{
					try
					{
						int x = std::stoi(payload.substr(5, p1 - 5));
						int y = std::stoi(payload.substr(p1 + 1, p2 - (p1 + 1)));
						int z = std::stoi(payload.substr(p2 + 1, p3 - (p2 + 1)));

						PeerInfo pi;
						pi.ip = remoteIp;
						pi.lastSeenMs = ts;
						{
							std::lock_guard<std::mutex> lk(_peersMutex);
							for (auto& kv : _peersByKey)
							{
								const PeerInfo &p = kv.second;
								if (p.ip == remoteIp)
								{
									pi = p;
									break;
								}
							}
						}
						if (_onGameMove)
						{
							_onGameMove(pi, x, y, z);
						}
					}
					catch (...) { /* 转换失败忽略 */ }
				}

			}
		}
		closesock(sock);
	}

	// TCP有边界帧发送
	bool LanP2PNode::tcpSendFramed(uintptr_t sock, const std::string &payload)
	{
		uint32_t n = (uint32_t)payload.size();
		uint32_t be = htonl(n);
		int off = 0;
		int r = 0;
		while (off < 4)
		{
			r = send(static_cast<SOCKET>(sock), ((const char *)&be) + off, 4 - off, 0);
			if (r <= 0)
				return false;
			off += r;
		}
		off = 0;
		while (off < (int)n)
		{
			r = send(static_cast<SOCKET>(sock), payload.data() + off, (int)n - off, 0);
			if (r <= 0)
				return false;
			off += r;
		}
		return true;
	}

	// TCP有边界帧接收
	bool LanP2PNode::tcpRecvFramed(uintptr_t sock, std::string &outPayload)
	{
		uint32_t be = 0;
		int got = 0;
		int r = 0;
		while (got < 4)
		{
			r = recv(static_cast<SOCKET>(sock), ((char *)&be) + got, 4 - got, 0);
			if (r <= 0)
				return false;
			got += r;
		}
		uint32_t n = ntohl(be);
		outPayload.resize(n);
		int off = 0;
		while (off < (int)n)
		{
			r = recv(static_cast<SOCKET>(sock), &outPayload[off], (int)n - off, 0);
			if (r <= 0)
				return false;
			off += r;
		}
		return true;
	}

	// 发送匹配请求（带重试）
	bool LanP2PNode::sendMatchRequest(const std::string &peerIp, uint16_t peerTcpPort, const std::string &matchId)
	{
		for (int attempt = 0; attempt < _maxSendRetries; ++attempt)
		{
			uintptr_t s = (uintptr_t)socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
			if ((SOCKET)s == INVALID_SOCKET)
				return false;
			sockaddr_in addr{};
			addr.sin_family = AF_INET;
			addr.sin_port = htons(peerTcpPort);
			addr.sin_addr.s_addr = inet_addr(peerIp.c_str());
			if (connect(static_cast<SOCKET>(s), (sockaddr * )&addr, sizeof(addr)) != 0)
			{
				closesock(s);
				std::this_thread::sleep_for(std::chrono::milliseconds(100));
				continue;
			}
			std::string toId;
			{
				std::lock_guard<std::mutex> lk(_peersMutex);
				for (auto& kv : _peersByKey)
				{
					const PeerInfo &p = kv.second;
					if (p.ip == peerIp && p.tcpPort == peerTcpPort)
					{
						toId = p.id;
						break;
					}
				}
			}
			std::ostringstream oss;
			if (toId.empty())
				oss << "REQ|" << _nodeId << "|" << _tcpPort << "|" << matchId << "|";
			else
				oss << "REQ|" << _nodeId << "|" << _tcpPort << "|" << matchId << "|" << toId << "|";
			bool ok = tcpSendFramed(s, oss.str());
			closesock(s);
			if (ok)
			{
				if (!toId.empty())
					markMatchActive(peerIp, peerTcpPort, toId, matchId);
				return true;
			}
			std::this_thread::sleep_for(std::chrono::milliseconds(100));
		}
		return false;
	}

	// 响应匹配请求（带重试）
	bool LanP2PNode::respondToMatch(const std::string &peerIp, uint16_t peerTcpPort, const std::string &matchId,
	                                bool accept)
	{
		for (int attempt = 0; attempt < _maxSendRetries; ++attempt)
		{
			uintptr_t s = (uintptr_t)socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
			if ((SOCKET)s == INVALID_SOCKET)
				return false;
			sockaddr_in addr{};
			addr.sin_family = AF_INET;
			addr.sin_port = htons(peerTcpPort);
			addr.sin_addr.s_addr = inet_addr(peerIp.c_str());
			if (connect(static_cast<SOCKET>(s), (sockaddr * )&addr, sizeof(addr)) != 0)
			{
				closesock(s);
				std::this_thread::sleep_for(std::chrono::milliseconds(100));
				continue;
			}
			std::ostringstream oss;
			oss << "RESP|" << _nodeId << "|" << matchId << "|" << (accept ? "1" : "0") << "|";
			bool ok = tcpSendFramed(s, oss.str());
			closesock(s);
			if (ok)
				return true;
			std::this_thread::sleep_for(std::chrono::milliseconds(100));
		}
		return false;
	}

	// 发送匹配中断（带重试）
	bool LanP2PNode::interruptMatch(const std::string &peerIp, uint16_t peerTcpPort, const std::string &matchId)
	{
		for (int attempt = 0; attempt < _maxSendRetries; ++attempt)
		{
			uintptr_t s = (uintptr_t)socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
			if ((SOCKET)s == INVALID_SOCKET)
				return false;
			sockaddr_in addr{};
			addr.sin_family = AF_INET;
			addr.sin_port = htons(peerTcpPort);
			addr.sin_addr.s_addr = inet_addr(peerIp.c_str());
			if (connect(static_cast<SOCKET>(s), (sockaddr * )&addr, sizeof(addr)) != 0)
			{
				closesock(s);
				std::this_thread::sleep_for(std::chrono::milliseconds(100));
				continue;
			}
			std::ostringstream oss;
			oss << "INT|" << _nodeId << "|" << matchId << "|";
			bool ok = tcpSendFramed(s, oss.str());
			closesock(s);
			if (ok)
				return true;
			std::this_thread::sleep_for(std::chrono::milliseconds(100));
		}
		return false;
	}

	// 发送TCP心跳（带重试）
	bool LanP2PNode::sendTcpHeartbeat(const std::string &ip, uint16_t port, const std::string &matchId)
	{
		for (int attempt = 0; attempt < _maxSendRetries; ++attempt)
		{
			uintptr_t s = (uintptr_t)socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
			if ((SOCKET)s == INVALID_SOCKET)
				return false;
			sockaddr_in addr{};
			addr.sin_family = AF_INET;
			addr.sin_port = htons(port);
			addr.sin_addr.s_addr = inet_addr(ip.c_str());
			if (connect(static_cast<SOCKET>(s), (sockaddr * )&addr, sizeof(addr)) != 0)
			{
				closesock(s);
				std::this_thread::sleep_for(std::chrono::milliseconds(100));
				continue;
			}
			std::ostringstream oss;
			oss << "HB|" << _nodeId << "|" << matchId << "|";
			bool ok = tcpSendFramed(s, oss.str());
			closesock(s);
			if (ok)
				return true;
			std::this_thread::sleep_for(std::chrono::milliseconds(100));
		}
		return false;
	}

	// 工具：当前毫秒时间戳
	uint64_t LanP2PNode::nowMs()
	{
		using namespace std::chrono;
		return duration_cast<milliseconds>(steady_clock::now().time_since_epoch()).count();
	}

	// 工具：生成随机节点ID
	std::string LanP2PNode::randomId()
	{
		std::random_device rd;
		std::mt19937_64 rng(rd());
		std::uniform_int_distribution<uint64_t> dist;
		uint64_t v = dist(rng);
		char buf[17];
		std::snprintf(buf, sizeof(buf), "%016llx", (unsigned long long)v);
		return std::string(buf);
	}

	// 工具：生成随机匹配ID
	std::string LanP2PNode::generateMatchId()
	{
		std::random_device rd;
		std::mt19937_64 rng(rd());
		std::uniform_int_distribution<uint64_t> dist;
		uint64_t v = dist(rng);
		char buf[17];
		std::snprintf(buf, sizeof(buf), "%016llx", (unsigned long long)v);
		return std::string(buf);
	}

	// 后台维护线程：清理对端与维持心跳
	void LanP2PNode::peersMaintenanceLoop()
	{
		while (_running && _maintenanceActive)
		{
			const uint64_t now = nowMs();
			// 1) 移除基于DISC的超时对端（若该对端存在活跃匹配，则暂不移除）
			if (_peerStaleMs > 0)
			{
				std::lock_guard<std::mutex> lk(_peersMutex);
				for (auto it = _peersByKey.begin(); it != _peersByKey.end(); )
				{
					if (_matchesByKey.find(it->first) != _matchesByKey.end())
					{
						++it;
						continue;
					}
					if ((now - it->second.lastSeenMs) > _peerStaleMs)
					{
						const PeerInfo &p = it->second;
						std::printf("[LanP2PNode][DEBUG] 因超时移除 peer (DISC维护): id=%s ip=%s port=%u lastSeenMs=%llu nowMs=%llu staleMs=%llu\n",
						            p.id.c_str(), p.ip.c_str(), (unsigned)p.tcpPort,
						            (unsigned long long)p.lastSeenMs, (unsigned long long)now, (unsigned long long)_peerStaleMs);
						it = _peersByKey.erase(it);
					}
					else ++it;
				}
			}
			// 2) 构造匹配快照（避免持锁网络操作）
			std::vector<std::tuple<std::string, std::string, uint16_t, std::string, uint64_t>> matchSnapshot;
			{
				std::lock_guard<std::mutex> lk(_peersMutex);
				for (auto& kv : _matchesByKey)
				{
					const std::string &key = kv.first;
					size_t p1 = key.find(':');
					size_t p2 = (p1 == std::string::npos) ? std::string::npos : key.find(':', p1 + 1);
					std::string ip = (p1 == std::string::npos) ? std::string() : key.substr(0, p1);
					uint16_t port = 0;
					if (p1 != std::string::npos && p2 != std::string::npos)
					{
						try
						{
							port = (uint16_t)std::stoi(key.substr(p1 + 1, p2 - (p1 + 1)));
						}
						catch (...)
						{
							port = 0;
						}
					}
					matchSnapshot.emplace_back(key, ip, port, kv.second.matchId, kv.second.lastHbMs);
				}
			}
			// 3) 根据快照发送心跳与处理超时
			for (auto& t : matchSnapshot)
			{
				const std::string &key = std::get<0>(t);
				const std::string &ip = std::get<1>(t);
				uint16_t port = std::get<2>(t);
				const std::string &matchId = std::get<3>(t);
				uint64_t last = std::get<4>(t);
				const uint64_t now2 = nowMs();
				if (_matchHeartbeatIntervalMs > 0 && (now2 - last) >= _matchHeartbeatIntervalMs)
				{
					sendTcpHeartbeat(ip, port, matchId);
					std::lock_guard<std::mutex> lk(_peersMutex);
					auto it = _matchesByKey.find(key);
					if (it != _matchesByKey.end() && it->second.matchId == matchId)
						it->second.lastHbMs = now2;
				}
				if (_matchHeartbeatTimeoutMs > 0 && (now2 - last) > _matchHeartbeatTimeoutMs)
				{
					std::string peerId;
					size_t p1 = key.find(':');
					size_t p2 = (p1 == std::string::npos) ? std::string::npos : key.find(':', p1 + 1);
					if (p2 != std::string::npos)
						peerId = key.substr(p2 + 1);
					std::printf("[LanP2PNode][DEBUG] 因超时移除 peer (HB 超时): id=%s ip=%s port=%u matchId=%s lastHbMs=%llu nowMs=%llu timeoutMs=%llu\n",
					            peerId.c_str(), ip.c_str(), (unsigned)port, matchId.c_str(),
					            (unsigned long long)last, (unsigned long long)now2, (unsigned long long)_matchHeartbeatTimeoutMs);
					clearMatch(ip, port, peerId, matchId, true);
				}
			}
			// 4) 维持5秒节奏（细分为10次以便更及时响应停止）
			for (int i = 0; i < 10 && _running && _maintenanceActive; ++i)
				std::this_thread::sleep_for(std::chrono::milliseconds(500));
		}
	}

	// 在匹配表中标记对战活跃（用于心跳）
	void LanP2PNode::markMatchActive(const std::string &ip, uint16_t tcpPort, const std::string &peerId,
	                                 const std::string &matchId)
	{
		std::lock_guard<std::mutex> lk(_peersMutex);
		std::string key = ip + ":" + std::to_string(tcpPort) + ":" + peerId;
		auto &st = _matchesByKey[key];
		st.matchId = matchId;
		st.lastHbMs = nowMs();
	}

	// 清理匹配状态；必要时回调上层中断事件
	void LanP2PNode::clearMatch(const std::string &ip, uint16_t tcpPort, const std::string &peerId,
	                            const std::string &matchId, bool notify)
	{
		{
			std::lock_guard<std::mutex> lk(_peersMutex);
			_matchesByKey.erase(ip + ":" + std::to_string(tcpPort) + ":" + peerId);
			for (auto it = _peersByKey.begin(); it != _peersByKey.end(); )
			{
				const PeerInfo &p = it->second;
				if (p.ip == ip && p.tcpPort == tcpPort && p.id == peerId)
					it = _peersByKey.erase(it);
				else ++it;
			}
		}
		if (notify && _onMatchInterrupted)
		{
			PeerInfo pi;
			pi.id = peerId;
			pi.ip = ip;
			pi.tcpPort = tcpPort;
			pi.lastSeenMs = nowMs();
			_onMatchInterrupted(pi, matchId);
		}
	}

} // namespace lanp2p
