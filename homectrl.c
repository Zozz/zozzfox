/* -*- Mode: C; indent-tabs-mode: t; c-basic-offset: 4; tab-width: 4 -*- */
/*
 * homectrl.c
 * Copyright (C) Kovács Zoltán 2008-2009 <zozz@freemail.hu>
 * 
 * homectrl.c is free software.
 * 
 * You may redistribute it and/or modify it under the terms of the
 * GNU General Public License, as published by the Free Software
 * Foundation; either version 2 of the License, or (at your option)
 * any later version.
 * 
 * main.c is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.
 * See the GNU General Public License for more details.
 * 
 * You should have received a copy of the GNU General Public License
 * along with homectrl.c.  If not, write to:
 * 	The Free Software Foundation, Inc.,
 * 	51 Franklin Street, Fifth Floor
 * 	Boston, MA  02110-1301, USA.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <time.h>
#include "sys/ioctl.h"
#include "fcntl.h"
#include "asm/etraxgpio.h"
#include <sys/wait.h>
#include <sys/stat.h>
#include <sys/shm.h>

#define ON			1
#define OFF			0
#define CONFIG_FILE "/var/homectrl.cnf"
#define CTRL_PORT	"/dev/gpiog"
#define CH1_ON_BIT	1<<4
#define CH1_OFF_BIT	1<<1
#define CH2_ON_BIT	1<<3
#define CH2_OFF_BIT	1<<5
// rain sensor port A1
#define RS_PORT		"/dev/gpioa"
#define RS_BIT		1<<1

#define ST_START	0
#define ST_ON		1
#define ST_OFF		2
#define ST_IDLE		3
#define ST_DISABLE	4
#define ST_SKIP	5
#define MAX_LINE_LEN 40
#define MIN			*60

#define die(msg) { perror(msg); exit(EXIT_FAILURE); }

/* Configuration parameters */
int SP_ON_TIME = 30 MIN;	// sprinkler on cycle time
int SP_OFF_TIME = 20 MIN;	// pause between sprinkler cycles
int SP_START_HOUR = 19;		// when start sprinkling
int FLT_ON_TIME = 120 MIN;	// how long run pool filter
int FLT_ON_TEMP = 25;		// min temperature to run pool filter
int FLT_START_HOUR = 14;	// when start pool filter
int HEAT_ENABLED = 1;
int SP_ENABLED = 1;
//0000 0000 0000 0000 0000 0000	program bits
//0    4    8    12   16   20	hours
unsigned int prog[]={
	0x00FFFC /*0: 8-21*/,
	0x03FFFC /*1: 6-21*/}; // heating programs
int days[7]={0,1,1,1,1,1,0}; // assign programs to days

struct tm *ptm, utc;
time_t t, sp_last_t;
int fd, fa, sp_st=ST_IDLE, sp_round=1, flt_st=ST_IDLE, ht_corr = 0, sp_freq, precip;
FILE *fc;
float Tmin = 50.0, Tmax = -50.0, Tmax1 = -50.0, temp;
char *wdays[]={"Sun","Mon","Tue","Wed","Thu","Fri","Sat"};

/* generate on/off pulse for remote controller */
static void pulse(int bit)
{
		ioctl(fd, _IO(ETRAXGPIO_IOCTYPE, IO_SETBITS), bit);
		sleep(4);
		ioctl(fd, _IO(ETRAXGPIO_IOCTYPE, IO_CLRBITS), bit);
		sleep(1);
}

/* get external temperature */
static void get_temp(void)
{
	int tmp;
    tmp = system("/mnt/1/temp.sh");
    if(tmp == -1 || WEXITSTATUS(tmp)) return; // error

    if ((fc = fopen ("/var/tmp/temp.val", "r")) == NULL) {
        printf ("Failed to open temp.val\n");
        return;
    }
	fscanf(fc, "%f", &temp); // get the script return value
	fclose(fc);
	if(temp > Tmax) Tmax = temp; // save max temp
	if(temp > Tmax1) Tmax1 = temp; // save max temp for sprinkling
	if(temp < Tmin) Tmin = temp; // save min temp
}

/* read rain sensor state */
static int rain_sensor(void)
{
	int value;
	static int prev_value = -1;
	value = !(ioctl(fa, _IO(ETRAXGPIO_IOCTYPE, IO_READBITS)) & RS_BIT);
	if(value != prev_value){
		printf("%s %02d:%02d Rain sensor %s\n", wdays[ptm->tm_wday], ptm->tm_hour, ptm->tm_min, value ? "on" : "off");
		prev_value = value;
	}
	return value;
}

/* switch heating ON or OFF */
static void heat(int state)
{
	static int heat_status = -1;
	if(state == heat_status) return;
	printf("%s %02d:%02d Heating %s\n", wdays[ptm->tm_wday], ptm->tm_hour, ptm->tm_min, state ? "on" : "off");
	if(state)
		pulse(CH1_ON_BIT);
	else
		pulse(CH1_OFF_BIT);
	heat_status = state;
}

/* control sprinklers */
static void sprinkler(void)
{
	static time_t saved_t;
	static int saved_day;

	switch(sp_st){
	case ST_DISABLE:
		break;
	case ST_SKIP: // skip to next day
		if(ptm->tm_wday == saved_day) break;
	case ST_IDLE:
		/* delay to Saturday if possible */
		if(sp_freq > 2 && ptm->tm_wday == 5){
			break;
		}
		/* check time to start */
		if(difftime(t, sp_last_t) >= sp_freq*60*60*24 && ptm->tm_hour == SP_START_HOUR){
	    	saved_day = ptm->tm_wday;
		    precip = WEXITSTATUS(system("/mnt/1/precip.sh")); // first check precip
		    if(precip > 5){
				sp_st = ST_SKIP;
			}
		    else{
				sp_st = ST_START;
				saved_t = t;
				sp_round = 1;
			}
		}
		break;
	case ST_START: // start cycle
		printf("%s %02d:%02d Sprinkler%d on\n", wdays[ptm->tm_wday], ptm->tm_hour, ptm->tm_min, sp_round);
		pulse(CH2_ON_BIT);
		sp_st = ST_ON;
		if(sp_round == 1) sp_last_t = t; // save last sprinkling time
		break;
	case ST_ON: // wait for sprinkler time
		if(difftime(t, saved_t) >= SP_ON_TIME){
			printf("%s %02d:%02d Sprinkler%d off\n", wdays[ptm->tm_wday], ptm->tm_hour, ptm->tm_min, sp_round);
			pulse(CH2_OFF_BIT);
			sp_st = ST_OFF;
			saved_t += SP_ON_TIME;
		}
		break;
	case ST_OFF: // wait for pause time
		if(difftime(t, saved_t) >= SP_OFF_TIME){
			sp_st = ++sp_round > 6 ? ST_IDLE : ST_START;
			saved_t += SP_OFF_TIME;
		}
		break;
	}
}

/* control filter pump */
static void filter(void)
{
	static time_t saved_t;

	if(ptm->tm_hour == FLT_START_HOUR && flt_st == ST_IDLE && Tmax > FLT_ON_TEMP) flt_st = ST_START;
	switch(flt_st){
	case ST_START: // start cycle
		printf("%s %02d:%02d Filter on\n",wdays[ptm->tm_wday], ptm->tm_hour, ptm->tm_min);
		pulse(CH1_ON_BIT);
		flt_st = ST_ON;
		saved_t = t;
		break;
	case ST_ON: // wait for filter time
		if(difftime(t, saved_t) >= FLT_ON_TIME){
			printf("%s %02d:%02d Filter off\n", wdays[ptm->tm_wday], ptm->tm_hour, ptm->tm_min);
			pulse(CH1_OFF_BIT);
			flt_st = ST_IDLE;
		}
		break;
	case ST_IDLE: // idle
		break;
	}
}

static void corrections(void)
{
	// correct heating time: lower temperature -> earlier ON time
	ht_corr = -3 * temp + 15;
	if(ht_corr < 0) ht_corr = 0;

	// correct sprinkling frequency: higher temperature -> frequent sprinkling
	sp_freq = -0.4 * Tmax1 + 13.0 + 0.5; // 20C -> 5 day, 30C -> 1 day, rounded
	if(sp_freq < 0) sp_freq = 0;

	// get rain sensor signal and set sp_last_t
	if(rain_sensor()){
		sp_last_t = t; // set last sprinkling time
		if(sp_st == ST_ON){ // stop sprinkling
			pulse(CH2_OFF_BIT);
		}
		sp_st = ST_SKIP;
	}
}

/* read and parse configuration file */
static void parse_file (const char *filename)
{
    FILE *config_fp;
    char  buffer[MAX_LINE_LEN + 1];
    char *token;
    int   line, i;

    if ((config_fp = fopen (filename, "r")) == NULL) {
        printf ("Failed to open file \"%s\". Skipping ...\n", filename);
        return;
    }
    line = 1;
    while (fgets (buffer, MAX_LINE_LEN, config_fp) != NULL) {
        token = strtok (buffer, "\t =\n\r");

        if (token != NULL && *token != '#') {
            char name[MAX_LINE_LEN];

            strcpy (name, token);
            token = strtok (NULL, "\t =\n\r");

            if (token != NULL) {
                if(strcmp(name, "SP_ENABLED") == 0)
                	SP_ENABLED = atoi(token);
                else if(strcmp(name, "SP_ON_TIME") == 0)
                	SP_ON_TIME = atoi(token) * 60;
                else if(strcmp(name, "SP_OFF_TIME") == 0)
                	SP_OFF_TIME = atoi(token) * 60;
                else if(strcmp(name, "SP_START_HOUR") == 0)
                	SP_START_HOUR = atoi(token);
                else if(strcmp(name, "sp_last_t") == 0){
                	memset(&utc, 0, sizeof(utc));
                	sscanf(token, "%d/%d", &utc.tm_mon, &utc.tm_mday);
                	utc.tm_mon--;
                	utc.tm_year = ptm->tm_year;
                	sp_last_t = mktime(&utc);
				}
                else if(strcmp(name, "FLT_ON_TIME") == 0)
                	FLT_ON_TIME = atoi(token) * 60;
                else if(strcmp(name, "FLT_ON_TEMP") == 0)
                	FLT_ON_TEMP = atoi(token);
                else if(strcmp(name, "FLT_START_HOUR") == 0)
                	FLT_START_HOUR = atoi(token);
                else if(strcmp(name, "HEAT_ENABLED") == 0)
                	HEAT_ENABLED = atoi(token);
                else if(strcmp(name, "HEAT_Program") == 0)
                	for(i=0; i<7; i++, token += 2)
                		days[i] = atoi(token);
            }
            else {
                printf ("Error in line %d: value expected\n", line);
            }
        }

        ++line;
    }
}

int main(void)
{
	time_t last_get_temp = 0, last_check_cnf = 0;
	struct stat cfst;
	int T_flag, time_flag = 0, p_flag = 1, n=0;
	int shmid;
	char *shm;
	
	setlinebuf(stdout); // for correct logging to file
	t = time(NULL);
	ptm = localtime(&t);
	T_flag = 1;
	while(ptm->tm_year == 70){	// wait for get time
    	sleep(60 * T_flag);
    	if(T_flag % 2)
    		system("/mnt/1/gettime.sh");
    	else
    		system("/mnt/1/gettime1.sh");	// try another time server
    	sleep(10);
		t = time(NULL);
		ptm = localtime(&t);
		T_flag++;
		printf("%d\n",T_flag);
	}

	printf("HomeControl V2.0 - Starting on %d/%d/%d (%d)\n", ptm->tm_year+1900, ptm->tm_mon+1, ptm->tm_mday, T_flag);

	T_flag = 0; // do not log T on startup
	
	/* create shared memory for Web communication */
	if((shmid = shmget(1962, 200, IPC_CREAT | 0666)) < 0) die("shmget");
	if((shm = shmat(shmid, NULL, 0)) == (char *) -1) die("shmat");

	if((fd = open(CTRL_PORT, O_RDWR)) < 0) die("open CTRL_PORT");
	if((fa = open(RS_PORT, O_RDWR)) < 0) die("open RS_PORT");
	sp_last_t = t;
	precip = WEXITSTATUS(system("/mnt/1/precip.sh"));

	// main loop
	while(1){
		/* check if config file modified */
		if(stat(CONFIG_FILE, &cfst) != 0){
			// no config file, create it
			if((fc = fopen(CONFIG_FILE, "w")) == NULL){
		    	printf("Open error on config file\n");
		    }
		    else{
			    fprintf(fc,"    SP_ENABLED = %d\n", SP_ENABLED);
			    fprintf(fc,"    SP_ON_TIME = %d min\n", SP_ON_TIME/60);
			    fprintf(fc,"   SP_OFF_TIME = %d min\n", SP_OFF_TIME/60);
			    fprintf(fc," SP_START_HOUR = %d\n", SP_START_HOUR);
				ptm = localtime(&sp_last_t);
		    	fprintf(fc,"     sp_last_t = %d/%d\n", ptm->tm_mon+1, ptm->tm_mday);
			    fprintf(fc,"   FLT_ON_TIME = %d min\n", FLT_ON_TIME/60);
			    fprintf(fc,"   FLT_ON_TEMP = %d C\n", FLT_ON_TEMP);
			    fprintf(fc,"FLT_START_HOUR = %d\n", FLT_START_HOUR);
			    fprintf(fc,"  HEAT_ENABLED = %d\n", HEAT_ENABLED);
			    fprintf(fc,"  HEAT_Program = %d,%d,%d,%d,%d,%d,%d\n",days[0],days[1],days[2],days[3],days[4],days[5],days[6]);
				fclose(fc);
			}
		}
		else if(cfst.st_mtime > last_check_cnf){
			parse_file(CONFIG_FILE);
			last_check_cnf = cfst.st_mtime;
		}

		t = time(NULL);
		ptm = localtime(&t);

		/* update system time on every Sunday */
		if(ptm->tm_wday == 0){
			if(time_flag){
				system("/mnt/1/gettime.sh");
				time_flag = 0;
			}
		}
		else
			time_flag = 1;

		/* get temperature every 15 minutes */
		if(difftime(t, last_get_temp) >= 15 MIN){
			get_temp();
			last_get_temp = t;
		}

		corrections(); // must do before reset Tmax!

		/* Log and reset Tmin, Tmax */
		gmtime_r(&t, &utc);
		switch(utc.tm_hour){
		case 18:	// 18 UTC: reset for log
			if(T_flag){ // do it only once
				T_flag = 0;
				if((fc = fopen("/var/T.dat", "a")) == NULL){
			    	printf("Open error on data file\n");
			    }
			    else{
			    	fprintf(fc, "%d/%d:\tTmin=%3.1f\tTmax=%3.1f\n", ptm->tm_mon+1, ptm->tm_mday, Tmin, Tmax);
			    	fclose(fc);
			    }
				Tmin = Tmax = temp;	// reset Tmin, Tmax
			}
			break;
		case 6:		// 6 UTC: reset for sprinkling
			if(T_flag){ // do it only once
				T_flag = 0;
				Tmax1 = temp;	// reset Tmax1
			}
			break;
		default:
			T_flag = 1;
		}

		/* get precipitation for next day */
		switch(ptm->tm_hour){
		case 6:
			if(p_flag){ // do it only once
				p_flag = 0;
	    		precip = WEXITSTATUS(system("/mnt/1/precip.sh"));
	    	}
	    	break;
		default:
			p_flag = 1;
		}

		/* control heating */
		if(HEAT_ENABLED){
			if((prog[days[ptm->tm_wday]] >> (23-ptm->tm_hour)) & 1)
				heat(ON);
			else{
				int next_hour = ptm->tm_hour + 1;
				if(next_hour > 23) next_hour = 0; // TODO inc wday?
				if((prog[days[ptm->tm_wday]] >> (23-next_hour)) & 1){
					if(ptm->tm_min >= 60-ht_corr) heat(ON); // heat on earlier than in the program
				}
				else heat(OFF);
			}
		}
		else{
			heat(OFF);
			filter(); // control pool filter
		}

		if(SP_ENABLED) sprinkler(); // control sprinkler system

		/* write status information */
		if((fc = fopen("/var/homectrl.stat", "w")) == NULL){
	    	printf("Open error on status file\n");
	    }
	    else{
			ptm = localtime(&sp_last_t);
	    	fprintf(fc,"sp_last_t = %d/%d\n",ptm->tm_mon+1,ptm->tm_mday);
	    	fprintf(fc,"  sp_freq = %d day%c\n",sp_freq, sp_freq > 1 ? 's' : ' ');
	    	fprintf(fc,"  ht_corr = %d min\n",ht_corr);
	    	fprintf(fc,"     Temp = %3.1f\n",temp);
	    	fprintf(fc,"     Tmin = %3.1f\n",Tmin);
	    	fprintf(fc,"     Tmax = %3.1f\n",Tmax);
	    	fprintf(fc,"    Tmax1 = %3.1f\n",Tmax1);
	    	fprintf(fc,"Est. prec = %d mm\n",precip);
	    	fclose(fc);
	    }

		/* send all data to the web server in JSON format*/
		sprintf(shm, "{\"id\":%d, \"sp\":%d, \"sp_st\":%d, \"temp\":%3.1f}", ++n, sp_round, sp_st, temp);

		sleep(30);
	}
	close(fd);
	printf("Exiting...\n");
	return (0);
}

