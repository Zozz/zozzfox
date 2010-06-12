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

/* WMRS data structures - must be same as in wmrs200log.c! */
struct sensor_t{
	float temp;
	int rh;
	char sBatt;
};
struct wmrs_t{
	struct sensor_t s[2];
	int relP;
	int absP;
	float wind;
	float gust;
	int windDir;
	char wBatt;
	float prec, prec1, prec24, precTot;
	char pBatt;
} *w;
static char dt[20], key[33];

/* Convert string to uppercase */
static char *strupr(char *s)
{
	char *p = s;

	while(*s){
		*s = toupper(*s);
		s++;
	}
	return p;
}

int main(void)
{
	int shmid;
	FILE *fc;
    time_t t;
    struct tm *tmp;
    char keygen[] = "echo -n 'ZWMRSCE323B11B2E18FC9C1DB1DA870BACAA82010-05-29 21:14:10' | md5sum >/var/key"; //sector
    char *dtpos = strstr(keygen, "2010-");	// position of date/time

	/* create shared memory for WMRS communication */
	if((shmid = shmget(1962, sizeof(struct wmrs_t), IPC_CREAT | 0666)) < 0) exit(EXIT_FAILURE);
	if((w = shmat(shmid, NULL, SHM_RDONLY)) == (void *)-1) exit(EXIT_FAILURE);

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
	puts("POST /code/api/obs_auto.php HTTP/1.1\r");
	puts("Host: metnet.hu\r");
	puts("Content-Type: multipart/form-data; boundary=--------06j410213928046\r");
	puts("Content-Length: 809\r\n\r");

	puts("----------06j410213928046\r");
	puts("Content-Disposition: form-data; name=\"xmlfile\"; filename=\"wsdata.xml\"\r");
	puts("Content-Type: text/xml\r\n\r");

	puts("<?xml version=\"1.0\" encoding=\"ISO-8859-2\" standalone=\"yes\"?>\r");
	puts("    <?generator program=\"WMRS200log 1.0\"?>\r");
	puts("    <?verzio verzio=\"1.0\" ?>\r");
	puts("    <adatok>\r");
	puts("       <muszer>ZWMRS</muszer>\r");
	printf("       <kulcs>%s</kulcs>\r\n", strupr(key));
	printf("       <meres>%s</meres>\r\n", dt);
	printf("       <homerseklet>%5.1f</homerseklet>\r\n", w->s[1].temp);
	printf("       <relativlegnyomas>%4d</relativlegnyomas>\r\n", w->relP);
	printf("       <legnedvesseg>%3d</legnedvesseg>\r\n", w->s[1].rh);
	printf("       <szelsebesseg>%5.1f</szelsebesseg>\r\n", w->wind);
	printf("       <szelirany>%3d</szelirany>\r\n", w->windDir);
	printf("       <szellokes>%5.1f</szellokes>\r\n", w->gust);
	printf("       <csapadek1ora>%5.1f</csapadek1ora>\r\n", w->prec1);
	printf("       <csapadek24ora>%5.1f</csapadek24ora>\r\n", w->prec24);
	printf("       <csapadekosszes>%7.1f</csapadekosszes>\r\n", w->precTot);
	puts("    </adatok>\r");
	puts("----------06j410213928046--\r");

    exit(EXIT_SUCCESS);
}
/* SDG */
