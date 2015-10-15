SocketCloseTest
===============

This program reproduces two socket networking issues on Windows:

1. If you repeatedly make a TCP socket connection to the loopback interface
   (127.0.0.1), `connect()` will eventually return the error `WSAEADDRINUSE`
   (10048), "Only one usage of each socket address (protocol/network
   address/port) is normally permitted.".

   This error does not occur if the server closes its socket before the client
   closes its socket.

2. If you repeatedly make a TCP socket connection to the loopback interface
   (127.0.0.1), and you wait for graceful shutdown by calling `recv()` until
   it returns zero, `recv()` will occasionally return the error `WSAECONNRESET`
   (10054), "An existing connection was forcibly closed by the remote host.".

These issues do not occur on Linux. (This program is written to compile on
Windows and Linux.)

How to compile
--------------

- On Linux, run `make`.
- On Windows, use the Visual Studio project.

Command Line Options
--------------------

To reproduce issue #1, run any of the following commands:

- `socketclosetest any`: Don't enforce any specific socket close ordering,
  just connect and close as fast as possible.

- `socketclosetest client`: Have the client close its socket before the server
  closes its socket.

- `socketclosetest simultaneous`: Try to have the client and server close their
  sockets at approximately the same time.

For issue #1, to see that closing the server socket before closing the client
socket does not produce errors, run:

- `socketclosetest server`: Have the server close its socket before the client
  closes its socket.

To reproduce issue #2, run:

- `socketclosetest graceful`: The client will call recv() until it returns zero
  (suggesting that the server has closed its socket), then the client will
  close its socket. It may take 30-60 seconds to encounter an `WSAECONNRESET`
  error from `recv()`.

Results
-------

| Command                      | Windows 10               | Linux (Ubuntu 14.04 LTS)
|------------------------------|--------------------------|-------------------------
| socketclosetest any          | WSAEADDRINUSE            | ok
| socketclosetest client       | WSAEADDRINUSE            | ok
| socketclosetest simultaneous | WSAEADDRINUSE            | ok
| socketclosetest server       | ok                       | ok
| socketclosetest graceful     | occasional WSAECONNRESET | ok

What might be going on?
-----------------------

For issue #1:

Given a TCP socket client and server, the half of the connection that calls
closesocket() first does an "active close" and enters the TIME_WAIT state, and
the other half gets closed immediately (unless it also did an active close
simultaneously).

When the client closes first, the client's half of the connection remains in
TIME_WAIT, so if we do this repeatedly, we run out of free unique pairs of
{address/server port, address/client port}.

When the server closes first, the client's half is closed immediately, but the
server's half remains in TIME_WAIT. This sounds like it would also run out of
free unique pairs of {address/server port, address/client port}, but Windows
has a (standards permitted) optimization that allows a new connection to be
established even though the server socket is in TIME_WAIT:

http://blogs.technet.com/b/networking/archive/2010/08/11/how-tcp-time-wait-assassination-works.aspx

For issue #2:

I don't know, but when `recv()` encounters an error during graceful shutdown,
the program outputs the port number and a timestamp so that the error can be
correlated with a packet trace for further investigation.

Are these bugs in Windows or bugs/features in Linux?
----------------------------------------------------

I don't know.

What is the impact of this issue?
---------------------------------

It makes porting code from Unix a real pain because the original, simple code
works fine on Linux, but it has to be changed to avoid this issue on Windows.

I ran into this issue while working on the Android Debug Bridge (adb) which
runs on Unix and Windows.

What is the work-around?
------------------------

If the server is waiting for a command from the client, the client should send
a command to the server to tell the server to close the connection.

Then the client should wait for graceful socket shutdown and ignore recv()
errors in the process:

```
// Note: do not call shutdown(SD_SEND) here because that will have the effect
// of half-closing the client socket before the server socket which will
// cause the WSAEADDRINUSE errors that we're trying to avoid in the first place.

// Call recv() until it returns an error or 0, indicating that orderly/graceful
// shutdown has occurred and that closesocket() has occurred on the server.
// This will silently discard any data that hasn't already been read by the
// client.
char buf[1024];
int result;
do {
	result = recv(socket, buf, sizeof(buf), 0);
} while (result != SOCKET_ERROR && result != 0);

// Ignore recv() errors here because sometimes WSAGetLastError() may return
// WSAECONNRESET here.

// Finally close the socket
closesocket(socket);
```

About the source code
---------------------

The program starts up two threads:

- TestClientThread: Constantly tries to connect() to 127.0.0.1:40000 and when
  it succeeds, calls closesocket().
- TestServerThread: Listens on 127.0.0.1:40000, and repeatedly calls accept()
  and when it succeeds, calls closesocket().

The various socket close ordering strategies are implemented by classes
derived from SynchronizationPolicy.

The NetStat() function runs netstat and parses the output to count the number
of half-connections (server or client).
