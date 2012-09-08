/*
 * udPerf - A simple tool for creating loss and delay traces
 * for a UDP connection
 *
 * Schier Michael, April 2011
 */

#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <time.h>
#include <netdb.h>
#include <signal.h>
#include <err.h>
#include <errno.h>

#include <sys/types.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <sys/resource.h>

#include <iwlib.h>

#define PORT 43197
#define PACKET_LEN 150
#define NOTIFY_INTERVAL 0.5

int sock;
int iw_sock;
FILE* log_file = NULL;

// ====================================================================================================================

/*
 * returns nanoseconds
 */
unsigned long long int curtimei()
{
        static struct timespec arg;
		clock_gettime(CLOCK_REALTIME, &arg);
		unsigned long long int v = arg.tv_nsec;
		v += arg.tv_sec * 1000000000;
		return v;
}

/*
 * returns seconds
 */
double curtimef()
{
        static struct timespec arg;
		clock_gettime(CLOCK_REALTIME, &arg);
        return arg.tv_sec + 0.000000001 * arg.tv_nsec;
}

// ====================================================================================================================

void waittime(double sleep_time)
{
	if (sleep_time < 0)
		return;
	
	struct timespec t;
	t.tv_sec = sleep_time;
	t.tv_nsec = (long)(sleep_time*1000000) % 1000000 * 1000;
	if (nanosleep(&t, NULL))
		perror("error when sleeping");
}

// ====================================================================================================================

/*
 * send data at speed Byte/s for duration secs.
 */
void sender(in_addr_t ip, double rate, double duration)
{
	printf("Started udperf in sender mode...\n");

	struct sockaddr_in addr;
	sock = socket(PF_INET, SOCK_DGRAM, 0);
	int buf_size = 0;
	if (setsockopt(sock, SOL_SOCKET, SO_SNDBUF, &buf_size, sizeof(buf_size)))
		err(errno, "Could not set sockopt!");
	printf("%d\n", buf_size);
	bzero(&addr, sizeof(addr));
	addr.sin_family = AF_INET;
	addr.sin_port = htons(PORT);
	addr.sin_addr.s_addr = ip;

	char data[PACKET_LEN];

	double time_start = curtimef();
	double time_end = time_start + duration;
	double time_last_notify = curtimef();
	double time_emit_in_interval = 0;
	
	unsigned long pack_cnt = 0;
	unsigned long pack_cnt_in_interval = 0;
	long packid = 1;

	printf("        Time       Send-Rate       Emit-Rate  P-Sent\n");
	printf("====================================================\n");
	while (curtimef() < time_end)
	{
		sprintf(data, "%ld\n%ld\n", packid++, (long)(curtimef()*1000000));
		double time_before = curtimef();
		if (sendto(sock, data, PACKET_LEN, 0, (struct sockaddr*)&addr, sizeof(addr)) != PACKET_LEN)
			err(EIO, "Could not emit packet!");
		time_emit_in_interval += curtimef() - time_before;
		pack_cnt++;
		pack_cnt_in_interval++;
		double time_passed = curtimef() - time_start;

		if (time_last_notify + NOTIFY_INTERVAL < curtimef())
		{
			double time_interval = curtimef() - time_last_notify;
			double rate_int = 0.000008 * PACKET_LEN * pack_cnt_in_interval / time_interval;
			double rate_max = 0.000008 * PACKET_LEN * pack_cnt_in_interval / time_emit_in_interval;
			printf("  %6.3f sec  %7.3f Mbit/s  %7.3f Mbit/s %7lu\n",
				time_passed, rate_int, rate_max, pack_cnt_in_interval);

			time_emit_in_interval = 0;
			pack_cnt_in_interval = 0;
			time_last_notify = curtimef();
		}

		// we have to wait for some time to send at the target rate
		double wait_time = PACKET_LEN * (pack_cnt+1) / rate - (curtimef() - time_start);
		waittime(wait_time);
	}

	close(sock);
}

// ====================================================================================================================

void receiver()
{
	printf("Started udperf in receiver mode...\n");

	struct sockaddr_in addr;
	sock = socket(PF_INET, SOCK_DGRAM, 0);
	bzero(&addr, sizeof(addr));
	addr.sin_family = AF_INET;
	addr.sin_port = htons(PORT);
	addr.sin_addr.s_addr = INADDR_ANY;
	if ( bind(sock, (struct sockaddr*)&addr, sizeof(addr)) != 0 )
		err(errno, "Could not bind the socket!");

	iw_sock = iw_sockets_open();
	if (iw_sock < 0)
		err(EIO, "Couldn't get iw socket file descriptor");

	char recv_buf[1500];
	unsigned long packid_newest = -1;
	unsigned long packid;
	double time_pack1;
	double time_delay_interval;
	double time_last_notify;
	long pack_cnt;
	long pack_cnt_in_interval;
	long pack_cnt_lost;
	long pack_cnt_lost_in_interval;
	while (1)
	{
		socklen_t addr_len=sizeof(addr);
		ssize_t bytes = recvfrom(sock, recv_buf, sizeof(recv_buf), 0, (struct sockaddr*)&addr, &addr_len);
		if (bytes != PACKET_LEN)
			err(EIO, "The packet could only be partially obtained!");

		unsigned long time_send;
		sscanf(recv_buf, "%li\n%li\n", &packid, &time_send);
		pack_cnt_in_interval++;
		
		if (packid == 1 || packid_newest == -1)
		{
			if (log_file)
				fclose(log_file);
			log_file = fopen("udperf.log", "w");
			packid_newest = packid-1;
			time_delay_interval = 0;
			time_pack1 = curtimef();
			time_last_notify = time_pack1;
			pack_cnt = 1;
			pack_cnt_lost = 0;
			pack_cnt_in_interval = 1;
			pack_cnt_lost_in_interval = 0;
			printf("          Rate      Loss       Delay         avgRate    avgLoss  sigLevel   sigQual\n");
			printf("===================================================================================\n");
		}

		if (packid <= packid_newest) // out of order reception
		{
			pack_cnt_lost_in_interval--;
			printf("delayed %ld\n", packid);
		}
		else
		{
			pack_cnt_lost_in_interval += packid - packid_newest - 1;
			if(packid > packid_newest+1)
				printf("lost %ld - %ld\n", packid_newest+1, packid-1);
			packid_newest = packid;
		}

		double time_delay = curtimef() - time_send * 0.000001;
		time_delay_interval += time_delay;

		iwstats stats;
		iw_get_stats(iw_sock, "wlan0", &stats, NULL, 0);

		double time_passed = curtimef() - time_pack1;
		double time_interval = curtimef() - time_last_notify;
		if (time_interval >= NOTIFY_INTERVAL)
		{
			pack_cnt += pack_cnt_in_interval;
			pack_cnt_lost += pack_cnt_lost_in_interval;

			double rate_int = 0.000008 * PACKET_LEN * pack_cnt_in_interval / time_interval;
			double loss_int = 100.0 * pack_cnt_lost_in_interval / pack_cnt_in_interval;
			double delay_int = 1000.0 * time_delay_interval / pack_cnt_in_interval;
			double rate_avg = 0.000008 * PACKET_LEN * pack_cnt / time_passed;
			double loss_avg = 100.0 * pack_cnt_lost / pack_cnt;
			
			printf("%7.3f Mbit/s  %7.3f%%  %7.3f ms  %7.3f MBit/s  %7.3f%%  %6hi dB  %5hhu/70\n",
				rate_int, loss_int, delay_int, rate_avg, loss_avg, stats.qual.level - 256, stats.qual.qual);

			time_delay_interval = 0;
			pack_cnt_in_interval = 0;
			pack_cnt_lost_in_interval = 0;
			time_last_notify = curtimef();
		}

		fprintf(log_file, "R %ld %f %ld %hi %hhu\n", packid, time_delay, pack_cnt_lost, stats.qual.level - 256, stats.qual.qual);

	}
	close(sock);
}

// ====================================================================================================================

void sig_int_handler(int sig)
{
	signal(sig, SIG_IGN);
	printf("Quitting.\n");
	iw_sockets_close(iw_sock);
	close(sock);
	if (log_file)
		fclose(log_file);
	exit(0);
}

// ====================================================================================================================

void print_usage(char* name)
{
	printf("Usage: %s -s                      receiver mode\n", name);
	printf("       %s -c <ip> <kByte/s> <s>   sender mode\n", name);
	printf("   The sender sends for a certain period with a constant rate\n");
	exit(-1);
}

// ====================================================================================================================

int main(int argc, char** argv)
{
	signal(SIGINT, sig_int_handler);
	if (setpriority(PRIO_PROCESS, 0 ,-20) )
		err(EPERM, "Couldn't set the process priority!");
	
	if (argc == 5 && strncmp(argv[1], "-c", 2) == 0)
	{
		struct addrinfo *addr_info;
		struct addrinfo hints;
		memset(&hints, 0, sizeof(hints));
		hints.ai_socktype = SOCK_DGRAM;
		hints.ai_family = AF_INET;
		int retcode = getaddrinfo(argv[2], NULL, &hints, &addr_info);
		if (retcode)
			err(EIO, "error in getaddrinfo: %s\n", gai_strerror(retcode));

		in_addr_t target = ((struct sockaddr_in*)addr_info->ai_addr)->sin_addr.s_addr;;
		freeaddrinfo(addr_info);

		sender(target, atof(argv[3]) * 1000, atof(argv[4]));
	}
	else if (argc == 2 && strncmp(argv[1], "-s", 2) == 0)
		receiver();
	else
		print_usage(argv[0]);

	return 0;
}
