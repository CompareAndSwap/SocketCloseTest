// Linux: g++ -o socketclosetest SocketCloseTest.cpp -lpthread
// Windows: Visual Studio

#ifdef _WIN32
#define _CRT_SECURE_NO_WARNINGS // for sscanf
#include <winsock2.h>
#include <Windows.h>
#endif

#include <stdio.h>
#include <stdlib.h> // for EXIT_FAILURE
#include <string.h> // for strerror()
#include <stdarg.h> // for va_arg stuff

#ifndef _WIN32
#include <errno.h>
#include <pthread.h>
#include <unistd.h>

#include <netinet/in.h>
#endif

#include <string>

#ifdef _WIN32
#define popen _popen
#define pclose _pclose
typedef int socklen_t;
#else
typedef int SOCKET;
#define SOCKET_ERROR (-1)
#define INVALID_SOCKET (-1)
#define closesocket(x) close(x)

typedef long LONG;
typedef unsigned long DWORD;
#define INFINITE 0xFFFFFFFF

#define WINAPI
typedef DWORD(WINAPI *LPTHREAD_START_ROUTINE)(void*);
#endif

// Windows ephemeral port range is 49152-65535, so pick a port out of the range
// to make it easier to find in netstat output.
static const int PORT = 40000;
static const size_t MAX_ERROR_COUNT = 10;

size_t g_warning_count = 0;
volatile size_t g_connect_count = 0;

#ifdef _WIN32
void TrimEnd(char* s) { // Adapted from http://stackoverflow.com/a/123724
    char* p = s;
    int l = strlen(p);
    while (l != 0 && isspace(p[l - 1])) p[--l] = 0;
}

std::string GetErrorString(DWORD err) {
    char buf[256];
    if (!FormatMessageA(FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS,
                        NULL, err, 0, buf, sizeof(buf), NULL))
        sprintf_s(buf, sizeof(buf), "FormatMessageA(%lu): %lu", err, GetLastError());

    TrimEnd(buf);
    return buf;
}

const char* GetErrorName(int err) {
    if (err == WSAEADDRINUSE)   return "WSAEADDRINUSE";
    if (err == WSAECONNRESET)   return "WSAECONNRESET";
    return NULL;
}

std::string GetErrorNumberDescription(int err) {
    const char* name = GetErrorName(err);
    char buf[128];

    if (name != NULL)
        sprintf_s(buf, sizeof(buf), "%s (%d)", name, err);
    else
        sprintf_s(buf, sizeof(buf), "%d", err);
    return buf;
}
#endif

void voutput(const char *fmt, va_list ap) {
    int err;

#ifdef _WIN32
    err = GetLastError();   // Same as WSAGetLastError()
#else
    err = errno;
#endif

    // Buffer up all output so that multiple threads' output is not interleaved.
    std::string output;
    char buf[1024];

#ifdef _WIN32
    SYSTEMTIME system_time;
    GetLocalTime(&system_time);
    snprintf(buf, sizeof(buf), "[%02u:%02u:%02u:%03u] ", system_time.wHour, system_time.wMinute, system_time.wSecond, system_time.wMilliseconds);
    output.append(buf);
#endif

    output.append("error: ");
    if (fmt != NULL) {
        vsnprintf(buf, sizeof(buf), fmt, ap);
        output.append(buf);
        output.append(": ");
    }
#ifdef _WIN32
    snprintf(buf, sizeof(buf), "error %s: %s\n", GetErrorNumberDescription(err).c_str(), GetErrorString(err).c_str());
#else
    snprintf(buf, sizeof(buf), "error %d: %s\n", err, strerror(err));
#endif
    output.append(buf);

    fputs(output.c_str(), stderr);
}

void error(const char *fmt, ...) {
    va_list ap;

    va_start(ap, fmt);
    voutput(fmt, ap);
    va_end(ap);

    exit(EXIT_FAILURE);
}

#ifndef _WIN32
void Sleep(DWORD milliseconds) {
    if (milliseconds == INFINITE) {
        for (;;)
            Sleep(1000000);
    } else {
        struct timespec t;
        t.tv_sec = milliseconds / 1000;
        t.tv_nsec = (milliseconds % 1000) * 1000000;
        if (nanosleep(&t, NULL) == -1)
            error("nanosleep");
    }
}

LONG InterlockedIncrement(volatile LONG* val) {
    return __sync_add_and_fetch(val, 1);
}
#endif

void warning(const char *fmt, ...) {
    va_list ap;

    va_start(ap, fmt);
    voutput(fmt, ap);
    va_end(ap);

    if (++g_warning_count >= MAX_ERROR_COUNT)
        Sleep(INFINITE);    // block forever to stop error output from subsequent failures
}

#ifdef _WIN32
class Event {
public:
    Event(bool manual_reset, bool initial_state) {
        event_ = CreateEventW(NULL, manual_reset ? TRUE : FALSE, initial_state ? TRUE : FALSE, NULL);
        if (event_ == NULL)
            error("CreateEventW");
    }

    void Set() {
        if (!SetEvent(event_))
            error("SetEvent");
    }

    void Wait() {
        if (WaitForSingleObject(event_, INFINITE) != WAIT_OBJECT_0)
            error("WaitForSingleObject");
    }

private:
    HANDLE event_;
};
#else
class Event {   // Adapted from http://stackoverflow.com/a/178962
public:
    Event(bool manual_reset, bool initial_state)
        : set_(initial_state), manual_reset_(manual_reset) {
        pthread_mutex_init(&mutex_, 0);
        pthread_cond_init(&cond_, 0);
    }

    void Set() {
        pthread_mutex_lock(&mutex_);
        set_ = true;
        pthread_cond_signal(&cond_);
        pthread_mutex_unlock(&mutex_);
    }

    void Wait() {
        pthread_mutex_lock(&mutex_);
        while (!set_)
            pthread_cond_wait(&cond_, &mutex_);
        if (!manual_reset_)
            set_ = false;
        pthread_mutex_unlock(&mutex_);
    }

private:
    pthread_mutex_t mutex_;
    pthread_cond_t cond_;
    bool set_;
    bool manual_reset_;
};
#endif

void StartThread(LPTHREAD_START_ROUTINE pfn, void* pv) {
#ifdef _WIN32
    if (CreateThread(NULL, 0, pfn, pv, 0, NULL) == NULL)
        error("CreateThread");
#else
    pthread_t thread;
    errno = pthread_create(&thread, NULL, (void*(*)(void*))pfn, pv);
    if (errno != 0)
        error("pthread_create");
#endif
}

void NetStat(size_t* server_connections, size_t* client_connections) {
#ifdef _WIN32
    const char* cmd = "netstat -an -p TCP";
#else
    const char* cmd = "netstat -an -t -4";
#endif
    FILE* p = popen(cmd, "r");
    if (p == NULL)
        error("popen");

    *server_connections = 0;
    *client_connections = 0;
    char line[1024];
    while (fgets(line, sizeof(line), p)) {
        int local_port = 0, peer_port = 0;
        char state[64];
        bool parsed;
#ifdef _WIN32
        parsed = sscanf(line, " TCP 127.0.0.1:%d 127.0.0.1:%d %s", &local_port, &peer_port, state) == 3;
#else
        char recv_q[64], send_q[64];
        parsed = sscanf(line, "tcp %s %s 127.0.0.1:%d 127.0.0.1:%d %s", recv_q, send_q, &local_port, &peer_port, state) == 5;
#endif
        if (parsed) {
            if (local_port == PORT)
                (*server_connections)++;
            else if (peer_port == PORT)
                (*client_connections)++;
        }
    }

    if (pclose(p) == -1)
        error("pclose");
}

class Barrier { // Adapted from http://6xq.net/barrier-intro/
public:
    class Context {
    public:
        Context()
            : local_sense_(0) {}

    private:
        LONG local_sense_;

        friend class Barrier;
    };

    Barrier(LONG total)
        : total_(total), count_(0), global_sense_(0) {}

    void Wait(Barrier::Context* context) {
        context->local_sense_ = !context->local_sense_;
        if (InterlockedIncrement(&count_) == total_) {
            count_ = 0;    // reset to zero
            global_sense_ = context->local_sense_;  // release everyone else
        } else {
            while (global_sense_ != context->local_sense_) {
                // Busy-wait the CPU which is wasteful, but perhaps it will
                // cause all the waiters to run very nearly at the same time.
            }
        }
    }

private:
    const LONG total_;
    volatile LONG count_;
    volatile LONG global_sense_;
};

class SynchronizationPolicy {
public:
    virtual void BeforeCloseSocket(SOCKET s) {};
    virtual void AfterCloseSocket() {};
};

class WaitSynchronizationPolicy : public SynchronizationPolicy {
public:
    WaitSynchronizationPolicy(Event* event1, Event* event2)
        : event1_(event1), event2_(event2) {}

    void BeforeCloseSocket(SOCKET s) {
        event1_->Wait();
    }

    void AfterCloseSocket() {
        event2_->Set();
    }

private:
    Event* event1_;
    Event* event2_;
};

class BarrierSynchronizationPolicy : public SynchronizationPolicy {
public:
    BarrierSynchronizationPolicy(Barrier* barrier)
        : barrier_(barrier), barrier_context_() {}

    void BeforeCloseSocket(SOCKET s) {
        barrier_->Wait(&barrier_context_);
    }

private:
    Barrier* barrier_;
    Barrier::Context barrier_context_;
};

class GracefulShutdownSynchronizationPolicy : public SynchronizationPolicy {
public:
    void BeforeCloseSocket(SOCKET s) {
        int result;
        do {
            char buf[64];
            result = recv(s, buf, sizeof(buf), 0);
        } while (result != SOCKET_ERROR && result != 0);

        if (result == SOCKET_ERROR) {
            // Save error because it will be overwritten by
            // getsockname()/getpeername().
#ifdef _WIN32
            DWORD saved_err = GetLastError();
#else
            int saved_err = errno;
#endif
            int local_port = -1, peer_port = -1;
            struct sockaddr_in name;
            socklen_t namelen = sizeof(name);
            if (getsockname(s, (struct sockaddr*)&name, &namelen) != SOCKET_ERROR)
                local_port = ntohs(name.sin_port);
            namelen = sizeof(name);
            if (getpeername(s, (struct sockaddr*)&name, &namelen) != SOCKET_ERROR)
                peer_port = ntohs(name.sin_port);
#ifdef _WIN32
            SetLastError(saved_err);
#else
            errno = saved_err;
#endif
            warning("recv on local port %d, peer port %d", local_port, peer_port);
        }
    }
};

Event g_server_ready(false, false);

DWORD WINAPI TestServerThread(void* arg) {
    SynchronizationPolicy* synch_policy = (SynchronizationPolicy*)arg;

    SOCKET listener = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (listener == INVALID_SOCKET)
        error("socket");
    
#ifndef _WIN32
    // After process exits, allow a subsequent process to grab the port.
    int optval = 1;
    if (setsockopt(listener, SOL_SOCKET, SO_REUSEADDR, (char*)&optval, sizeof(optval)) == SOCKET_ERROR)
        error("setsockopt");
#endif

    struct sockaddr_in sa;
    memset(&sa, 0, sizeof(sa));
    sa.sin_family = AF_INET;
    sa.sin_port = htons(PORT);
    sa.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    if (bind(listener, (const sockaddr*)&sa, sizeof(sa)) == SOCKET_ERROR)
        error("bind");

    if (listen(listener, SOMAXCONN) == SOCKET_ERROR)
        error("listen");

    g_server_ready.Set();   // let client start connecting

    for (;;) {
        SOCKET s = accept(listener, NULL, NULL);
        if (s == INVALID_SOCKET)
            error("accept");

        synch_policy->BeforeCloseSocket(s);

        if (closesocket(s) == SOCKET_ERROR)
            error("closesocket");

        synch_policy->AfterCloseSocket();
    }

    if (closesocket(listener) == SOCKET_ERROR)
        error("closesocket");

    return 0;
}

DWORD WINAPI TestClientThread(void* arg) {
    SynchronizationPolicy* synch_policy = (SynchronizationPolicy*)arg;

    g_server_ready.Wait();  // wait for server to start listening

    for (;;) {
        SOCKET s = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
        if (s == INVALID_SOCKET)
            error("socket");
        
        struct sockaddr_in sa;
        memset(&sa, 0, sizeof(sa));
        sa.sin_family = AF_INET;
        sa.sin_port = htons(PORT);
        sa.sin_addr.s_addr = htonl(INADDR_LOOPBACK);

        if (connect(s, (const sockaddr*)&sa, sizeof(sa)) == SOCKET_ERROR) {
            warning("connect");
            if (closesocket(s) == SOCKET_ERROR)
                error("closesocket");
        } else {
            g_connect_count++;

            synch_policy->BeforeCloseSocket(s);

            if (closesocket(s) == SOCKET_ERROR)
                error("closesocket");

            synch_policy->AfterCloseSocket();
        }
    }

    return 0;
}

int Usage(int /*argc*/, char** argv) {
    fprintf(stderr, "usage: %s <any|client|server|simultaneous|graceful>\n", argv[0]);
    fprintf(stderr, "NOTE: This program runs netstat to display connection statistics.\n");
    return 1;
}

int main(int argc, char** argv) {
    if (argc != 2)
        return Usage(argc, argv);

    // Windows
    // -------
    // any: 11000 connects/sec, WSAEADDRINUSE, netstat: 16300 client, 700 server
    // client close: 11000 connects/sec, WSAEADDRINUSE, netstat: 16000 client connections
    // server close: 6000 connects/sec, no errors, netstat: 16000 server connections, client connections slowly growing
    // simultaneous close: 6500 connects/sec, WSAEADDRINUSE, netstat: 16000 server, 16000 client
    // graceful close: 6500 connects/sec, occasional WSAECONNRESET on recv(), netstat: 16000 server

    // Linux
    // -----
    // any: wide ranging connects/sec (250-80000), no errors, netstat: 2700 server, 14000 client
    // client close: 21000 connects/sec, no errors, netstat: 16000 client connections
    // server close: 16000 connects/sec, no errors, netstat: 16000 server connections
    // simultaneous close: 48000 connects/sec, no errors, netstat: 8000 server, 8000 client
    // graceful close: 17000 connects/sec, no errors, netstat: 16000 server

    SynchronizationPolicy* client_policy = NULL;
    SynchronizationPolicy* server_policy = NULL;
    Event event_initially_nonsignaled(false, false);
    Event event_initially_signaled(false, true);
    Barrier barrier(2);

    if (!strcmp(argv[1], "any")) {
        client_policy = server_policy = new SynchronizationPolicy();    // policy does nothing
    } else if (!strcmp(argv[1], "client")) {
        // server waits for client to do closesocket(), then client tells server to do closesocket()
        client_policy = new WaitSynchronizationPolicy(&event_initially_signaled, &event_initially_nonsignaled);
        server_policy = new WaitSynchronizationPolicy(&event_initially_nonsignaled, &event_initially_signaled);
    } else if (!strcmp(argv[1], "server")) {
        // client waits for server to do closesocket(), then server tells client to do closesocket()
        client_policy = new WaitSynchronizationPolicy(&event_initially_nonsignaled, &event_initially_signaled);
        server_policy = new WaitSynchronizationPolicy(&event_initially_signaled, &event_initially_nonsignaled);
    } else if (!strcmp(argv[1], "simultaneous")) {
        // client and server try to call closesocket() approximately simultaneously.
        client_policy = new BarrierSynchronizationPolicy(&barrier);
        server_policy = new BarrierSynchronizationPolicy(&barrier);
    } else if (!strcmp(argv[1], "graceful")) {
        client_policy = new GracefulShutdownSynchronizationPolicy();
        server_policy = new SynchronizationPolicy();    // don't do anything special
    } else {
        return Usage(argc, argv);
    }

#ifdef _WIN32
    WSADATA wsaData;
    int result = WSAStartup(MAKEWORD(2, 2), &wsaData);
    if (result != 0) {
        fprintf(stderr, "error: WSAStartup: %d\n", result);
        exit(EXIT_FAILURE);
    }
#endif

    size_t server_connections, client_connections;
    NetStat(&server_connections, &client_connections);
    if (server_connections != 0)
        fprintf(stderr, "There are %zu lingering server connections from 127.0.0.1:%d in netstat.\n", server_connections, PORT);
    if (client_connections != 0)
        fprintf(stderr, "There are %zu lingering client connections to 127.0.0.1:%d in netstat.\n", client_connections, PORT);
    if ((server_connections != 0) || (client_connections != 0)) {
        fprintf(stderr, "Wait a minute and try again.\n");
        exit(EXIT_FAILURE);
    }

    StartThread(TestServerThread, server_policy);
    StartThread(TestClientThread, client_policy);

    while (g_warning_count < MAX_ERROR_COUNT) {
        size_t old_connect_count = g_connect_count;
        Sleep(1000);
        size_t new_connect_count = g_connect_count;
        NetStat(&server_connections, &client_connections);
        printf("connects per second: %zu, server connections: %zu, client connections: %zu\n",
               new_connect_count - old_connect_count, server_connections, client_connections);
    }

    fprintf(stderr, "exiting after %zu errors\n", MAX_ERROR_COUNT);
    NetStat(&server_connections, &client_connections);
    printf("server connections: %zu, client connections: %zu\n",
           server_connections, client_connections);
    exit(EXIT_FAILURE);

    return 0;
}
