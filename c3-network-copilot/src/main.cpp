#include "../include/LanP2PNode.h"
#include "../include/GameClient.h"
#include <iostream>
#include <string>

// 简易控制台菜单
static void printMenu()
{
    std::cout << "\n===== 3D 五子棋 =====\n";
    std::cout << "1. 开始发现（监听10秒）\n";
    std::cout << "2. 列出可用对端\n";
    std::cout << "3. 向选定对端发起匹配\n";
    std::cout << "4. 处理待处理的来访请求\n";
    std::cout << "5. 查看当前对局信息\n";
    std::cout << "6. 结束当前对局\n";
    std::cout << "7. 退出\n";
    std::cout << "请选择: ";
}

int main()
{
    using namespace lanp2p;

    // 创建节点（UDP发现端口37000，TCP随机端口），启动广播和TCP监听
    LanP2PNode node(37000, 0);
    node.setPeerStaleMs(15000);
    node.startBroadcastOnly();

    // 创建客户端，负责回调接入与游戏流程
    Client client(node);

    // 设置显示名（可选）
    std::cout << "请输入你的显示名（可留空）: ";
    std::string name;
    std::getline(std::cin, name);
    if (!name.empty()) node.setNodeName(name);

    std::cout << "初始化完成。你的ID: " << node.getNodeId()
              << ", TCP端口: " << node.getTcpPort()
              << ", 发现端口: " << node.getDiscoveryPort() << std::endl;

    bool running = true;
    while (running)
    {
        // 若已进入对局，则直接进入游戏循环（阻塞至结束）
        if (client.isInMatch())
        {
            client.startGame();
            continue;
        }

        printMenu();
        std::string line;
        if (!std::getline(std::cin, line)) break;
        int choice = 0;
        try { choice = std::stoi(line); } catch (...) { std::cout << "输入不合法。" << std::endl; continue; }

        switch (choice)
        {
            case 1:
                client.startDiscovery();
                break;
            case 2:
            {
                auto peers = client.getAvailablePeers();
                if (peers.empty()) { std::cout << "暂无可用对端。" << std::endl; break; }
                std::cout << "可用对端：" << std::endl;
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
                if (peers.empty()) { std::cout << "没有可发起匹配的对端。" << std::endl; break; }
                std::cout << "请选择对端序号: ";
                std::string sel; std::getline(std::cin, sel);
                int idx = 0; try { idx = std::stoi(sel); } catch (...) { std::cout << "输入不合法。" << std::endl; break; }
                if (idx < 1 || idx > (int)peers.size()) { std::cout << "序号越界。" << std::endl; break; }
                client.requestMatch(peers[(size_t)idx-1]);
                break;
            }
            case 4:
                client.handlePendingRequests();
                break;
            case 5:
            {
                if (!client.isInMatch()) { std::cout << "当前不在对局中。" << std::endl; break; }
                auto peer = client.getMatchPeer();
                std::cout << "对局ID: " << client.getMatchId() << ", 对手: "
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
                std::cout << "输入不合法。" << std::endl;
        }
    }

    // 退出时停止节点
    node.stop();
    return 0;
}
