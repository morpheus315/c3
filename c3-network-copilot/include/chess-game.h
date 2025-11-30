#pragma once

int place(int x, int y, int z, int BoardSize);
bool OnlineInitChessBoard(char** pChessBoard, int BoardSize);
bool NativeGetChessPosition(int input[]);
bool UpdateBoardState(int BoardSize, char* ChessBoard, int input[], char player);
int CheckWin(int BoardSize, char* ChessBoard, int input[], char player);