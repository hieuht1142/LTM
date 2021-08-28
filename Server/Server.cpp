#include "stdafx.h"
#include <stdio.h>
#include <string>
#include <iostream>
#include <fstream>
#include <WinSock2.h>
#include <WS2tcpip.h>
#include <direct.h>
#include <vector>
#include <time.h>
#include <set>

#define SERVER_ADDR "127.0.0.1"
#define FILE_NAME_SIZE 256
#define BLOCK_SIZE 65535
#define BUFF_SIZE 65542
#pragma comment(lib, "Ws2_32.lib")

using namespace std;

typedef struct Client
{
	SOCKET socket;
	char ip[INET_ADDRSTRLEN];
	int port;
	char fileName[FILE_NAME_SIZE]; // ten file tim kiem
	int currNumClient; // so luong client dang online tai thoi diem hien tai
	int returnNumClient; // so luong client da tra ve ket qua tim kiem
	set<SOCKET> clientHaveFile;
	bool isSearching; // client dang thuc hien tim kiem hay ko
	SOCKET downloadClient; // client duoc lua chon de download file
} Client;

Client clients[WSA_MAXIMUM_WAIT_EVENTS];
SOCKET listenSock;
u_short server_port;
DWORD nEvents = 0;
SOCKET socks[WSA_MAXIMUM_WAIT_EVENTS];
WSAEVENT events[WSA_MAXIMUM_WAIT_EVENTS];

void initWinsock();
void constructSocket(SOCKET *s);
void bindAddr(sockaddr_in *sa, char *addr, u_short port, SOCKET s);
int sendMsg(SOCKET socket, char *message, int len);
void handleMsg(Client &client, char *message, int len);
void recvMsg(Client &client, int n);
void disconnect(int index);
void removeClient(int index);
void notifyDownloadingClient(int index);
void sendACK(SOCKET s);

int main(int argc, char *argv[])
{
	if (argc != 2) {
		printf("Execution error.");
		return 0;
	}

	server_port = (u_short)atoi(argv[1]);

	DWORD index;
	WSANETWORKEVENTS sockEvent;

	initWinsock();
	constructSocket(&listenSock);

	// Bind address to server socket
	sockaddr_in serverAddr;
	bindAddr(&serverAddr, SERVER_ADDR, server_port, listenSock);

	socks[0] = listenSock;
	events[0] = WSACreateEvent();
	nEvents++;
	WSAEventSelect(socks[0], events[0], FD_ACCEPT | FD_CLOSE);

	if (listen(listenSock, 10)) {
		printf("Error %d: Cannot place server socket in state LISTEN.", WSAGetLastError());
		return 0;
	}

	printf("Server started!\n");
	printf("Waiting for connections...\n");

	// Communicate with client
	sockaddr_in clientAddr;
	char clientIP[INET_ADDRSTRLEN];
	SOCKET connSock;
	int clientAddrLen = sizeof(clientAddr), clientPort;

	for (int i = 1; i < WSA_MAXIMUM_WAIT_EVENTS; i++) {
		socks[i] = 0;
	}

	while (1) {
		index = WSAWaitForMultipleEvents(nEvents, events, FALSE, WSA_INFINITE, FALSE);
		if (index == WSA_WAIT_FAILED) {
			printf("Error %d: WSAWaitForMultipleEvents() failed\n", WSAGetLastError());
			continue;
		}
		index = index - WSA_WAIT_EVENT_0;
		WSAEnumNetworkEvents(socks[index], events[index], &sockEvent);
		if (sockEvent.lNetworkEvents & FD_ACCEPT) {
			if (sockEvent.iErrorCode[FD_ACCEPT_BIT] != 0) {
				printf("Error %d: FD_ACCEPT failed with error.\n", sockEvent.iErrorCode[FD_READ_BIT]);
				WSAResetEvent(events[index]);
				continue;
			}
			connSock = accept(socks[index], (sockaddr *)&clientAddr, &clientAddrLen);
			if (connSock == SOCKET_ERROR) {
				printf("Error %d: Cannot permit incoming connection.\n", WSAGetLastError());
			}
			else {
				if (nEvents == WSA_MAXIMUM_WAIT_EVENTS) {
					printf("Too many clients.\n");
					closesocket(connSock);
				}
				else {
					inet_ntop(AF_INET, &clientAddr.sin_addr, clientIP, sizeof(clientIP));
					clientPort = ntohs(clientAddr.sin_port);
					printf("Accept incoming connection from [%s:%d:%d]\n\n", clientIP, clientPort, connSock);
					socks[nEvents] = connSock;
					events[nEvents] = WSACreateEvent();

					clients[nEvents].socket = connSock;
					strcpy_s(clients[nEvents].ip, clientIP);
					clients[nEvents].port = clientPort;
					clients[nEvents].isSearching = false;
					clients[nEvents].downloadClient = 0;

					cout << "Socket " << clients[nEvents].socket << " connected" << endl;
					WSAEventSelect(clients[nEvents].socket, events[nEvents], FD_READ | FD_CLOSE);
					nEvents++;
				}
			}
			WSAResetEvent(events[index]);
		}

		if (sockEvent.lNetworkEvents & FD_READ) {
			//Receive message from client
			if (sockEvent.iErrorCode[FD_READ_BIT] != 0) {
				printf("Error %d: FD_READ failed with error.\n", sockEvent.iErrorCode[FD_READ_BIT]);
			}
			else {
				recvMsg(clients[index], index);
			}
			WSAResetEvent(events[index]);
		}

		if (sockEvent.lNetworkEvents & FD_CLOSE) {
			if (sockEvent.iErrorCode[FD_CLOSE_BIT] != 0 && sockEvent.iErrorCode[FD_CLOSE_BIT] != 10053) {
				printf("Error %d: FD_CLOSE failed with error.\n", sockEvent.iErrorCode[FD_CLOSE_BIT]);
			}
			else {
				disconnect(index);
			}
			WSAResetEvent(events[index]);
		}
	}

	// Close socket
	closesocket(listenSock);

	// Terminate winsock
	WSACleanup();

	return 0;
}

/*
Initiate winsock
*/
void initWinsock()
{
	WSAData wsaData;
	WORD wVersion = MAKEWORD(2, 2);
	if (WSAStartup(wVersion, &wsaData)) {
		printf("Winsock 2.2 is not supported!");
		exit(0);
	}
}

void constructSocket(SOCKET *s)
{
	*s = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
	if (*s == INVALID_SOCKET) {
		printf("Error %d: Cannot create socket.", WSAGetLastError());
		exit(0);
	}
}

void bindAddr(sockaddr_in *sa, char *addr, u_short port, SOCKET s)
{
	(*sa).sin_family = AF_INET;
	(*sa).sin_port = htons(port);
	inet_pton(AF_INET, addr, &((*sa).sin_addr));

	if (bind(s, (sockaddr *)sa, sizeof(*sa))) {
		printf("Error %d: Cannot associate a local address with server socket.", WSAGetLastError());
		exit(0);
	}
}

/*
len: length of message

@return 1 if send message successful, else return 0
*/
int sendMsg(SOCKET socket, char *message, int len)
{
	int ret, index, nLeft;
	nLeft = len;
	index = 0;

	while (nLeft > 0) {
		ret = send(socket, message + index, nLeft, 0);
		if (ret == SOCKET_ERROR) {
			return -1;
		}
		nLeft -= ret;
		index += ret;
	}
	return 0;
}

/*
Gui yeu cau tim kiem file toi cac client dang online

message: message client gui den
len: length of message
*/
void handleSearchFile(Client &client, char *message, int len) {

	int ret;

	// Gui lai message dang xu ly tim kiem: opcode = '5' va payload = '11'
	char buff[BUFF_SIZE];
	buff[0] = '5';
	buff[7] = '1';
	buff[8] = '1';
	unsigned short payloadLen = 2;
	memcpy(buff + 5, &payloadLen, 2);
	ret = sendMsg(client.socket, buff, 9);

	// Xu ly tim kiem file
	// Gui yeu cau tim kiem file toi cac client dang online

	// Chi co 1 client dang online
	if (nEvents == 2) {
		// Gui message bao loi tim kiem file: opcode = '5' va payload = '10'
		buff[0] = '5';
		buff[7] = '1';
		buff[8] = '0';
		unsigned short payloadLen = 2;
		memcpy(buff + 5, &payloadLen, 2);
		sendMsg(client.socket, buff, 9);
		return;
	}

	// Co client khac dang online

	client.isSearching = true; // dat trang thai dang tim kiem
	client.currNumClient = nEvents - 1; // so client dang online
	client.returnNumClient = 0; // so client da tra ve ket qua tim kiem ban dau bang 0
	memcpy(client.fileName, message + 7, len - 7); // gan ten file dang tim kiem cho client
	client.fileName[len - 7] = 0; // gan ky tu ket thuc xau

								  // Tao message yeu cau tim kiem: Opcode = '1', id = socket cua client, payload = ten files
	buff[0] = '1';
	memcpy(buff + 7, message + 7, len - 7);
	unsigned short fileNameLen = len - 7;
	memcpy(buff + 5, &fileNameLen, 2);
	memcpy(buff + 1, &(client.socket), 4);

	for (int i = 1; i < nEvents; i++) {
		if (socks[i] != client.socket) {
			ret = sendMsg(socks[i], buff, 7 + fileNameLen);
		}
	}
}

void handleDowloadFile(Client &client, char *message, int len) {

	char buff[BUFF_SIZE];
	SOCKET desClientSock; // socket cua client duoc yeu cau download file
	char desClientSockStr[10];
	int ret;

	memcpy(desClientSockStr, message + 7, len - 7);
	desClientSockStr[len - 7] = 0;
	desClientSock = (SOCKET)atoi(desClientSockStr);

	client.downloadClient = desClientSock;

	// Gui yeu cau download file den desClientSock
	// Thong diep voi opcode = '2', id = client.sock, payload = ten file
	buff[0] = '2';
	memcpy(buff + 1, &client.socket, 4);

	unsigned short fileNameLen = strlen(client.fileName);
	memcpy(buff + 5, &fileNameLen, 2);
	memcpy(buff + 7, client.fileName, fileNameLen);

	ret = sendMsg(desClientSock, buff, 7 + fileNameLen);
	if (ret == -1) {
		// Phan hoi lai client yeu cau download file rang khong the download: opcode = '5' va payload = '20'
		buff[0] = '5';
		buff[7] = '2';
		buff[8] = '0';
		unsigned short payloadLen = 2;
		memcpy(buff + 5, &payloadLen, 2);
		sendMsg(client.socket, buff, 9);
	}
	else {
		// Phan hoi lai client yeu cau download file message dang xu ly download: opcode = '5' va payload = '21'
		buff[0] = '5';
		buff[7] = '2';
		buff[8] = '1';
		unsigned short payloadLen = 2;
		memcpy(buff + 5, &payloadLen, 2);
		sendMsg(client.socket, buff, 9);
	}
}

/*
Xu ly ket qua tim kiem file tu cac client gui ve
len: length of message
*/
void handleSearchFileResult(Client &client, char *message, int len)
{
	int index; // index cua client gui yeu cau tim kiem file
	SOCKET desClientSocket;

	memcpy(&desClientSocket, message + 1, 4);
	for (int i = 1; i < nEvents; i++) {
		if (desClientSocket == socks[i]) {
			index = i;
			break;
		}
	}

	clients[index].returnNumClient += 1; // tang so luong client tra ve ket qua tim kiem them 1

	if (message[7] == '1') { // Co file
		clients[index].clientHaveFile.insert(client.socket);
	}

	// Tat ca client duoc yeu cau da tra ve ket qua tim kiem
	if (clients[index].currNumClient - 1 == clients[index].returnNumClient) {
		char buff[BUFF_SIZE];

		if (clients[index].clientHaveFile.empty()) { // Khong co client nao co file
													 // Gui message bao loi tim kiem file: opcode = '3' va payload = rong
			buff[0] = '3';
			unsigned short payloadLen = 0;
			memcpy(buff + 5, &payloadLen, 2);
			sendMsg(desClientSocket, buff, 7);
		}
		else {
			// Gui message voi opcode = 3, payload chua danh sach cac client co file, ngan cach nhau boi dau '-'
			buff[0] = '3';

			char list[1024];
			strcpy_s(list, "");
			for (auto sock : clients[index].clientHaveFile) {
				strcat_s(list, to_string(sock).c_str());
				strcat_s(list, "-");
			}

			unsigned short payloadLen = strlen(list) - 1;
			list[payloadLen] = 0; // Xoa ky tu '-' o cuoi cung

			memcpy(buff + 7, list, payloadLen);
			memcpy(buff + 5, &payloadLen, 2);

			sendMsg(desClientSocket, buff, 7 + payloadLen);
		}

		clients[index].isSearching = false;
		clients[index].currNumClient = 0;
		clients[index].returnNumClient = 0;
		clients[index].clientHaveFile.clear();
	}
}

void handleMsg(Client &client, char *message, int len)
{
	char opcode = message[0];

	switch (opcode)
	{
	case '1':
		handleSearchFile(client, message, len);
		break;

	case '2':
		handleDowloadFile(client, message, len);
		break;

	case '3':
		handleSearchFileResult(client, message, len);
		sendACK(client.socket);
		break;

	case '4':
		unsigned int desClientSock;
		memcpy(&desClientSock, message + 1, 4); // Tinh socket cua client yeu cau download file tu ID cua thong diep
		sendMsg((SOCKET)desClientSock, message, len);

		if (len == 7) { //fix bug
			for (int i = 1; i <= nEvents; i++) {
				if (socks[i] == desClientSock) {
					clients[i].downloadClient = 0;
					break;
				}
			}
		}
		sendACK(client.socket);
		break;

	case '5':
		char code[2];
		memcpy(code, message + 7, 2);

		if (memcmp(code, "40", 2) == 0) { // Da nhan duoc block
										  // Gui cho client gui file message bao rang client yeu cau file da nhan duoc block
			memcpy(message + 1, &client.socket, 4);
			sendMsg(client.downloadClient, message, len);
		}
		else {
			printf("Unknown code\n");
		}
		sendACK(client.socket);
		break;

	default:
		break;
	}
}

/*
n: index of client
*/
void recvMsg(Client &client, int n)
{
	char buff[BUFF_SIZE];
	int ret, nLeft, index;
	unsigned short payloadLen;
	char opcode;

	//while (1) {
	nLeft = 7;
	index = 0;
	while (nLeft > 0) {
		ret = recv(client.socket, buff + index, nLeft, 0);
		if (ret == SOCKET_ERROR) {
			//printf("Error %d: Cannot receive message\n", WSAGetLastError());
			//disconnect(n); return;
			//break;
		}
		else {
			nLeft -= ret; index += ret;
		}
	}

	opcode = buff[0];

	memcpy(&payloadLen, buff + 5, 2);
	//cout << "Opcode-" << buff[0] << " length: " << payloadLen << endl;
	nLeft = payloadLen;
	while (nLeft > 0) {
		ret = recv(client.socket, buff + index, nLeft, 0);
		if (ret == SOCKET_ERROR) {
			//printf("Error %d: Cannot receive message\n", WSAGetLastError());
			//return -1;
		}
		else {
			nLeft -= ret; index += ret;
		}
	}

	handleMsg(client, buff, payloadLen + 7);
}


/*
index: index of client in array clients
*/
void removeClient(int index)
{
	for (int i = 1; i < nEvents; i++) {
		if (clients[i].isSearching && clients[i].currNumClient >= index) {
			// Giai thich 1 chut: Khi 1 client moi duoc ket noi den thi no se co index lon hon currNumClient vi client moi luon duoc them vao cuoi mang
			clients[i].currNumClient--;

			// Neu client nay da phan hoi
			if ((clients[i].clientHaveFile).find(socks[index]) != (clients[i].clientHaveFile).end()) {
				// Client nay co file
				// Xoa client nay khoi set clientHaveFile
				(clients[i].clientHaveFile).erase(socks[index]);
			}
			else { // Client nay chua phan hoi
				if (clients[i].currNumClient == clients[i].returnNumClient) { // So luong client duoc gui yeu cau download = so luong client da phan hoi
					char buff[BUFF_SIZE];

					if (clients[index].clientHaveFile.empty()) { // Khong co client nao co file
																 // Gui message bao loi tim kiem file: opcode = '5' va payload = '10'
						buff[0] = '5';
						buff[7] = '1';
						buff[8] = '0';
						unsigned short payloadLen = 2;
						memcpy(buff + 5, &payloadLen, 2);
						sendMsg(clients[i].downloadClient, buff, 9);
					}
					else {
						// Gui message voi opcode = 3, payload chua danh sach cac client co file, ngan cach nhau boi dau '-'
						buff[0] = '3';

						char list[1024];
						strcpy_s(list, "");
						for (auto sock : clients[index].clientHaveFile) {
							strcat_s(list, to_string(sock).c_str());
							strcat_s(list, "-");
						}

						unsigned short payloadLen = strlen(list) - 1;
						list[payloadLen] = 0; // Xoa ky tu '-' o cuoi cung

						memcpy(buff + 7, list, payloadLen);
						memcpy(buff + 5, &payloadLen, 2);

						sendMsg(clients[i].downloadClient, buff, 7 + payloadLen);
					}

					clients[index].isSearching = false;
					clients[index].currNumClient = 0;
					clients[index].returnNumClient = 0;
					clients[index].clientHaveFile.clear();
				}
			}
		}
	}
}

void notifyDownloadingClient(int index) {
	for (int i = 1; i <= nEvents; i++) {
		if (clients[i].downloadClient == socks[index]) {
			// Gui thong diep loi download file: opcode = '5' va payload = '20'
			char buff[9];
			buff[0] = '5';
			buff[7] = '2';
			buff[8] = '0';
			unsigned short payloadLen = 2;
			memcpy(buff + 5, &payloadLen, 2);
			sendMsg(clients[i].socket, buff, 9);
			cout << "Send a notify downloading client" << endl;
		}
	}
}

/*
Handle event that a client disconnected

@param [in] index: index of client in clients array
*/
void disconnect(int index)
{
	notifyDownloadingClient(index);

	// Xoa client tu cac set clientHaveFile
	removeClient(index);

	printf("Client [%s, %d] disconected.\n", clients[index].ip, clients[index].port);

	// Don mang thay vi doi cho nhu truoc
	closesocket(socks[index]);
	socks[index] = 0;
	WSACloseEvent(events[index]);

	for (int i = index; i < nEvents - 1; i++) {
		socks[i] = socks[i + 1];
		events[i] = events[i + 1];
		clients[i] = clients[i + 1];
	}

	nEvents--;

	/*
	closesocket(socks[index]);
	socks[index] = 0;
	WSACloseEvent(events[index]);

	nEvents -= 1;


	if (nEvents != index) {
	socks[index] = socks[nEvents];
	socks[nEvents] = 0;
	WSACloseEvent(events[nEvents]);
	events[index] = WSACreateEvent();

	clients[index] = clients[nEvents];
	WSAEventSelect(socks[index], events[index], FD_READ | FD_CLOSE);
	}
	*/
}

void sendACK(SOCKET s)
{
	char buff[9];
	buff[0] = '5';
	buff[7] = '5';
	buff[8] = '0';
	unsigned short len = 2;
	memcpy(buff + 5, &len, 2);

	sendMsg(s, buff, 9);
}
