#include "getopt.h"
#include <stdlib.h>
#include <stdio.h>
#include <stdbool.h>
#include <stdint.h>
#include <math.h>
#include <string.h>

#ifdef WIN32
#include <winsock2.h>
#include <windows.h>
#include <ws2tcpip.h>

#pragma comment(lib, "Ws2_32.lib")
#endif

#define MAX_SIZE 1500
#define PING_MAGIC 0xF00D6655

#pragma pack(push, 1)
typedef struct {
	int32_t magic;
	int32_t seqno;
	int64_t send_ts;
} phdr;
#pragma pack(pop)

typedef enum {
	MODE_UNSPECIFIED = 0,
	MODE_CLIENT,
	MODE_SERVER
} prog_mode;

typedef struct {
	int port;
	prog_mode mode;

	struct {
		char* server;
		struct in_addr server_addr;
		int num_packets;
		unsigned pkt_size;
		int interval_ms;
	} client;
} prog_args;

typedef struct {
	bool running;
	int64_t min_rtt;
	int64_t max_rtt;
	int64_t tot_rtt;
	int num_pkts;
	SOCKET sock;
	double freq;
	const prog_args* args;
} client_state;

static void usage() {
	printf(
		"Usage: udpping [-s] [-c server] [-p port] [args]\n"
		"\nOptions:\n"
		"  -s                   run as a server\n"
		"  -c server            connect to the given server IP\n"
		"  -p port              specify UDP port (default 6000)\n"
		"\nClient options:\n"
		"  -n packets           number of packets to send (default 4)\n"
		"  -b size              size of the UDP payload (default 64 B)\n"
		"  -i interval_ms       interval for the packets send (default 1000)\n"
	);
}

static bool parse_args(int argc, char **argv, prog_args *args) {
	int c;

	while ((c = getopt(argc, argv, "sc:p:n:b:i:")) != -1) {
		switch (c) {
		case 's':
			args->mode = MODE_SERVER;
			break;
		case 'c':
			args->mode = MODE_CLIENT;
			args->client.server = _strdup(optarg);
			inet_pton(AF_INET, optarg, &args->client.server_addr);
			break;
		case 'p':
			args->port = atoi(optarg);
			break;
		case 'n':
			args->client.num_packets = atoi(optarg);
			break;
		case 'b':
			args->client.pkt_size = atoi(optarg);
			break;
		case 'i':
			args->client.interval_ms = atoi(optarg);
			break;
		default:
			return false;
		}
	}

	if (args->mode == MODE_UNSPECIFIED) {
		puts("-s/-c must be specified");
		return false;
	}

	if ((args->mode == MODE_CLIENT) && (args->client.server_addr.S_un.S_addr == 0)) {
		puts("invalid server address");
		return false;
	}

	if ((args->client.pkt_size < sizeof(phdr)) || (args->client.pkt_size > MAX_SIZE)) {
		puts("invalid packet size");
		return false;
	}

	return true;
}

static inline int64_t GetTicks() {
	LARGE_INTEGER ticks;
	if (!QueryPerformanceCounter(&ticks))
	{
		puts("QueryPerformanceCounter failed");
		return 0;
	}
	return ticks.QuadPart;
}

static bool run_server(prog_args *args) {
	char buffer[MAX_SIZE];
	struct sockaddr_in servaddr = {
		.sin_family = AF_INET,
		.sin_port = htons(args->port),
		.sin_addr.S_un.S_addr = INADDR_ANY,
	}, cliaddr;
	SOCKET sock = socket(AF_INET, SOCK_DGRAM, 0);

	if (sock == INVALID_SOCKET) {
		printf("Socket creation failed\n");
		return false;
	}

	if (bind(sock, (struct sockaddr*)&servaddr, sizeof(servaddr)) == SOCKET_ERROR) {
		printf("Bind failed\n");
		return false;
	}

	while (true) {
		int len = sizeof(cliaddr);
		int n = recvfrom(sock, buffer, MAX_SIZE, 0, (struct sockaddr*)&cliaddr, &len);
		if (n == SOCKET_ERROR) {
			printf("recvfrom failed\n");
			return false;
		}
		n = sendto(sock, buffer, n, 0, (struct sockaddr*)&cliaddr, len);
		if (n == SOCKET_ERROR) {
			printf("sendto failed\n");
			return false;
		}
	}

	return true;
}

DWORD WINAPI ReceiverThread(LPVOID lpParam) {
	char buffer[MAX_SIZE];
	client_state* state = (client_state*) lpParam;
	phdr* hdr = (phdr*)(buffer);

	while (state->running) {
		int n = recv(state->sock, buffer, MAX_SIZE, 0);
		int64_t now = GetTicks();

		if ((n == state->args->client.pkt_size) && (hdr->magic == PING_MAGIC)) {
			// TODO check seq
			int64_t rtt = now - hdr->send_ts;
			if (state->num_pkts == 0) {
				state->min_rtt = rtt;
				state->max_rtt = rtt;
			} else {
				state->min_rtt = min(rtt, state->min_rtt);
				state->max_rtt = max(rtt, state->max_rtt);
			}
			state->tot_rtt += rtt;
			state->num_pkts++;

			printf("Reply from %s: bytes=%u time=%.1fms\n", state->args->client.server,
				state->args->client.pkt_size, rtt / state->freq);
		}
	}

	return 0;
}

static bool run_client(prog_args* args) {
	char buffer[MAX_SIZE];
	phdr* hdr = (phdr*)(buffer);
	SOCKET sock = socket(AF_INET, SOCK_DGRAM, 0);

	if (sock == INVALID_SOCKET) {
		printf("Socket creation failed\n");
		return false;
	}

	struct sockaddr_in servaddr = {
		.sin_family = AF_INET,
		.sin_port = htons(args->port),
		.sin_addr = args->client.server_addr,
	};
	int slen = sizeof(servaddr);

	// ensure that the receiver thread wakes after some time
	DWORD timeoutMs = 100;
	if (setsockopt(sock,
			SOL_SOCKET,
			SO_RCVTIMEO,
			(char*)&timeoutMs,
			sizeof(timeoutMs)) < 0) {
		puts("setsocketopt failed");
		return false;
	}

	client_state state = { .sock = sock, .args = args, .running = true };
	LARGE_INTEGER frequency;
	if (!QueryPerformanceFrequency(&frequency)) {
		puts("QueryPerformanceFrequency failed");
		return false;
	}
	state.freq = (double)frequency.QuadPart / 1e3; // msec

	// start receiver thread
	DWORD threadId;
	HANDLE threadHandle = CreateThread(
		NULL,                   // default security attributes
		0,                      // use default stack size  
		ReceiverThread,			// thread function name
		&state,					// argument to thread function 
		0,                      // use default creation flags 
		&threadId);
	if (threadHandle == NULL) {
		printf("CreateThread failed\n");
		return false;
	}
	if (!SetThreadPriority(threadHandle, THREAD_PRIORITY_HIGHEST))
		printf("SetThreadPriority (receiver) failed\n");

	HANDLE curThread = GetCurrentThread();
	if (!SetThreadPriority(curThread, THREAD_PRIORITY_HIGHEST))
		printf("SetThreadPriority (sender) failed\n");
	CloseHandle(curThread);

	// bind the socket to perform route lookup now
	if (connect(sock, (struct sockaddr*)&servaddr, sizeof(servaddr)) == SOCKET_ERROR) {
		printf("connect failed\n");
		return false;
	}
	Sleep(50);

	int num_sent = 0;
	while (num_sent < args->client.num_packets) {
		hdr->magic = PING_MAGIC;
		hdr->seqno = num_sent;
		hdr->send_ts = GetTicks();
		if (!hdr->send_ts)
			return false;

		int n = send(sock, buffer, args->client.pkt_size, 0);
		if (n == SOCKET_ERROR) {
			printf("Send failed\n");
			return false;
		}

		num_sent++;
		Sleep(args->client.interval_ms);
	}

	// wait some time
	Sleep(500);
	state.running = false;

	WaitForSingleObject(threadHandle, INFINITE);
	CloseHandle(threadHandle);
	closesocket(sock);

	// print stats
	int lost = args->client.num_packets - state.num_pkts;
	printf("Statistics for %s\n\tPackets: Sent = %d, Received = %d, Lost = %d (%d %% loss)\n",
		args->client.server, args->client.num_packets, state.num_pkts, lost,
		(double)lost * 100.0 / args->client.num_packets);

	double avg_rtt = (double)state.tot_rtt / state.num_pkts;
	printf("\tRTT (ms): Min = %.1f, Max = %.1f, Avg = %.1f\n",
		state.min_rtt / state.freq,
		state.max_rtt / state.freq,
		avg_rtt / state.freq);

	return true;
}

int main(int argc, char **argv) {
#ifdef WIN32
	WSADATA wsaData;
#endif

	prog_args args = {
		// defaults
		.port = 6000,
		.client = {
			.num_packets = 4,
			.pkt_size = 64,
			.interval_ms = 1000,
		}
	};

#ifdef WIN32
	if (WSAStartup(MAKEWORD(2, 2), &wsaData)) {
		printf("WSAStartup failed\n");
		return EXIT_FAILURE;
	}
#endif

	if(!parse_args(argc, argv, &args)) {
		usage();
		return EXIT_FAILURE;
	}

	bool rv;
	if (args.mode == MODE_SERVER)
		rv = run_server(&args);
	else
		rv = run_client(&args);

#ifdef WIN32
	WSACleanup();
#endif

	if (args.client.server)
		free(args.client.server);

	return rv ? EXIT_SUCCESS : EXIT_FAILURE;
}
