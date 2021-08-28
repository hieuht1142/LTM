#include "stdafx.h"
#include <WinSock2.h>
#include <WS2tcpip.h>
#include <iostream>
#include <string>
#include <vector>
#include <windows.h>
#include <fstream>
#include <process.h>
#include <map>
#include <mutex>
#include <condition_variable>
#include <deque>

#pragma comment (lib, "Ws2_32.lib")

#define SERVER_ADDR "127.0.0.1"
#define SERVER_PORT 5500
#define BLOCK_SIZE 65535
#define BUFF_SIZE 65542

using namespace std;

struct MESSAGE {
	char opcode;
	unsigned int id;
	unsigned short length;
	char* payload;
};

struct SEND_DATA {
	char* buff;
	int len;

	SEND_DATA(int _len) : len{ _len } {
		buff = new char[_len];
	}
};

struct DOWNLOAD_INFO {
	string absFilePath;
	streamoff offset;
};

template <typename T>
class blocking_queue
{
private:
	mutex              d_mutex;
	condition_variable d_condition;
	deque<T>           d_queue;

public:
	void push(T const& value) {
		{
			unique_lock<mutex> queueLock(this->d_mutex);
			d_queue.push_front(value);
		}
		this->d_condition.notify_one();
	}

	T pop() {
		unique_lock<mutex> queueLock(this->d_mutex);
		this->d_condition.wait(queueLock, [=] { return !this->d_queue.empty(); });
		T rc(move(this->d_queue.back()));
		this->d_queue.pop_back();
		return rc;
	}

	int size() {
		unique_lock<mutex> lock(this->d_mutex);
		return this->d_queue.size();
	}
};

SOCKET client;
string sharedFolderPath;
string downloadFolderPath;
string requestedFileName;
map<unsigned int, DOWNLOAD_INFO> downloadClientInfos;
blocking_queue<SEND_DATA> sendQueue;
HANDLE hExitEvent;
CRITICAL_SECTION criticalSection;
bool recvACK = false;
mutex recvACK_mutex;
condition_variable recvACK_condition;

bool existsFile(LPCSTR lpFileName) {
	WIN32_FIND_DATAA FindFileData;
	HANDLE hFind;

	hFind = FindFirstFileA(lpFileName, &FindFileData);
	if (hFind == INVALID_HANDLE_VALUE) {
		return false;
	}
	else {
		FindClose(hFind);
		return true;
	}
}

void handleFindFile(MESSAGE message) {
	SEND_DATA data(8);
	data.buff[0] = '3';
	memcpy(data.buff + 1, &message.id, 4);
	unsigned short length = 1;
	memcpy(data.buff + 5, &length, 2);

	string absFilePath = sharedFolderPath + "\\";
	absFilePath.append(message.payload, message.length);
	if (existsFile(absFilePath.c_str())) {
		data.buff[7] = '1';
	}
	else {
		data.buff[7] = '0';
	}

	sendQueue.push(data);
}

void requestFindFile() {
	do {
		cout << endl << "Do you want to find a file (1: Find file, 2: Exit) ?" << endl;
		cout << "Your selection: ";
		string selection; getline(cin, selection);

		if (selection == "1") {
			do {
				cout << "Enter file name: ";
				getline(cin, requestedFileName);
				if (requestedFileName.find('.') != string::npos) {
					break;
				}
				else {
					cout << "File name must have an extension part!" << endl;
				}
			} while (true);

			unsigned short payloadLen = requestedFileName.length();
			SEND_DATA data(payloadLen + 7);
			data.buff[0] = '1';
			unsigned int id = 0;
			memcpy(data.buff + 1, &id, 4);
			memcpy(data.buff + 5, &payloadLen, 2);
			memcpy(data.buff + 7, requestedFileName.c_str(), payloadLen);

			sendQueue.push(data);
			break;
		}
		else if (selection == "2") {
			exit(0);
			//SetEvent(hExitEvent); break;
		}
		else {
			cout << "Invalid selection!" << endl;
		}
	} while (true);
}

vector<string> split(string s, string delimiter) {
	vector<string> result;
	size_t position = 0;

	while ((position = s.find(delimiter)) != string::npos) {
		result.push_back(s.substr(0, position));
		s.erase(0, position + delimiter.length());
	}
	result.push_back(s);

	return result;
}

/*
void handleFindFileResult(MESSAGE message) {
if (message.length == 0) {
cout << "No client has a such file!" << endl;
requestFindFile();
return;
}

do {
cout << endl << "Do you want to download file (1: Download, 2: Exit) ?" << endl;
cout << "Your selection: ";
string selection; getline(cin, selection);

if (selection == "1") {
string payloadStr(message.payload, message.length);
vector<string> clientIDs = split(payloadStr, "-");

cout << "Selects a client id to download: " << endl;
for (int i = 0; i < clientIDs.size(); i++) {
cout << i << ". " << clientIDs[i] << endl;
}

cout << "Your selected id: ";
string idxStr; getline(cin, idxStr);
int idx = stoi(idxStr);

unsigned short payloadLen = clientIDs[idx].length();
SEND_DATA data(payloadLen + 7);
data.buff[0] = '2';
unsigned int id = 0;
memcpy(data.buff + 1, &message.id, 4);
memcpy(data.buff + 5, &payloadLen, 2);
memcpy(data.buff + 7, clientIDs[idx].c_str(), payloadLen);

sendQueue.push(data);
break;
}
else if (selection == "2") {
exit(0);
//SetEvent(hExitEvent); break;
}
else {
cout << "Invalid selection!" << endl;
}
} while (true);
}
*/

void handleFindFileResult(MESSAGE message) {
	if (message.length == 0) {
		cout << "No client has that file!" << endl;
		requestFindFile();
		return;
	}

	string payloadStr(message.payload, message.length);
	vector<string> clientIDs = split(payloadStr, "-");

	//display find file result
	cout << "Clients have that file:" << endl;
	for (int i = 0; i < clientIDs.size(); i++) {
		cout << i << ". " << clientIDs[i] << endl;
	}

	do {
		cout << endl << "Do you want to download file (1: Download, 2: Exit, 3: Back) ?" << endl;
		cout << "Your selection: ";
		string selection; getline(cin, selection);

		if (selection == "1") {
			int idx = -1;

			do {
				cout << "Select a client index to download: " << endl;
				cout << "Your selected index: ";
				string idxStr; getline(cin, idxStr);

				try {
					idx = stoi(idxStr);
					if (0 <= idx && idx < clientIDs.size()) {
						break;
					}
				}
				catch (invalid_argument exception) {}

				cout << "Invalid index!" << endl;
			} while (true);

			unsigned short payloadLen = clientIDs[idx].length();
			SEND_DATA data(payloadLen + 7);
			data.buff[0] = '2';
			unsigned int id = 0;
			memcpy(data.buff + 1, &message.id, 4);
			memcpy(data.buff + 5, &payloadLen, 2);
			memcpy(data.buff + 7, clientIDs[idx].c_str(), payloadLen);

			sendQueue.push(data);
			break;
		}
		else if (selection == "2") {
			exit(0);
			//SetEvent(hExitEvent); break;
		}
		else if (selection == "3") {
			requestFindFile();
		}
		else {
			cout << "Invalid selection!" << endl;
		}
	} while (true);
}

void sendBlockFile(unsigned int id) {
	SEND_DATA data(BUFF_SIZE);
	data.buff[0] = '4';
	memcpy(data.buff + 1, &id, 4);
	unsigned short payloadLen;

	EnterCriticalSection(&criticalSection);

	ifstream source(downloadClientInfos[id].absFilePath, ios::binary);

	if (source.good()) {
		source.seekg(downloadClientInfos[id].offset, ios::beg);

		if (!source.eof()) {
			source.read(data.buff + 7, BLOCK_SIZE);

			payloadLen = source.gcount();
			memcpy(data.buff + 5, &payloadLen, 2);
			data.len = payloadLen + 7;

			sendQueue.push(data);
			downloadClientInfos[id].offset += payloadLen;
		}
		else {
			payloadLen = 0;
			memcpy(data.buff + 5, &payloadLen, 2);
			data.len = 0;

			sendQueue.push(data);
			downloadClientInfos.erase(id);
		}

		source.close();
	}

	LeaveCriticalSection(&criticalSection);
}

void handleDownloadFile(MESSAGE message) {
	DOWNLOAD_INFO info;
	info.absFilePath = sharedFolderPath + "\\";
	info.absFilePath.append(message.payload, message.length);
	info.offset = 0;

	EnterCriticalSection(&criticalSection);
	downloadClientInfos[message.id] = info;
	LeaveCriticalSection(&criticalSection);

	sendBlockFile(message.id);
}

void handleFileDataResult(MESSAGE message) {
	if (message.length > 0) {
		string absFilePath = downloadFolderPath + "\\" + requestedFileName;

		EnterCriticalSection(&criticalSection);

		ofstream destination(absFilePath, ios::app | ios::binary);
		destination.write(message.payload, message.length);
		destination.close();

		LeaveCriticalSection(&criticalSection);

		//send ACK
		SEND_DATA data(9);
		data.buff[0] = '5';
		unsigned int id = 0;
		memcpy(data.buff + 1, &id, 4);
		unsigned short payloadLen = 2;
		memcpy(data.buff + 5, &payloadLen, 2);
		memcpy(data.buff + 7, "40", 2);

		sendQueue.push(data);
	}
	else {
		requestFindFile();
	}
}

void noticeRecvACK() {
	{
		unique_lock<mutex> lock(recvACK_mutex);
		recvACK = true;
	}
	recvACK_condition.notify_one();
}

void handleNotice(MESSAGE message) {
	string noticeStr(message.payload, message.length);

	try {
		unsigned short noticeCode = stoi(noticeStr);

		switch (noticeCode)
		{
		case 10:
			cout << "Find fail!" << endl;
			noticeRecvACK();
			requestFindFile();
			break;
		case 11:
			cout << "Finding ..." << endl;
			noticeRecvACK();
			break;
		case 20:
			cout << "Download fail!" << endl;
			noticeRecvACK();
			requestFindFile(); break;
		case 21:
			cout << "Downloading ..." << endl;
			noticeRecvACK();
			break;
		case 40:
			sendBlockFile(message.id); break;
		default:
			noticeRecvACK();
			break;
		}
	}
	catch (invalid_argument exception) {
		cout << "noticeStr = " << noticeStr << endl;
		cout << "Notice message can't be parsed to integer. Terminate program!" << endl;
		exit(1);
	}
}

unsigned _stdcall handleThread(void* param) {
	MESSAGE* message = (MESSAGE*)param;

	switch (message->opcode)
	{
	case '1':
		handleFindFile(*message); break;
	case '2':
		handleDownloadFile(*message); break;
	case '3':
		handleFindFileResult(*message); break;
	case '4':
		handleFileDataResult(*message); break;
	case '5':
		handleNotice(*message); break;
	default:
		break;
	}

	delete[] message->payload;
	delete message;
	return 0;
}

unsigned _stdcall recvThread(void* param) {
	while (1) {
		try {
			MESSAGE* message = new MESSAGE();
			char header[7];
			recv(client, header, 7, MSG_WAITALL);
			message->opcode = header[0];
			memcpy(&message->id, header + 1, 4);
			memcpy(&message->length, header + 5, 2);

			if (message->length > 0) {
				message->payload = new char[message->length];
				recv(client, message->payload, message->length, MSG_WAITALL);
			}
			else {
				message->payload = new char[0];
			}

			_beginthreadex(0, 0, handleThread, (void*)message, 0, 0);
		}
		catch (bad_alloc exception) {
			cout << "Memory can't be allocated. Terminate program!" << endl;
			exit(1);
		}

	}

	return 0;
}

unsigned _stdcall sendThread(void* param) {
	while (1) {
		SEND_DATA data = sendQueue.pop();

		int ret = send(client, data.buff, data.len, 0);
		if (ret == SOCKET_ERROR) {
			cout << "Error " << WSAGetLastError() << " : Can't send data!" << endl;
		}

		delete[] data.buff;

		unique_lock<mutex> condition_lock(recvACK_mutex);
		recvACK_condition.wait(condition_lock, [=] { return recvACK == true; });
		recvACK = false;
	}

	return 0;
}

void enterFolders() {
	cout << endl;

	while (true) {
		cout << "Enter your shared folder path: ";
		getline(cin, sharedFolderPath);

		if (sharedFolderPath.find(".") != string::npos) {
			cout << "Folder name must not have extension part!" << endl;
		}
		else if (existsFile(sharedFolderPath.c_str())) {
			break;
		}
		else {
			cout << "Invalid folder!" << endl;
		}
	}

	while (true) {
		cout << "Enter your download folder path: ";
		getline(cin, downloadFolderPath);

		if (downloadFolderPath.find(".") != string::npos) {
			cout << "Folder name must not have extension part!" << endl;
		}
		else if (existsFile(downloadFolderPath.c_str())) {
			break;
		}
		else {
			cout << "Invalid folder!" << endl;
		}
	}
}

/*
string prepareDownloadFileName() {

}
*/

int main(int argc, char *argv[])
{
	if (argc != 3) {
		printf("Execution error.");
		return 0;
	}

	char* serverIP = argv[1];
	int serverPort = atoi(argv[2]);

	enterFolders();

	//initiates winsock v2.2
	WORD wVersion = MAKEWORD(2, 2);
	WSADATA wsaData;
	if (WSAStartup(wVersion, &wsaData)) {
		cout << "Winsock v2.2 is not supported." << endl;
		return 0;
	}

	//creates socket
	client = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);

	if (client == INVALID_SOCKET) {
		cout << "Error " << WSAGetLastError() << ": Cannot create client socket." << endl;
		return 0;
	}

	//specifies server address
	sockaddr_in serverAddr;
	serverAddr.sin_family = AF_INET;
	serverAddr.sin_port = htons(serverPort);
	inet_pton(AF_INET, serverIP, &serverAddr.sin_addr);

	//Request to connect server
	if (connect(client, (sockaddr*)&serverAddr, sizeof(serverAddr))) {
		cout << "Error " << WSAGetLastError() << ": Cannot connect to server." << endl;
		return 0;
	}

	cout << "Connected server!" << endl;

	hExitEvent = CreateEvent(
		NULL,               // default security attributes
		FALSE,               // auto-reset event
		FALSE,              // initial state is nonsignaled
		TEXT("ExitEvent")  // object name
	);

	InitializeCriticalSection(&criticalSection);

	_beginthreadex(0, 0, recvThread, NULL, 0, 0);
	_beginthreadex(0, 0, sendThread, NULL, 0, 0);
	requestFindFile();
	WaitForSingleObject(hExitEvent, INFINITE);

	DeleteCriticalSection(&criticalSection);

	closesocket(client);
	WSACleanup();

	return 0;
}