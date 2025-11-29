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

	static void closesock(uintptr_t s)//关闭套接字
	{
		closesocket(static_cast<SOCKET>(s));
	}

	static bool setReuse(uintptr_t s)//启用地址重复使用
	{
		int yes =1;
		BOOL no = FALSE;
		setsockopt(static_cast<SOCKET>(s), SOL_SOCKET, SO_EXCLUSIVEADDRUSE, (const char*)&no, sizeof(no));
		return setsockopt(static_cast<SOCKET>(s), SOL_SOCKET, SO_REUSEADDR, (const char*)&yes, sizeof(yes)) ==0;
	}

	static bool setBroadcast(uintptr_t s)//初始化广播功能
	{
		int yes =1;
		return setsockopt(static_cast<SOCKET>(s), SOL_SOCKET, SO_BROADCAST, (const char*)&yes, sizeof(yes)) ==0;
	}

	LanP2PNode::LanP2PNode(uint16_t discoveryPort, uint16_t tcpPort)
		: _discoveryPort(discoveryPort), _tcpPort(tcpPort), _nodeId(randomId())
	{
		WSADATA wsa;
		WSAStartup(MAKEWORD(2,2), &wsa);
	}

	LanP2PNode::~LanP2PNode()
	{
		stop();
		WSACleanup();
	}

	void LanP2PNode::start()//初始化
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

	void LanP2PNode::startBroadcastOnly()//启动除了UDP监听以外的功能
	{
		if (!_running.exchange(true))
		{
//首次进入运行状态的代码
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

	void LanP2PNode::startUdpListen()//启用UDP监听
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

	void LanP2PNode::stopUdpListen()//停止UDP监听
	{
		if (!_udpListenActive.exchange(false))
			return;
//发送一个数据包到本地的发现端口以解除阻塞
		uintptr_t ps = (uintptr_t)socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
		if ((SOCKET)ps != INVALID_SOCKET)
		{
			sockaddr_in a{};
			a.sin_family = AF_INET;
			a.sin_port = htons(_discoveryPort);
			a.sin_addr.s_addr = inet_addr("127.0.0.1");
			sendto(static_cast<SOCKET>(ps), "",0,0, (sockaddr*)&a, sizeof(a));
			closesock(ps);
		}
		if (_udpListener.joinable())
			_udpListener.join();
	}

	void LanP2PNode::stop()//停止所有功能
	{
		if (!_running.exchange(false))
			return;
		_broadcastActive.store(false);
		_udpListenActive.store(false);
		_tcpActive.store(false);
		_maintenanceActive.store(false);
		uintptr_t ps = (uintptr_t)socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
		if ((SOCKET)ps != INVALID_SOCKET)
		{
			sockaddr_in a{};
			a.sin_family = AF_INET;
			a.sin_port = htons(_discoveryPort);
			a.sin_addr.s_addr = inet_addr("127.0.0.1");
			sendto(static_cast<SOCKET>(ps), "",0,0, (sockaddr*)&a, sizeof(a));
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

	void LanP2PNode::setOnPeerDiscovered(const std::function<void(const PeerInfo&)>& cb)//设置发现玩家时调用的函数
	{
		_onPeerDiscovered = cb;
	}
	void LanP2PNode::setOnMatchRequest(const std::function<void(const PeerInfo&, const std::string& matchId)>& cb)//设置收到匹配请求时调用的函数
	{
		_onMatchRequest = cb;
	}
	void LanP2PNode::setOnMatchResponse(const std::function<void(const PeerInfo&, bool accepted, const std::string& matchId)>& cb)//设置收到匹配响应时调用的函数
	{
		_onMatchResponse = cb;
	}
	void LanP2PNode::setOnMatchInterrupted(const std::function<void(const PeerInfo&, const std::string& matchId)>& cb)//设置匹配中断时调用的函数
	{
		_onMatchInterrupted = cb;
	}

	std::vector<PeerInfo> LanP2PNode::getPeersSnapshot()//获取当前在线玩家列表
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
			if (_peerStaleMs >0 && (now - it->second.lastSeenMs) > _peerStaleMs)
			{
				const PeerInfo& p = it->second;
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

	void LanP2PNode::udpBroadcastLoop()//udp广播循环
	{
		uintptr_t s = (uintptr_t)socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
		if ((int)s <0)
			return;
		setReuse(s);
		setBroadcast(s);

		//等待绑定TCP端口完成
		for (int i=0; i<50 && _running && _broadcastActive && !_tcpBoundReady.load(); ++i)
			std::this_thread::sleep_for(std::chrono::milliseconds(20));

		sockaddr_in addrBC{};
		addrBC.sin_family = AF_INET;
		addrBC.sin_port = htons(_discoveryPort);
		addrBC.sin_addr.s_addr = INADDR_BROADCAST;
		sockaddr_in addrLoop{};
		addrLoop.sin_family = AF_INET;
		addrLoop.sin_port = htons(_discoveryPort);
		addrLoop.sin_addr.s_addr = inet_addr("127.0.0.1");

		//初始快速广播5次
		for (int b =0; b <5 && _running && _broadcastActive; ++b)
		{
			char buf[256];
			int len;
			if (_nodeName.empty())
				len = std::snprintf(buf, sizeof(buf), "DISC|%s|%u", _nodeId.c_str(), (unsigned)_tcpPort);
			else
				len = std::snprintf(buf, sizeof(buf), "DISC|%s|%u|%s", _nodeId.c_str(), (unsigned)_tcpPort, _nodeName.c_str());
			sendto((int)s, buf, len,0, (sockaddr*)&addrBC, sizeof(addrBC));
			sendto((int)s, buf, len,0, (sockaddr*)&addrLoop, sizeof(addrLoop));
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
			sendto((int)s, buf, len,0, (sockaddr*)&addrBC, sizeof(addrBC));
			sendto((int)s, buf, len,0, (sockaddr*)&addrLoop, sizeof(addrLoop));
			for (int i =0; i <10 && _running && _broadcastActive; i++)
				std::this_thread::sleep_for(std::chrono::milliseconds(500));
		}
		closesock(s);
	}

	void LanP2PNode::udpListenLoop()//udp监听循环
	{
		uintptr_t s = (uintptr_t)socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
		if ((SOCKET)s == INVALID_SOCKET)
			return;
		setReuse(s);
		sockaddr_in addr{};
		addr.sin_family = AF_INET;
		addr.sin_port = htons(_discoveryPort);
		addr.sin_addr.s_addr = INADDR_ANY;
		if (bind(static_cast<SOCKET>(s), (sockaddr*)&addr, sizeof(addr)) !=0)
		{
			closesock(s);
			return;
		}
		char buf[512];
		while (_running && _udpListenActive)
		{
			sockaddr_in from{};
			int fl = sizeof(from);
			int r = recvfrom(static_cast<SOCKET>(s), buf, sizeof(buf) -1,0, (sockaddr*)&from, &fl);
			if (!_udpListenActive)
				break;
			if (r <=0)
			{
				std::this_thread::sleep_for(std::chrono::milliseconds(50));
				continue;
			}
			buf[r] =0;
			std::string line(buf);
			if (line.compare(0,5, "DISC|") ==0)
			{
				size_t p1 = line.find('|',5);
				size_t p2 = line.find('|', p1 +1);
				if (p1 != std::string::npos && p2 != std::string::npos)
				{
					std::string id = line.substr(5, p1 -5);
					uint16_t tcpPort = (uint16_t)std::stoi(line.substr(p1 +1));
					std::string ip = inet_ntoa(from.sin_addr);
					std::string name;
					if (p2 != std::string::npos && p2 +1 < line.size())
						name = line.substr(p2 +1);
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
							//优先保留局域网ip，忽略重复的环回地址
							bool hasNonLoopForId = false;
							std::string loopKeyToErase;
							for (auto it = _peersByKey.begin(); it != _peersByKey.end(); ++it)
							{
								const PeerInfo& e = it->second;
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

	void LanP2PNode::tcpListenLoop()//tcp监听循环
	{
		uintptr_t s = (uintptr_t)socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
		if ((SOCKET)s == INVALID_SOCKET)
			return;
		setReuse(s);

		uint16_t chosen =0;
		if (_tcpPort ==0)
		{
			sockaddr_in addr{};
			addr.sin_family = AF_INET;
			addr.sin_port = htons(0);
			addr.sin_addr.s_addr = INADDR_ANY;
			if (bind(static_cast<SOCKET>(s), (sockaddr*)&addr, sizeof(addr)) ==0)
			{
				int len = sizeof(addr);
				if (getsockname(static_cast<SOCKET>(s), (sockaddr*)&addr, &len) ==0)
				{
					chosen = ntohs(addr.sin_port);
				}
			}
		}
		else
		{
			for (int attempt =0; attempt <128; ++attempt)
			{
				uint16_t portTry = (uint16_t)(_tcpPort + attempt);
				sockaddr_in addr{};
				addr.sin_family = AF_INET;
				addr.sin_port = htons(portTry);
				addr.sin_addr.s_addr = INADDR_ANY;
				if (bind(static_cast<SOCKET>(s), (sockaddr*)&addr, sizeof(addr)) ==0)
				{
					chosen = portTry;
					break;
				}
			}
		}
		if (chosen ==0)
		{
			closesock(s);
			return;
		}
		_tcpPort = chosen;
		_tcpBoundReady.store(true);

		if (listen(static_cast<SOCKET>(s),8) !=0)
		{
			closesock(s);
			return;
		}
		while (_running && _tcpActive)
		{
			sockaddr_in cli{};
			int cl = sizeof(cli);
			uintptr_t c = (uintptr_t)accept(static_cast<SOCKET>(s), (sockaddr*)&cli, &cl);
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

	uint16_t LanP2PNode::findPeerTcpPort(const std::string& ip, const std::string& id)//按ip和id查找tcp端口
	{
		std::lock_guard<std::mutex> lk(_peersMutex);
		for (auto& kv : _peersByKey)
		{
			const PeerInfo& p = kv.second;
			if (p.ip == ip && p.id == id)
				return p.tcpPort;
		}
		return 0;
	}

	void LanP2PNode::tcpConnectionHandler(uintptr_t sock, std::string remoteIp)//tcp连接处理函数
	{
		std::string payload;
		while (_running && tcpRecvFramed(sock, payload))
		{
			const uint64_t ts = nowMs();
			if (payload.compare(0,4, "REQ|") ==0)
			{
// Formats supported:
// v1: REQ|fromId|fromPort|matchId|
// v2: REQ|fromId|fromPort|matchId|toId|
				size_t p1 = payload.find('|',4); // end of fromId
				size_t p2 = (p1!=std::string::npos)?payload.find('|', p1 +1):std::string::npos; // end of fromPort
				size_t p3 = (p2!=std::string::npos)?payload.find('|', p2 +1):std::string::npos; // end of matchId
				size_t p4 = (p3!=std::string::npos)?payload.find('|', p3 +1):std::string::npos; // end of toId (optional)
				if (p1 != std::string::npos && p2 != std::string::npos && p3 != std::string::npos)
				{
					std::string fromId = payload.substr(4, p1 -4);
					uint16_t fromPort =0;
					try
					{
						fromPort = (uint16_t)std::stoi(payload.substr(p1 +1, p2 - (p1 +1)));
					}
					catch (...)
					{
						fromPort =0;
					}
					std::string matchId = payload.substr(p2 +1, p3 - (p2 +1));
					std::string toId;
					if (p4 != std::string::npos)
						toId = payload.substr(p3 +1, p4 - (p3 +1));
// Ignore self-originated requests
					if (fromId == _nodeId)
						continue;
// If targeted to a specific id and it is not me, ignore
					if (!toId.empty() && toId != _nodeId)
						continue;
// Update peers table with info from message
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
// mark this match active for heartbeat
					markMatchActive(remoteIp, fromPort, fromId, matchId);
				}
			}
			else if (payload.compare(0,5, "RESP|") ==0)
			{
				size_t p1 = payload.find('|',5);
				size_t p2 = payload.find('|', p1 +1);
				size_t p3 = payload.find('|', p2 +1);
				if (p1 != std::string::npos && p2 != std::string::npos && p3 != std::string::npos)
				{
					std::string fromId = payload.substr(5, p1 -5);
					std::string matchId = payload.substr(p1 +1, p2 - (p1 +1));
					bool accepted = payload.substr(p2 +1, p3 - (p2 +1)) == "1";
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
							const PeerInfo& pr = kv.second;
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
// clear match state if rejected
						clearMatch(remoteIp, ptcp, fromId, matchId, false);
					}
				}
			}
			else if (payload.compare(0,4, "INT|") ==0)
			{
				size_t p1 = payload.find('|',4);
				size_t p2 = payload.find('|', p1 +1);
				if (p1 != std::string::npos && p2 != std::string::npos)
				{
					std::string fromId = payload.substr(4, p1 -4);
					if (fromId == _nodeId)
						continue;
					std::string matchId = payload.substr(p1 +1, p2 - (p1 +1));
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
							const PeerInfo& pr = kv.second;
							if (pr.ip == remoteIp && pr.id == fromId)
							{
								pi.name = pr.name;
								break;
							}
						}
					}
					if (_onMatchInterrupted)
						_onMatchInterrupted(pi, matchId);
// clear match state on interrupt
					clearMatch(remoteIp, ptcp, fromId, matchId, false);
				}
			}
			else if (payload.compare(0,3, "HB|") ==0)
			{
// Heartbeat: HB|fromId|matchId|
				size_t p1 = payload.find('|',3);
				size_t p2 = (p1!=std::string::npos)?payload.find('|', p1 +1):std::string::npos;
				if (p1 != std::string::npos && p2 != std::string::npos)
				{
					std::string fromId = payload.substr(3, p1 -3);
					std::string matchId = payload.substr(p1 +1, p2 - (p1 +1));
// refresh match heartbeat timestamp
					std::string key = remoteIp + ":" + std::to_string(findPeerTcpPort(remoteIp, fromId)) + ":" + fromId;
					{
						std::lock_guard<std::mutex> lk(_peersMutex);
						auto it = _matchesByKey.find(key);
						if (it != _matchesByKey.end() && it->second.matchId == matchId)
						{
							it->second.lastHbMs = nowMs();
						}
					}
				}
			}
		}
		closesock(sock);
	}

	bool LanP2PNode::tcpSendFramed(uintptr_t sock, const std::string& payload)
	{
		uint32_t n = (uint32_t)payload.size();
		uint32_t be = htonl(n);

		int off =0; int r =0;
		while (off <4) { r = send(static_cast<SOCKET>(sock), ((const char*)&be) + off,4 - off,0); if (r <=0) return false; off += r; }

		off =0;
		while (off < (int)n) { r = send(static_cast<SOCKET>(sock), payload.data() + off, (int)n - off,0); if (r <=0) return false; off += r; }
		return true;
	}

	bool LanP2PNode::tcpRecvFramed(uintptr_t sock, std::string& outPayload)
	{
		uint32_t be =0; int got =0; int r =0;
		while (got <4) { r = recv(static_cast<SOCKET>(sock), ((char*)&be) + got,4 - got,0); if (r <=0) return false; got += r; }
		uint32_t n = ntohl(be);
		outPayload.resize(n);
		int off =0;
		while (off < (int)n) { r = recv(static_cast<SOCKET>(sock), &outPayload[off], (int)n - off,0); if (r <=0) return false; off += r; }
		return true;
	}

	bool LanP2PNode::sendMatchRequest(const std::string& peerIp, uint16_t peerTcpPort, const std::string& matchId)
	{
		uintptr_t s = (uintptr_t)socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
		if ((SOCKET)s == INVALID_SOCKET) return false;
		sockaddr_in addr{}; addr.sin_family = AF_INET; addr.sin_port = htons(peerTcpPort); addr.sin_addr.s_addr = inet_addr(peerIp.c_str());
		if (connect(static_cast<SOCKET>(s), (sockaddr*)&addr, sizeof(addr)) !=0) { closesock(s); return false; }
// Try to find target id by ip:port to disambiguate multi-instance on same host
		std::string toId;
		{
			std::lock_guard<std::mutex> lk(_peersMutex);
			for (auto& kv : _peersByKey)
			{
				const PeerInfo& p = kv.second;
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
// mark match active if we know peerId
		if (!toId.empty() && ok)
			markMatchActive(peerIp, peerTcpPort, toId, matchId);
		return ok;
	}

	bool LanP2PNode::respondToMatch(const std::string& peerIp, uint16_t peerTcpPort, const std::string& matchId, bool accept)
	{
		uintptr_t s = (uintptr_t)socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
		if ((SOCKET)s == INVALID_SOCKET) return false;
		sockaddr_in addr{}; addr.sin_family = AF_INET; addr.sin_port = htons(peerTcpPort); addr.sin_addr.s_addr = inet_addr(peerIp.c_str());
		if (connect(static_cast<SOCKET>(s), (sockaddr*)&addr, sizeof(addr)) !=0) { closesock(s); return false; }
		std::ostringstream oss;
		oss << "RESP|" << _nodeId << "|" << matchId << "|" << (accept ? "1" : "0") << "|";
		bool ok = tcpSendFramed(s, oss.str());
		closesock(s);
		return ok;
	}

	bool LanP2PNode::interruptMatch(const std::string& peerIp, uint16_t peerTcpPort, const std::string& matchId)
	{
		uintptr_t s = (uintptr_t)socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
		if ((SOCKET)s == INVALID_SOCKET) return false;
		sockaddr_in addr{}; addr.sin_family = AF_INET; addr.sin_port = htons(peerTcpPort); addr.sin_addr.s_addr = inet_addr(peerIp.c_str());
		if (connect(static_cast<SOCKET>(s), (sockaddr*)&addr, sizeof(addr)) !=0) { closesock(s); return false; }
		std::ostringstream oss;
		oss << "INT|" << _nodeId << "|" << matchId << "|";
		bool ok = tcpSendFramed(s, oss.str());
		closesock(s);
		return ok;
	}

	bool LanP2PNode::sendTcpHeartbeat(const std::string& ip, uint16_t port, const std::string& matchId)
	{
		uintptr_t s = (uintptr_t)socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
		if ((SOCKET)s == INVALID_SOCKET) return false;
		sockaddr_in addr{}; addr.sin_family = AF_INET; addr.sin_port = htons(port); addr.sin_addr.s_addr = inet_addr(ip.c_str());
		if (connect(static_cast<SOCKET>(s), (sockaddr*)&addr, sizeof(addr)) !=0) { closesock(s); return false; }
		std::ostringstream oss;
		oss << "HB|" << _nodeId << "|" << matchId << "|";
		bool ok = tcpSendFramed(s, oss.str());
		closesock(s);
		return ok;
	}

	uint64_t LanP2PNode::nowMs()
	{
		using namespace std::chrono;
		return duration_cast<milliseconds>(steady_clock::now().time_since_epoch()).count();
	}

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

	void LanP2PNode::peersMaintenanceLoop()
	{
// Periodically prune stale peers and manage match heartbeats
		while (_running && _maintenanceActive)
		{
			const uint64_t now = nowMs();
//1) prune discovery-based stale peers when not in match with them
			if (_peerStaleMs >0)
			{
				std::lock_guard<std::mutex> lk(_peersMutex);
				for (auto it = _peersByKey.begin(); it != _peersByKey.end(); )
				{
// If peer is in active match (has match state), skip UDP-based pruning here
					if (_matchesByKey.find(it->first) != _matchesByKey.end())
					{
						++it;
						continue;
					}
					if ((now - it->second.lastSeenMs) > _peerStaleMs)
					{
						const PeerInfo& p = it->second;
						std::printf("[LanP2PNode][DEBUG] 因超时移除 peer (DISC维护): id=%s ip=%s port=%u lastSeenMs=%llu nowMs=%llu staleMs=%llu\n",
						            p.id.c_str(), p.ip.c_str(), (unsigned)p.tcpPort,
						            (unsigned long long)p.lastSeenMs, (unsigned long long)now, (unsigned long long)_peerStaleMs);
						it = _peersByKey.erase(it);
					}
					else ++it;
				}
			}
//2) Create snapshot for heartbeats/timeouts
			std::vector<std::tuple<std::string, std::string, uint16_t, std::string, uint64_t>> matchSnapshot;
			{
				std::lock_guard<std::mutex> lk(_peersMutex);
				for (auto& kv : _matchesByKey)
				{
					const std::string& key = kv.first;
					size_t p1 = key.find(':');
					size_t p2 = (p1==std::string::npos)?std::string::npos:key.find(':', p1+1);
					std::string ip = (p1==std::string::npos)?std::string():key.substr(0,p1);
					uint16_t port =0;
					if (p1!=std::string::npos && p2!=std::string::npos)
					{
						try
						{
							port = (uint16_t)std::stoi(key.substr(p1+1, p2-(p1+1)));
						}
						catch(...)
						{
							port=0;
						}
					}
					matchSnapshot.emplace_back(key, ip, port, kv.second.matchId, kv.second.lastHbMs);
				}
			}
//3) Act on snapshot without holding lock
			for (auto& t : matchSnapshot)
			{
				const std::string& key = std::get<0>(t);
				const std::string& ip = std::get<1>(t);
				uint16_t port = std::get<2>(t);
				const std::string& matchId = std::get<3>(t);
				uint64_t last = std::get<4>(t);
				const uint64_t now2 = nowMs();
				if (_matchHeartbeatIntervalMs>0 && (now2 - last) >= _matchHeartbeatIntervalMs)
				{
					sendTcpHeartbeat(ip, port, matchId);
// update last hb to now
					std::lock_guard<std::mutex> lk(_peersMutex);
					auto it = _matchesByKey.find(key);
					if (it != _matchesByKey.end() && it->second.matchId == matchId)
						it->second.lastHbMs = now2;
				}
				if (_matchHeartbeatTimeoutMs>0 && (now2 - last) > _matchHeartbeatTimeoutMs)
				{
// timeout -> clear match and consider peer offline (remove peer entry)
					std::string peerId;
					size_t p1 = key.find(':');
					size_t p2 = (p1==std::string::npos)?std::string::npos:key.find(':', p1+1);
					if (p2!=std::string::npos)
						peerId = key.substr(p2+1);
					std::printf("[LanP2PNode][DEBUG] 因超时移除 peer (HB 超时): id=%s ip=%s port=%u matchId=%s lastHbMs=%llu nowMs=%llu timeoutMs=%llu\n",
					            peerId.c_str(), ip.c_str(), (unsigned)port, matchId.c_str(),
					            (unsigned long long)last, (unsigned long long)now2, (unsigned long long)_matchHeartbeatTimeoutMs);
					clearMatch(ip, port, peerId, matchId, true);
				}
			}
// sleep in small chunks to be responsive to stop
			for (int i=0; i<10 && _running && _maintenanceActive; ++i)
				std::this_thread::sleep_for(std::chrono::milliseconds(500)); // ~5s cadence
		}
	}

	void LanP2PNode::markMatchActive(const std::string& ip, uint16_t tcpPort, const std::string& peerId,
	                                 const std::string& matchId)
	{
		std::lock_guard<std::mutex> lk(_peersMutex);
		std::string key = ip + ":" + std::to_string(tcpPort) + ":" + peerId;
		auto& st = _matchesByKey[key];
		st.matchId = matchId;
		st.lastHbMs = nowMs();
	}

	void LanP2PNode::clearMatch(const std::string& ip, uint16_t tcpPort, const std::string& peerId,
	                            const std::string& matchId, bool notify)
	{
		{
			std::lock_guard<std::mutex> lk(_peersMutex);
			_matchesByKey.erase(ip + ":" + std::to_string(tcpPort) + ":" + peerId);
// Also remove peer record on timeout/interruption
			for (auto it = _peersByKey.begin(); it != _peersByKey.end(); )
			{
				const PeerInfo& p = it->second;
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
