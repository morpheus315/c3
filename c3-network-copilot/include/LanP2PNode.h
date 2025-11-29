#pragma once

#include <string>
#include <thread>
#include <atomic>
#include <mutex>
#include <unordered_map>
#include <vector>
#include <functional>
#include <cstdint>

namespace lanp2p
{
	struct PeerInfo
	{
		std::string id; //玩家ID
		std::string name; //玩家名字
		std::string ip; // IP地址
		uint16_t tcpPort{0}; //TCP端口
		uint64_t lastSeenMs{0};//最后一次响应
	};

	class LanP2PNode
	{
		public:
			LanP2PNode(uint16_t discoveryPort, uint16_t tcpPort);
			~LanP2PNode();
//初始化
			void start();
			void stop();

//广播发现模块
			void startBroadcastOnly();
			void startUdpListen();
			void stopUdpListen();

// Callbacks
			void setOnPeerDiscovered(const std::function<void(const PeerInfo&)>& cb);
			void setOnMatchRequest(const std::function<void(const PeerInfo&, const std::string& matchId)>& cb);
			void setOnMatchResponse(const std::function<void(const PeerInfo&, bool accepted, const std::string& matchId)>& cb);
			void setOnMatchInterrupted(const std::function<void(const PeerInfo&, const std::string& matchId)>& cb);

//用户行为
			bool sendMatchRequest(const std::string& peerIp, uint16_t peerTcpPort, const std::string& matchId);
			bool respondToMatch(const std::string& peerIp, uint16_t peerTcpPort, const std::string& matchId, bool accept);
			bool interruptMatch(const std::string& peerIp, uint16_t peerTcpPort, const std::string& matchId);

			std::vector<PeerInfo> getPeersSnapshot();

			std::string getNodeId() const
			{
				return _nodeId;
			}
			uint16_t getTcpPort() const
			{
				return _tcpPort;
			}
			uint16_t getDiscoveryPort() const
			{
				return _discoveryPort;
			}

//设置名字
			void setNodeName(const std::string& name)
			{
				_nodeName = name;
			}
			std::string getNodeName() const
			{
				return _nodeName;
			}

//阈值设置
			void setPeerStaleMs(uint64_t ms)
			{
				_peerStaleMs = ms;
			}
			void setMatchHeartbeatIntervalMs(uint64_t ms)
			{
				_matchHeartbeatIntervalMs = ms;
			}
			void setMatchHeartbeatTimeoutMs(uint64_t ms)
			{
				_matchHeartbeatTimeoutMs = ms;
			}

		private:
			void udpBroadcastLoop();
			void udpListenLoop();
			void tcpListenLoop();
			void tcpConnectionHandler(uintptr_t sock, std::string remoteIp);
			void peersMaintenanceLoop();

			bool tcpSendFramed(uintptr_t sock, const std::string& payload);
			bool tcpRecvFramed(uintptr_t sock, std::string& outPayload);

			static uint64_t nowMs();
			static std::string randomId();

//按ip和id查找tcp端口
			uint16_t findPeerTcpPort(const std::string& ip, const std::string& id);

//在线状态检测
			void markMatchActive(const std::string& ip, uint16_t tcpPort, const std::string& peerId, const std::string& matchId);
			void clearMatch(const std::string& ip, uint16_t tcpPort, const std::string& peerId, const std::string& matchId,
			                bool notify);
			bool sendTcpHeartbeat(const std::string& ip, uint16_t port, const std::string& matchId);

		private:
//参数
			uint16_t _discoveryPort{0};
			uint16_t _tcpPort{0};
			std::string _nodeId;
			std::string _nodeName;

//模块状态
			std::atomic<bool> _running{false};
			std::atomic<bool> _broadcastActive{false};
			std::atomic<bool> _udpListenActive{false};
			std::atomic<bool> _tcpActive{false};
			std::atomic<bool> _tcpBoundReady{ false }; //标记TCP端口已绑定
			std::thread _udpBroadcaster;
			std::thread _udpListener;
			std::thread _tcpListener;

//后台维护线程
			std::atomic<bool> _maintenanceActive{false};
			std::thread _maintenanceThread;

// Callbacks
			std::function<void(const PeerInfo&)> _onPeerDiscovered;
			std::function<void(const PeerInfo&, const std::string&)> _onMatchRequest;
			std::function<void(const PeerInfo&, bool, const std::string&)> _onMatchResponse;
			std::function<void(const PeerInfo&, const std::string&)> _onMatchInterrupted;

//检测用户相关
			std::mutex _peersMutex;
			std::unordered_map<std::string, PeerInfo> _peersByKey;
			uint64_t _peerStaleMs{15000}; //udp超时检测阈值

//匹配用户相关
			struct MatchState
			{
				std::string matchId;
				uint64_t lastHbMs{0};
			};
			std::unordered_map<std::string, MatchState> _matchesByKey;
			uint64_t _matchHeartbeatIntervalMs{2000}; //tcp心跳发送间隔
			uint64_t _matchHeartbeatTimeoutMs{7000}; //tcp超时检测阈值
	};
}
