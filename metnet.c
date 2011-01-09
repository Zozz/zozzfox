/*
 * metnet.c
 *
 *  Created on: May 29, 2010
 *      Author: zoli
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <unistd.h>
#include <time.h>
#include <sys/shm.h>
#include <sys/socket.h>
#include <arpa/inet.h>
#include <syslog.h>
#include <signal.h>
#include <netdb.h>

#include "wmrs200log.h"

#define STRLEN_MSG 1018	// length of whole message

static wmrs_t *w;
static char dt[20], key[33];
static char msg[1023] =
		"POST /code/api/obs_auto.php HTTP/1.1\r\n"
		"Host: metnet.hu\r\n"
		"Content-Type: multipart/form-data; boundary=--------06j410213928046\r\n"
		"Content-Length: 870\r\n\r\n"
		"----------06j410213928046\r\n"
		"Content-Disposition: form-data; name=\"xmlfile\"; filename=\"wsdata.xml\"\r\n"
		"Content-Type: text/xml\r\n\r\n"

		"<?xml version=\"1.0\" encoding=\"ISO-8859-2\" standalone=\"yes\"?>\r\n"
		"    <?generator program=\"WMRS200log 1.0\"?>\r\n"
		"    <?verzio verzio=\"1.0\" ?>\r\n"
		"    <adatok>\r\n"
		"       <muszer>ZWMRS</muszer>\r\n"
;

/* Convert string to uppercase */
static void strupr(char *s)
{
	while(*s){
		*s = toupper(*s);
		s++;
	}
	return;
}

// free resources on exit
static void cleanup(int dummy)
{
	shmdt(w);
    exit(EXIT_SUCCESS);
}

int main(int argc, char *argv[])
{
	unsigned int interval = 300;	// update interval in seconds
	const int hlen = strlen(msg);	// length of static part
	int shmid, sd, pos;
	FILE *fc;
    time_t t;
    struct tm *tmp;
    struct sockaddr_in saddr;
    struct hostent *host;
    char keygen[] = "echo -n 'ZWMRSCE323B11B2E18FC9C1DB1DA870BACAA82010-05-29 21:14:10' | md5sum >/var/key"; //sector
    char * const dtpos = strstr(keygen, "2010-");	// position of date/time

    openlog(argv[0], 0, 0);
    syslog(LOG_INFO, "starting");

    if(argc > 1){
    	if(sscanf(argv[1], "%u", &interval) != 1){
    		printf("Usage: %s [n]\n  n: update interval in seconds\n", argv[0]);
    	    exit(EXIT_SUCCESS);
    	}
    	if(interval < 60) interval = 60;
    }

	/* read the station identifier */
	if((fc = fopen("/mnt/1/statid", "r")) != NULL){
		fgets(&keygen[9], 38, fc);
		fclose(fc);
	}
	else{
		syslog(LOG_ERR, "statid: %m");
		exit(EXIT_FAILURE);
	}

	/* create shared memory for WMRS communication */
	if((shmid = shmget(1962, sizeof(wmrs_t), IPC_CREAT | 0666)) < 0){
		syslog(LOG_ERR, "shmget: %m");
		exit(EXIT_FAILURE);
	}
	if((w = shmat(shmid, NULL, SHM_RDONLY)) == (void *)-1){
		syslog(LOG_ERR, "shmat: %m");
		exit(EXIT_FAILURE);
	}

	signal(SIGTERM, cleanup);

	for(; ; sleep(interval)){
		/* connect to metnet.hu */
		if((sd = socket(AF_INET, SOCK_STREAM, 0)) < 0){
			syslog(LOG_ERR, "socket: %m");
			continue;
		}
		memset(&saddr, 0, sizeof(saddr));	// clear struct
		saddr.sin_family = AF_INET;
		saddr.sin_port = htons(80);
		host = gethostbyname("metnet.hu");
		if(host != NULL){
			char *s, *d;
			s = host->h_addr_list[0];
			d = (char *)&saddr.sin_addr;
			for(pos = 0; pos < sizeof(saddr.sin_addr); pos++){
				*d++ = *s++;
			}
		}
		if(connect(sd, (struct sockaddr *)&saddr, sizeof(saddr)) < 0){
			syslog(LOG_ERR, "connect: %m");
			close(sd);
			continue;
		}

		/* generate key */
		t = time(NULL);
		tmp = localtime(&t);
		strftime(dt, sizeof(dt), "%F %T", tmp);
		memcpy(dtpos, dt, 19);	// replace date/time part
		system(keygen);
		key[0] = '\0';
		if((fc = fopen("/var/key", "r")) != NULL){
			fgets(key, 33, fc);
			fclose(fc);
		}
		strupr(key);

		/* build variable part of message */
		pos = hlen;	// end of static part
		pos += sprintf(&msg[pos], "       <kulcs>%s</kulcs>\r\n", key);
		pos += sprintf(&msg[pos], "       <meres>%s</meres>\r\n", dt);
		pos += sprintf(&msg[pos], "       <homerseklet>%5.1f</homerseklet>\r\n", w->s[1].temp);
		pos += sprintf(&msg[pos], "       <relativlegnyomas>%4d</relativlegnyomas>\r\n", w->relP);
		pos += sprintf(&msg[pos], "       <legnedvesseg>%3d</legnedvesseg>\r\n", w->s[1].rh);
		pos += sprintf(&msg[pos], "       <harmatpont>%5.1f</harmatpont>\r\n", w->s[1].dew);
		pos += sprintf(&msg[pos], "       <szelsebesseg>%5.1f</szelsebesseg>\r\n", w->wind);
		pos += sprintf(&msg[pos], "       <szelirany>%3d</szelirany>\r\n", w->windDir);
		pos += sprintf(&msg[pos], "       <szellokes>%5.1f</szellokes>\r\n", w->gust);
		pos += sprintf(&msg[pos], "       <csapadek1ora>%5.1f</csapadek1ora>\r\n", w->prec1);
		pos += sprintf(&msg[pos], "       <csapadek24ora>%5.1f</csapadek24ora>\r\n", w->prec24);
		pos += sprintf(&msg[pos], "       <csapadekosszes>%7.1f</csapadekosszes>\r\n", w->precTot);
		strcpy(&msg[pos], "    </adatok>\r\n----------06j410213928046--\r\n");
//		printf("%d\n", strlen(msg));	// print whole length

		if(send(sd, msg, STRLEN_MSG, 0) < 0){
			syslog(LOG_ERR, "send: %m");
		}

		while(recv(sd, &msg[hlen], 512, 0) > 1){
//			puts(&msg[hlen]);
		}
		close(sd);
	}
}
/* SDG */
