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
	// 对端信息（用于发现与对战）
	struct PeerInfo
	{
		std::string id;        // 节点唯一ID（16位十六进制字符串）
		std::string name;      // 节点显示名（可为空）
		std::string ip;        // 节点IP地址
		uint16_t tcpPort{0};   // 节点TCP监听端口
		uint64_t lastSeenMs{0};// 最近一次被发现/心跳到达的时间戳（毫秒）
	};

	// 局域网P2P节点：负责UDP发现、TCP控制与对战消息收发
	class LanP2PNode
	{
		public:
			LanP2PNode(uint16_t discoveryPort, uint16_t tcpPort);
			~LanP2PNode();
			// 启动全部功能（UDP广播/监听、TCP监听、维护线程）
			void start();
			// 停止全部功能并回收线程
			void stop();

			// 仅启动广播与TCP监听（不启动UDP监听）
			void startBroadcastOnly();
			// 启动UDP发现监听
			void startUdpListen();
			// 停止UDP发现监听
			void stopUdpListen();

			// 设置回调：发现对端/收到匹配请求/响应/中断/落子
			void setOnPeerDiscovered(const std::function<void(const PeerInfo &)> &cb);
			void setOnMatchRequest(const std::function<void(const PeerInfo &, const std::string &matchId)> &cb);
			void setOnMatchResponse(const std::function<void(const PeerInfo &, bool accepted, const std::string &matchId)> &cb);
			void setOnMatchInterrupted(const std::function<void(const PeerInfo &, const std::string &matchId)> &cb);
			void setOnGameMove(const std::function<void(const PeerInfo &, int x, int y, int z)> &cb);

			// 业务操作：发起/响应/中断匹配；发送落子
			bool sendMatchRequest(const std::string &peerIp, uint16_t peerTcpPort, const std::string &matchId);
			bool respondToMatch(const std::string &peerIp, uint16_t peerTcpPort, const std::string &matchId, bool accept);
			bool interruptMatch(const std::string &peerIp, uint16_t peerTcpPort, const std::string &matchId);
			bool sendGameMove(const std::string &peerIp, uint16_t peerTcpPort, int x, int y, int z);

			// 获取当前可用对端的快照（会剔除超时的项）
			std::vector<PeerInfo> getPeersSnapshot();

			// 基本属性读取
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

			// 节点显示名设置/读取
			void setNodeName(const std::string &name)
			{
				_nodeName = name;
			}
			std::string getNodeName() const
			{
				return _nodeName;
			}

			// 超时/心跳相关参数设置
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

			// 发送失败最大重试次数设置与读取
			void setMaxSendRetries(int r)
			{
				if (r > 0)
					_maxSendRetries = r;
			}
			int getMaxSendRetries() const
			{
				return _maxSendRetries;
			}

			// 在匹配表中标记对战活跃（用于心跳检测）
			void markMatchActive(const std::string &ip, uint16_t tcpPort, const std::string &peerId, const std::string &matchId);

			// 生成随机匹配ID（16位十六进制字符串）
			static std::string generateMatchId();

		private:
			// 线程函数：UDP广播/监听、TCP监听、TCP连接处理、维护线程
			void udpBroadcastLoop();
			void udpListenLoop();
			void tcpListenLoop();
			void tcpConnectionHandler(uintptr_t sock, std::string remoteIp);
			void peersMaintenanceLoop();

			// TCP有帧边界的发送/接收（前置4字节网络序长度）
			bool tcpSendFramed(uintptr_t sock, const std::string &payload);
			bool tcpRecvFramed(uintptr_t sock, std::string &outPayload);

			// 工具方法：时间戳与随机ID
			static uint64_t nowMs();
			static std::string randomId();

			// 根据 ip+id 查找对端TCP端口（辅助解析回调参数）
			uint16_t findPeerTcpPort(const std::string &ip, const std::string &id);

			// 对战状态管理：记录/清理匹配及其心跳
			void clearMatch(const std::string &ip, uint16_t tcpPort, const std::string &peerId, const std::string &matchId,
			                bool notify);
			bool sendTcpHeartbeat(const std::string &ip, uint16_t port, const std::string &matchId);

		private:
			// 端口与自身标识
			uint16_t _discoveryPort{0};
			uint16_t _tcpPort{0};
			std::string _nodeId;
			std::string _nodeName;

			// 模块运行状态与线程
			std::atomic<bool> _running{false};
			std::atomic<bool> _broadcastActive{false};
			std::atomic<bool> _udpListenActive{false};
			std::atomic<bool> _tcpActive{false};
			std::atomic<bool> _tcpBoundReady{ false }; // TCP端口已绑定
			std::thread _udpBroadcaster;
			std::thread _udpListener;
			std::thread _tcpListener;

			// 后台维护线程（对端过期与匹配心跳）
			std::atomic<bool> _maintenanceActive{false};
			std::thread _maintenanceThread;

			// 事件回调
			std::function<void(const PeerInfo &)> _onPeerDiscovered;
			std::function<void(const PeerInfo &, const std::string &)> _onMatchRequest;
			std::function<void(const PeerInfo &, bool, const std::string &)> _onMatchResponse;
			std::function<void(const PeerInfo &, const std::string &)> _onMatchInterrupted;
			std::function<void(const PeerInfo &, int x, int y, int z)> _onGameMove;

			// 对端与匹配状态
			std::mutex _peersMutex;
			std::unordered_map<std::string, PeerInfo> _peersByKey; // key: ip:port:id
			uint64_t _peerStaleMs{15000}; // 对端超时阈值（基于发现）

			// 匹配状态（用于心跳维护）
			struct MatchState
			{
				std::string matchId;   // 当前匹配ID
				uint64_t lastHbMs{0};  // 最近心跳时间（毫秒）
			};
			std::unordered_map<std::string, MatchState> _matchesByKey; // key: ip:port:id
			uint64_t _matchHeartbeatIntervalMs{2000}; // 心跳发送间隔
			uint64_t _matchHeartbeatTimeoutMs{7000};  // 心跳超时阈值

			// 发送失败时的最大重试次数
			int _maxSendRetries{3};
	};
}
