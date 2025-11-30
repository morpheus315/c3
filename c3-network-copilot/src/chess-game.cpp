#define NOMINMAX
#include <windows.h>
#include <cstdio>
#include <iostream>
#include <cmath>
#include <tchar.h>
#include <limits>
#include "../include/chess-game.h"
//棋盘位置逻辑：右手系xyz，先沿x再沿y再沿z，比如BoardSize=5，那*(ChessBoard+17)对应的棋子位置就是（2，4，1），计算公式：17=（2-1）+（4-1）* 5 +（1-1）* 25
using namespace std;


//根据坐标输出棋子存储位置
int place(int x, int y, int z, int BoardSize)
{
	return (x - 1) + (y - 1) * BoardSize + (z - 1) * BoardSize * BoardSize;
}


//单机游戏获取棋盘大小及分配内存
int NativeGetChessSize(int* pBoardSize,char** pChessBoard)
{
	while (TRUE)
	{
		bool choice2 = 1;
		cout << "How much the size do you want?(from 10 to 30)";
		if (!(cin >> *pBoardSize))
		{
			cin.clear();
			cin.ignore(numeric_limits<streamsize>::max(), '\n');
			cout << "Please input a number!" << endl;
			continue;
		}
		if (*pBoardSize < 10 || *pBoardSize > 30)
		{
			cout << "The number is out of the limits!" << endl;
			continue;
		}
		size_t n = (size_t)*pBoardSize * (size_t)*pBoardSize * (size_t)*pBoardSize;
		*pChessBoard = (char*)calloc(n, sizeof(char));
		if (*pChessBoard == NULL)
		{
			cout << "ERROR allocating! Input 1 to try again, 0 to exit:";
			while (!(cin >> choice2))
			{
				cin.clear();
				cin.ignore(numeric_limits<streamsize>::max(), '\n');
				cout << "Input 1 to try again, 0 to exit:";
			}
			if (!choice2)
			{
				cout << "See you next time!" << endl;
				return 0;
			}
		}
		break;
	}
	return 1;
}

//输入棋子位置
void NativeGetChessPosition(int input[])
{
	cout << "Enter the location where you want to place your order. Use the form like x y z." << endl;
	while (TRUE)
	{
		if (!(cin >> input[0] >> input[1] >> input[2]))
		{
			cin.clear();
			cin.ignore(numeric_limits<streamsize>::max(), '\n');
			cout << "Input again." << endl;
			continue;
		}
		break;
	}
}



//新函数：接受棋局描述，检查位置合法性，如果合法则更新棋局，否则输出非法标记
bool UpdateBoardState(int BoardSize, char* ChessBoard, int input[], char player)
{
	// 检查坐标是否在棋盘范围内
	if (input[0] < 1 || input[0] > BoardSize || input[1] < 1 || input[1] > BoardSize || input[2] < 1 || input[2] > BoardSize)
	{
		cout << "INVALID_MOVE: Position is out of the board range." << endl; // 输出非法标记
		return false;
	}

	// 计算棋子在一维数组中的索引
	int newChessIndex = place(input[0], input[1], input[2], BoardSize);

	// 检查目标位置是否已有棋子
	if (ChessBoard[newChessIndex] != 0)
	{
		cout << "INVALID_MOVE: A chess piece already exists at this position." << endl; // 输出非法标记
		return false;
	}

	// 位置合法，更新棋盘状态
	ChessBoard[newChessIndex] = player;
	cout << "MOVE_ACCEPTED: Board updated. Player " << player << " placed a piece at (" << input[0] << ", " << input[1] << ", " << input[2] << ")." << endl;
	
	// 在此可以调用一个函数来打印整个棋盘状态，如果需要的话。
	// 例如: PrintBoard(BoardSize, ChessBoard);

	return true;
}

//以落子点为中心检测9*9*9的空间内是否有连着的5个棋子，有输出谁赢了以及怎么赢的并返回1，无返回0，各部分检测用花括号括起来以减少占用
int CheckWin(int BoardSize, char* ChessBoard,int input[],char player)
{
	int x = input[0], y = input[1], z = input[2];
	int xmax = (x + 4 <= BoardSize) ? (x + 4) : BoardSize;
	int xmin = (x - 4 >= 1) ? (x - 4) : 1;
	int ymax = (y + 4 <= BoardSize) ? (y + 4) : BoardSize;
	int ymin = (y - 4 >= 1) ? (y - 4) : 1;
	int zmax = (z + 4 <= BoardSize) ? (z + 4) : BoardSize;
	int zmin = (z - 4 >= 1) ? (z - 4) : 1;
	//检测x方向
	for (int i = xmin; i + 4 <= xmax; i++)
	{
		if (*(ChessBoard + place(i, y, z, BoardSize)) == player &&
			*(ChessBoard + place(i + 1, y, z, BoardSize)) == player &&
			*(ChessBoard + place(i + 2, y, z, BoardSize)) == player &&
			*(ChessBoard + place(i + 3, y, z, BoardSize)) == player &&
			*(ChessBoard + place(i + 4, y, z, BoardSize)) == player)
		{
			printf("The player%c wins! In the x-axis direction\n", player);
			return 1;
		}
	}
	//检测y方向
	for (int j = ymin; j + 4 <= ymax; j++)
	{
		if (*(ChessBoard + place(x, j, z, BoardSize)) == player &&
			*(ChessBoard + place(x, j + 1, z, BoardSize)) == player &&
			*(ChessBoard + place(x, j + 2, z, BoardSize)) == player &&
			*(ChessBoard + place(x, j + 3, z, BoardSize)) == player &&
			*(ChessBoard + place(x, j + 4, z, BoardSize)) == player)
		{
			printf("The player%c wins! In the y-axis direction\n", player);
			return 1;
		}
	}
	//检测z方向
	for (int k = zmin; k + 4 <= zmax; k++)
	{
		if (*(ChessBoard + place(x, y, k, BoardSize)) == player &&
			*(ChessBoard + place(x, y, k + 1, BoardSize)) == player &&
			*(ChessBoard + place(x, y, k + 2, BoardSize)) == player &&
			*(ChessBoard + place(x, y, k + 3, BoardSize)) == player &&
			*(ChessBoard + place(x, y, k + 4, BoardSize)) == player)
		{
			printf("The player%c wins! In the z-axis direction\n", player);
			return 1;
		}
	}
	//检测xy对角线方向
	for (int i = xmin, j = ymin; i + 4 <= xmax && j + 4 <= ymax; i++, j++)
	{
		if (*(ChessBoard + place(i, j, z, BoardSize)) == player &&
			*(ChessBoard + place(i + 1, j + 1, z, BoardSize)) == player &&
			*(ChessBoard + place(i + 2, j + 2, z, BoardSize)) == player &&
			*(ChessBoard + place(i + 3, j + 3, z, BoardSize)) == player &&
			*(ChessBoard + place(i + 4, j + 4, z, BoardSize)) == player)
		{
			printf("The player%c wins! In the xy-diagonal direction\n", player);
			return 1;
		}
	}
	for (int i = xmax, j = ymin; i - 4 >= xmin && j + 4 <= ymax; i--, j++)
	{
		if (*(ChessBoard + place(i, j, z, BoardSize)) == player &&
			*(ChessBoard + place(i - 1, j + 1, z, BoardSize)) == player &&
			*(ChessBoard + place(i - 2, j + 2, z, BoardSize)) == player &&
			*(ChessBoard + place(i - 3, j + 3, z, BoardSize)) == player &&
			*(ChessBoard + place(i - 4, j + 4, z, BoardSize)) == player)
		{
			printf("The player%c wins! In the xy-diagonal direction\n", player);
			return 1;
		}
	}
	//检测yz对角线方向
	for (int j = ymin, k = zmin; j + 4 <= ymax && k + 4 <= zmax; j++, k++)
	{
		if (*(ChessBoard + place(x, j, k, BoardSize)) == player &&
			*(ChessBoard + place(x, j + 1, k + 1, BoardSize)) == player &&
			*(ChessBoard + place(x, j + 2, k + 2, BoardSize)) == player &&
			*(ChessBoard + place(x, j + 3, k + 3, BoardSize)) == player &&
			*(ChessBoard + place(x, j + 4, k + 4, BoardSize)) == player)
		{
			printf("The player%c wins! In the yz-diagonal direction\n", player);
			return 1;
		}
	}
	for (int j = ymax, k = zmin; j - 4 >= ymin && k + 4 <= zmax; j--, k++)
	{
		if (*(ChessBoard + place(x, j, k, BoardSize)) == player &&
			*(ChessBoard + place(x, j - 1, k + 1, BoardSize)) == player &&
			*(ChessBoard + place(x, j - 2, k + 2, BoardSize)) == player &&
			*(ChessBoard + place(x, j - 3, k + 3, BoardSize)) == player &&
			*(ChessBoard + place(x, j - 4, k + 4, BoardSize)) == player)
		{
			printf("The player%c wins! In the yz-diagonal direction\n", player);
			return 1;
		}
	}
	//检测xz对角线方向
	for (int i = xmin, k = zmin; i + 4 <= xmax && k + 4 <= zmax; i++, k++)
	{
		if (*(ChessBoard + place(i, y, k, BoardSize)) == player &&
			*(ChessBoard + place(i + 1, y, k + 1, BoardSize)) == player &&
			*(ChessBoard + place(i + 2, y, k + 2, BoardSize)) == player &&
			*(ChessBoard + place(i + 3, y, k + 3, BoardSize)) == player &&
			*(ChessBoard + place(i + 4, y, k + 4, BoardSize)) == player)
		{
			printf("The player%c wins! In the xz-diagonal direction\n", player);
			return 1;
		}
	}
	for (int i = xmax, k = zmin; i - 4 >= xmin && k + 4 <= zmax; i--, k++)
	{
		if (*(ChessBoard + place(i, y, k, BoardSize)) == player &&
			*(ChessBoard + place(i - 1, y, k + 1, BoardSize)) == player &&
			*(ChessBoard + place(i - 2, y, k + 2, BoardSize)) == player &&
			*(ChessBoard + place(i - 3, y, k + 3, BoardSize)) == player &&
			*(ChessBoard + place(i - 4, y, k + 4, BoardSize)) == player)
		{
			printf("The player%c wins! In the xz-diagonal direction\n", player);
			return 1;
		}
	}
	//检测空间对角线方向
	for (int i = xmin, j = ymin, k = zmin; i + 4 <= xmax && j + 4 <= ymax && k + 4 <= zmax; i++, j++, k++)
	{
		if (*(ChessBoard + place(i, j, k, BoardSize)) == player &&
			*(ChessBoard + place(i + 1, j + 1, k + 1, BoardSize)) == player &&
			*(ChessBoard + place(i + 2, j + 2, k + 2, BoardSize)) == player &&
			*(ChessBoard + place(i + 3, j + 3, k + 3, BoardSize)) == player &&
			*(ChessBoard + place(i + 4, j + 4, k + 4, BoardSize)) == player)
		{
			printf("The player%c wins! In the space-diagonal direction\n", player);
			return 1;
		}
	}
	for (int i = xmax, j = ymin, k = zmin; i - 4 >= xmin && j + 4 <= ymax && k + 4 <= zmax; i--, j++, k++)
	{
		if (*(ChessBoard + place(i, j, k, BoardSize)) == player &&
			*(ChessBoard + place(i - 1, j + 1, k + 1, BoardSize)) == player &&
			*(ChessBoard + place(i - 2, j + 2, k + 2, BoardSize)) == player &&
			*(ChessBoard + place(i - 3, j + 3, k + 3, BoardSize)) == player &&
			*(ChessBoard + place(i - 4, j + 4, k + 4, BoardSize)) == player)
		{
			printf("The player%c wins! In the space-diagonal direction\n", player);
			return 1;
		}
	}
	for (int i = xmin, j = ymax, k = zmin; i + 4 <= xmax && j - 4 >= ymin && k + 4 <= zmax; i++, j--, k++)
	{
		if (*(ChessBoard + place(i, j, k, BoardSize)) == player &&
			*(ChessBoard + place(i + 1, j - 1, k + 1, BoardSize)) == player &&
			*(ChessBoard + place(i + 2, j - 2, k + 2, BoardSize)) == player &&
			*(ChessBoard + place(i + 3, j - 3, k + 3, BoardSize)) == player &&
			*(ChessBoard + place(i + 4, j - 4, k + 4, BoardSize)) == player)
		{
			printf("The player%c wins! In the space-diagonal direction\n", player);
			return 1;
		}
	}
	for (int i = xmax, j = ymax, k = zmin; i - 4 >= xmin && j - 4 >= ymin && k + 4 <= zmax; i--, j--, k++)
	{
		if (*(ChessBoard + place(i, j, k, BoardSize)) == player &&
			*(ChessBoard + place(i - 1, j - 1, k + 1, BoardSize)) == player &&
			*(ChessBoard + place(i - 2, j - 2, k + 2, BoardSize)) == player &&
			*(ChessBoard + place(i - 3, j - 3, k + 3, BoardSize)) == player &&
			*(ChessBoard + place(i - 4, j - 4, k + 4, BoardSize)) == player)
		{
			printf("The player%c wins! In the space-diagonal direction\n", player);
			return 1;
		}
	}
	return 0;
}
/*
//单机游戏主程序
int NativeChessPlaying(int BoardSize, char* ChessBoard)
{
	if (!NativeGetChessSize(&BoardSize, &ChessBoard))
	{
		return 0;
	}
	char player1 = '1', player2 = '2';
	int input1[3] = { 0 }, input2[3] = { 0 };
	while (TRUE)
	{
		cout << "The turn of player1" << endl;
		do
		{
			NativeGetChessPosition(input1);
		} while (!CheckLegal(BoardSize, ChessBoard, input1, player1));
		Display3DModel(BoardSize, ChessBoard);
		if (CheckWin(BoardSize, ChessBoard, input1, player1))
		{
			break;
		}
		cout << "The turn of player2" << endl;
		do
		{
			NativeGetChessPosition(input2);
		} while (!CheckLegal(BoardSize, ChessBoard, input2, player2));
		Display3DModel(BoardSize, ChessBoard);
		if (CheckWin(BoardSize, ChessBoard, input2, player2))
		{
			break;
		}
	}
	system("pause");
	free(ChessBoard);
	return 1;
}*/
/*
int main()
{
	while (TRUE)
	{
		char* ChessBoard = NULL;
		int BoardSize = 0;
		system("cls");
		cout << "Welcome to this game!" << endl;
		cout << "Which game mode do you want, online battle(input 1) or native battle(input 2)?(Input 0 to exit)";
		int choice = 0;
		while (!(cin >> choice) || (choice != 0 && choice != 1 && choice != 2))
		{
			cin.clear();
			cin.ignore(numeric_limits<streamsize>::max(), '\n');
			cout << "Please input 0,1 or 2! Input again:";
		}
		if (choice == 0)//退出
		{
			cout << "See you next time!";
			return 0;
		}
		if (choice == 1)//联机
		{
			//待实现
		}
		if (choice == 2)//单机
		{
			if (!NativeChessPlaying(BoardSize, ChessBoard))
			{
				return 0;
			}
		}
	}
	return 0;
}*/