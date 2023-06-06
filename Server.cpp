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

static bool exitThread{ false };

enum class Task
{
    Multiply,
    Divide,
    Pow,
    Disconnect
};

struct TaskData
{
    double a{};
    double b{};
    Task task{};
};

struct ClientData
{
    int cpuUsage{};
    double freeMemSpace{};
    double data{};
};

struct Client
{
    ClientData clientData{};
    bool received{};
    bool busy{ false };
};

struct ReadClientDataParams
{
    SOCKET listenSocket{};
    fd_set* readfds{};
    map<SOCKET, Client>* connectedClients{};
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

SOCKET establishConnection(SOCKET socket, fd_set* readfds, map<SOCKET, Client>* connectedClients)
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
        (*connectedClients)[ClientSocket] = Client();
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

SOCKET getMaxSocket(SOCKET listenSocket, map<SOCKET, Client>* connectedClients)
{
    SOCKET max_sd{};

    if (connectedClients->size() == 0)
    {
        return listenSocket;
    }

    for(pair<SOCKET, Client> connectedClient : *connectedClients)
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
    timeval time{};
    time.tv_sec = 1;

    while (!exitThread)
    {
        waitResult = WaitForSingleObject(mutex, INFINITE);

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

                activity = select(getMaxSocket(params.listenSocket, params.connectedClients), params.readfds, NULL, NULL, &time);

                if ((activity < 0) && (errno != EINTR))
                {
                    cerr << "select() error" << endl;

                    ReleaseMutex(mutex);

                    continue;
                }
            
                if (FD_ISSET(params.listenSocket, params.readfds))
                {
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

                        Client client{};

                        client.busy = false;
                        client.clientData = clientData;

                        (*params.connectedClients)[clientSocket] = client;
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

        if (exitThread)
        {
            ReleaseMutex(mutex);

            break;
        }
    }

    return 0;
}

DWORD WINAPI receiveMessage(LPVOID lpParams)
{
    ReadClientDataParams params{ *(ReadClientDataParams*)lpParams };
    DWORD waitResult{};

    while (!exitThread)
    {
        waitResult = WaitForSingleObject(mutex, INFINITE);

        switch (waitResult)
        {
            case WAIT_OBJECT_0:

                vector<SOCKET> clientsToErase{};

                for (map<SOCKET, Client>::iterator client{ params.connectedClients->begin() }; client != params.connectedClients->end(); client++)
                {
                    if (FD_ISSET(client->first, params.readfds))
                    {
                        char message[DEFAULT_BUFLEN]{};

                        int size = recv(client->first, message, DEFAULT_BUFLEN - 1, 0);

                        if (size <= 0)
                        {
                            closesocket(client->first);

                            clientsToErase.push_back(client->first);

                            FD_CLR(client->first, params.readfds);
                        }
                        else if (size == sizeof(ClientData))
                        {
                            ClientData clientData{};

                            memcpy(&clientData, message, size);

                            client->second.clientData = clientData;
                            client->second.received = true;
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

        if (exitThread)
        {
            ReleaseMutex(mutex);

            break;
        }
    }

    return 0;
}

void sendMessage(SOCKET clientSocket, const char* sendBuf, int size, fd_set* readfds, map<SOCKET, Client>* connectedClients)
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

SOCKET chooseBestClient(map<SOCKET, Client>* connectedClients)
{
    while (connectedClients->size() == 0)
    {
        if (connectedClients->size())
        {
            break;
        }
    }

    double clientScore{};
    SOCKET clientSocket{};

    bool newClientScoreApplied{ false };

    map<SOCKET, Client> connectedClientsData{ *connectedClients };

    while (!newClientScoreApplied)
    {
        connectedClientsData = *connectedClients;

        for (auto& connectedClient : connectedClientsData)
        {
            if (!connectedClient.second.busy)
            {
                double newClientScore{ (100.0 - connectedClient.second.clientData.cpuUsage) * connectedClient.second.clientData.freeMemSpace };

                if (newClientScore >= clientScore)
                {
                    clientScore = newClientScore;
                    clientSocket = connectedClient.first;

                    newClientScoreApplied = true;
                }
            }
        }

        if (newClientScoreApplied)
        {
            break;
        }
    }

    return clientSocket;
}

#pragma region Tasks

double firstTask(fd_set* readfds, map<SOCKET, Client>* connectedClients)
{
    double a{ 5.5 }, b{ -10 }, c{ 200.3 }, d{ 161 };

    SOCKET client{ chooseBestClient(connectedClients) };

    (*connectedClients)[client].busy = true;
    (*connectedClients)[client].received = false;

    TaskData taskData{};

    taskData.a = a;
    taskData.b = b;
    taskData.task = Task::Multiply;

    sendMessage(client, (char*)&taskData, sizeof(TaskData), readfds, connectedClients);
 
    while (true)
    {
        if ((*connectedClients)[client].received == true)
        {
            break;
        }
    }

    (*connectedClients)[client].busy = false;
    (*connectedClients)[client].received = false;

    exitThread = true;

    return (*connectedClients)[client].clientData.data;
}

double secondTask(fd_set* readfds, map<SOCKET, Client>* connectedClients)
{
    double a{ 5.5 }, b{ -10 }, c{ 200.3 }, d{ 161 };

    SOCKET clients[2]{};

    for (int i{}; i < 2; i++)
    {
        clients[i] = chooseBestClient(connectedClients);

        (*connectedClients)[clients[i]].busy = true;
        (*connectedClients)[clients[i]].received = false;
    }

    TaskData taskData{};

    taskData.a = a;
    taskData.b = b;
    taskData.task = Task::Multiply;

    sendMessage(clients[0], (char*)&taskData, sizeof(TaskData), readfds, connectedClients);

    taskData.a = c;
    taskData.b = d;
    taskData.task = Task::Pow;

    sendMessage(clients[1], (char*)&taskData, sizeof(TaskData), readfds, connectedClients);

    while (true)
    {
        if ((*connectedClients)[clients[0]].received == true &&
            (*connectedClients)[clients[1]].received == true)
        {
            break;
        }
    }

    exitThread = true;

    return (*connectedClients)[clients[0]].clientData.data + (*connectedClients)[clients[1]].clientData.data;
}

#pragma endregion

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

    map<SOCKET, Client> connectedClients{};

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

    cout << "Result: " << firstTask(&readfds, &connectedClients);

    WaitForSingleObject(connectNewClientThread, INFINITE);
    WaitForSingleObject(receiveMessageThread, INFINITE);

    FD_ZERO(&readfds);

    for (auto& client : connectedClients)
    {
        TaskData taskData{};

        taskData.task = Task::Disconnect;

        sendMessage(client.first, (char*)& taskData, sizeof(TaskData), &readfds, &connectedClients);

        closesocket(client.first);
    }

    closesocket(listenSocket);

    WSACleanup();

	return 0;
}
