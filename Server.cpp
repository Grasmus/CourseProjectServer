#include <iostream>
#include <winsock2.h>
#include <ws2tcpip.h>
#include <stdio.h>
#include <vector>
#include <map>

#pragma comment (lib, "Ws2_32.lib")

#define DEFAULT_PORT "27015"
#define DEFAULT_BUFLEN 512

using namespace std;

HANDLE mutex{};

struct ClientData
{
    float cpuUsage{};
    int freeMemSpace{};
    double data{};
};

struct ReadClientDataParams
{
    SOCKET listenSocket{};
    fd_set* readfds{};
    map<SOCKET, ClientData>* connectedClients{};
};

SOCKET generateSocket(PCSTR port)
{
    struct addrinfo* result = nullptr, * ptr = nullptr, hints{};

    hints.ai_family = AF_UNSPEC;
    hints.ai_protocol = IPPROTO_TCP;
    hints.ai_socktype = SOCK_STREAM;
    hints.ai_flags = 0;
    hints.ai_addr = NULL;
    hints.ai_canonname = NULL;
    hints.ai_next = NULL;

    int iResult = getaddrinfo(NULL, port, &hints, &result);

    if (iResult != 0)
    {
        cerr << "getaddrinfo failed: " << iResult << endl;

        return -1;
    }

    SOCKET listenSocket{ INVALID_SOCKET };

    int optval{ 1 };

    for (auto resultIterator = result; resultIterator != NULL; resultIterator = resultIterator->ai_next)
    {
        listenSocket = socket(result->ai_family, result->ai_socktype, result->ai_protocol);

        if (listenSocket == INVALID_SOCKET)
        {
            continue;
        }

        if (setsockopt(listenSocket, SOL_SOCKET, SO_REUSEADDR, (char*)&optval, sizeof(int)) == -1)
        {
            cerr << "setsockopt() error" << endl;

            closesocket(listenSocket);

            return -1;
        }

        iResult = bind(listenSocket, result->ai_addr, (int)result->ai_addrlen);

        if (iResult == SOCKET_ERROR)
        {
            cerr << "bind failed with error: " << WSAGetLastError() << endl;

            freeaddrinfo(result);
            closesocket(listenSocket);

            return -1;
        }
    }

    freeaddrinfo(result);

    if (listen(listenSocket, SOMAXCONN) == SOCKET_ERROR)
    {
        cerr << "Listen failed with error: " << WSAGetLastError() << endl;

        closesocket(listenSocket);

        return -1;
    }

    return listenSocket;
}

SOCKET establishConnection(SOCKET socket, fd_set* readfds, map<SOCKET, ClientData>* connectedClients)
{
    SOCKET ClientSocket{ INVALID_SOCKET };

    ClientSocket = accept(socket, NULL, NULL);

    if (ClientSocket == INVALID_SOCKET)
    {
        cerr << "Accept failed: " << WSAGetLastError() << endl;

        closesocket(socket);

        return -1;
    }

    if (!FD_ISSET(ClientSocket, readfds))
    {
        FD_SET(ClientSocket, readfds);
    }

    if (connectedClients->find(ClientSocket) == connectedClients->end())
    {
        (*connectedClients)[ClientSocket] = ClientData();
    }

    return ClientSocket;
}

void shutdownSocket(SOCKET socket)
{
    int iResult = shutdown(socket, SD_SEND);

    if (iResult == SOCKET_ERROR)
    {
        cerr << "shutdown failed: " << WSAGetLastError() << endl;
    }

    closesocket(socket);
    WSACleanup();
}

SOCKET getMaxSocket(SOCKET listenSocket, map<SOCKET, ClientData>* connectedClients)
{
    SOCKET max_sd{};

    if (connectedClients->size() == 0)
    {
        return listenSocket;
    }

    for(pair<SOCKET, ClientData> connectedClient : *connectedClients)
    {
        if (connectedClient.first > max_sd)
        {
            max_sd = connectedClient.first;
        }
    }

    return max_sd;
}

DWORD WINAPI connectNewClient(LPVOID lpParams)
{
    ReadClientDataParams params{ *(ReadClientDataParams*)lpParams };
    int  activity{};
    ClientData clientData{};
    DWORD waitResult{};
    int iter{};

    while (iter < 1)
    {
        waitResult = WaitForSingleObject(mutex, 20);

        switch (waitResult)
        {
            case WAIT_OBJECT_0:

                FD_ZERO(params.readfds);

                FD_SET(params.listenSocket, params.readfds);

                for (auto client : *params.connectedClients)
                {
                    if (client.first > 0)
                    {
                        FD_SET(client.first, params.readfds);
                    }
                }
            
                if (FD_ISSET(params.listenSocket, params.readfds))
                {
                    activity = select(getMaxSocket(params.listenSocket, params.connectedClients), params.readfds, NULL, NULL, NULL);

                    if ((activity < 0) && (errno != EINTR))
                    {
                        cerr << "select() error" << endl;

                        ReleaseMutex(mutex);

                        continue;
                    }

                    SOCKET clientSocket{ establishConnection(params.listenSocket, params.readfds, params.connectedClients) };

                    if (clientSocket == -1)
                    {
                        cerr << "Can't connect to client" << endl;

                        ReleaseMutex(mutex);

                        continue;
                    }

                    char messageChunk[DEFAULT_BUFLEN]{};

                    int size = recv(clientSocket, messageChunk, DEFAULT_BUFLEN - 1, 0);

                    if (size == sizeof(ClientData))
                    {
                        memcpy(&clientData, messageChunk, sizeof(ClientData));

                        (*params.connectedClients)[clientSocket] = clientData;
                    }
                    else
                    {
                        closesocket(clientSocket);
                    }
                }

                ReleaseMutex(mutex);

                break;

            default:
                break;
        }

        iter++;
    }

    return 0;
}

DWORD WINAPI receiveMessage(LPVOID lpParams)
{
    ReadClientDataParams params{ *(ReadClientDataParams*)lpParams };
    DWORD waitResult{};

    while (true)
    {
        waitResult = WaitForSingleObject(mutex, INFINITE);

        switch (waitResult)
        {
            case WAIT_OBJECT_0:

                vector<SOCKET> clientsToErase{};

                cout << "receiveMessage" << endl;

                for (pair<SOCKET, ClientData> client : *params.connectedClients)
                {
                    if (FD_ISSET(client.first, params.readfds))
                    {
                        string message{};

                        char messageChunk[DEFAULT_BUFLEN]{};

                        int size = recv(client.first, messageChunk, DEFAULT_BUFLEN - 1, 0);

                        message.append(messageChunk);

                        if (message == "DISCONNECT")
                        {
                            shutdownSocket(client.first);

                            clientsToErase.push_back(client.first);

                            FD_CLR(client.first, params.readfds);

                            cout << "Received: " << message << endl;
                        }
                    }
                }

                for (SOCKET clientSocket : clientsToErase)
                {
                    params.connectedClients->erase(clientSocket);
                }

                clientsToErase.clear();

                ReleaseMutex(mutex);

                break;
        }
    }

    return 0;
}

void sendMessage(SOCKET clientSocket, const char* sendBuf, int size, fd_set* readfds, map<SOCKET, ClientData>* connectedClients)
{
    int iResult{ send(clientSocket, sendBuf, size, 0) };

    if (iResult == SOCKET_ERROR)
    {
        cerr << "send failed: " << WSAGetLastError() << endl;

        connectedClients->erase(clientSocket);

        FD_CLR(clientSocket, readfds);

        closesocket(clientSocket);

        return;
    }
}

int main()
{
    WSADATA wsaData{};
    int iResult{ WSAStartup(MAKEWORD(2, 2), &wsaData) };

    if (iResult != 0)
    {
        cerr << "WSAStartup failed with error: " << iResult << endl;
        return 1;
    }
    
    mutex = CreateMutex(NULL, FALSE, NULL);

    if (mutex == NULL)
    {
        cerr << "CreateMutex error" << endl;

        WSACleanup();

        return 1;
    }

    SOCKET listenSocket{ generateSocket(DEFAULT_PORT) };

    if (listenSocket == -1)
    {
        WSACleanup();

        return 1;
    }

    struct sockaddr_storage client {};
    socklen_t addrLen{ sizeof(sockaddr_storage) };

    fd_set readfds{};

    int max_sd{};

    vector<int> clientSockets{};

    map<SOCKET, ClientData> connectedClients{};

    ReadClientDataParams readClientDataParams{};

    readClientDataParams.connectedClients = &connectedClients;
    readClientDataParams.listenSocket = listenSocket;
    readClientDataParams.readfds = &readfds;

    DWORD connectNewClientExitCode{};
    DWORD receiveMessageExitCode{};

    HANDLE connectNewClientThread
    { 
        CreateThread(
            NULL,
            0,
            connectNewClient,
            (LPVOID)&readClientDataParams,
            0,
            0
        )
    };

    if (connectNewClientThread == NULL)
    {
        cerr << "Can't create thread connectNewClientThread" << endl;

        closesocket(listenSocket);

        WSACleanup();
    }

    HANDLE receiveMessageThread
    {
        CreateThread(
            NULL,
            0,
            receiveMessage,
            (LPVOID)&readClientDataParams,
            0,
            0
        )
    };

    if (receiveMessageThread == NULL)
    {
        cerr << "Can't create thread receiveMessageThread" << endl;

        GetExitCodeThread(connectNewClientThread, &connectNewClientExitCode);
        TerminateThread(connectNewClientThread, connectNewClientExitCode);

        closesocket(listenSocket);

        WSACleanup();
    }

    WaitForSingleObject(connectNewClientThread, INFINITE);
    WaitForSingleObject(receiveMessageThread, INFINITE);

    FD_ZERO(&readfds);

    for (auto client : connectedClients)
    {
        closesocket(client.first);
    }

    closesocket(listenSocket);

    WSACleanup();

	return 0;
}
