/*
 * homectrl.c
 * Copyright (C) Kovács Zoltán 2008-2012 <zozz@freemail.hu>
 * 
 * homectrl.c is free software.
 * 
 * You may redistribute it and/or modify it under the terms of the
 * GNU General Public License, as published by the Free Software
 * Foundation; either version 2 of the License, or (at your option)
 * any later version.
 * 
 * homectrl.c is distributed in the hope that it will be useful,
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

#include "wmrs200log.h"

#define ON			1
#define OFF			0
#define CONFIG_FILE "/var/homectrl.cnf"
#define EXCEPT_FILE "/mnt/1/exceptions.dat"
#define CTRL_PORT	"/dev/gpiog"
#define CH1_ON_BIT	1<<4
#define CH1_OFF_BIT	1<<1
#define CH2_ON_BIT	1<<3
#define CH2_OFF_BIT	1<<5

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
int FLT_ON_TEMP = 30;		// min temperature to run pool filter
int FLT_START_HOUR = 14;	// when start pool filter
int HEAT_ENABLED = 1;
int SP_ENABLED = 1;
int sp_time_corr[6] = {0, 0, 0, 10 MIN, 0, 0}; // sprinkler time correction

// heating programs
//0000 0000 0000 0000 0000 0000	program bits
//0    4    8    12   16   20	hours
unsigned int prog[]={
	0x00FFFC /*0: 8-21 holiday*/,
	0x03FFFC /*1: 6-21 workday*/,
	0x00FFFF /*2: 8-23 party time*/,
    0x07FFFC /*3: 5-21 early get up*/};
int days[7]={0,1,1,1,1,1,0}; // assign programs to days

struct tm *ptm, utc;
time_t t, sp_last_t;
int fd, fa, sp_st=ST_IDLE, sp_round=1, flt_st=ST_IDLE, ht_corr = 0, sp_freq, precip;
FILE *fc;
float Tmin = 50.0, Tmax = -50.0, Tmax1 = -50.0, precLast;
char *wdays[]={"Sun","Mon","Tue","Wed","Thu","Fri","Sat"};
wmrs_t *wmrs;

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
	float temp;

	temp = wmrs->s[1].temp;
	if(temp > Tmax) Tmax = temp; 	// save max temp
	if(temp > Tmax1) Tmax1 = temp; 	// save max temp for watering
	if(temp < Tmin) Tmin = temp;	// save min temp
}

/* read rain sensor state */
static int rain_sensor(void)
{
	int value;
	static int prev_value = -1;

	value = wmrs->prec24 > 5;
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
		/* forced sprinkling */
		if(SP_ENABLED == 2 && ptm->tm_hour == SP_START_HOUR){
			SP_ENABLED = 1; // reset to normal state
			sp_st = ST_START;
			saved_t = t;
			sp_round = 1;
			break;
		}
		/* delay to Saturday if possible */
		if(sp_freq > 2 && ptm->tm_wday == 5){
			break;
		}
		/* check time to start */
		if(difftime(t, sp_last_t) >= sp_freq*60*60*24 && ptm->tm_hour == SP_START_HOUR){
	    	saved_day = ptm->tm_wday;
		    precip = WEXITSTATUS(system("/mnt/1/precip.sh")); // first check precip forecast
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
		printf("%s %02d/%02d %02d:%02d Sp%d on",
				wdays[ptm->tm_wday], ptm->tm_mon+1, ptm->tm_mday, ptm->tm_hour, ptm->tm_min, sp_round);
		fflush(stdout); // really print it out
		pulse(CH2_ON_BIT);
		sp_st = ST_ON;
		if(sp_round == 1) sp_last_t = t; // save last sprinkling time
		break;
	case ST_ON: // wait for sprinkler time
		if(difftime(t, saved_t) >= (SP_ON_TIME + sp_time_corr[sp_round])){
			printf(", %02d:%02d off\n", ptm->tm_hour, ptm->tm_min);
			pulse(CH2_OFF_BIT);
			sp_st = ST_OFF;
			saved_t = t;
		}
		break;
	case ST_OFF: // wait for pause time
		if(difftime(t, saved_t) >= SP_OFF_TIME){
			sp_st = ++sp_round > 6 ? ST_IDLE : ST_START;
			saved_t = t;
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
	ht_corr = -3 * wmrs->s[1].temp + 15;
	if(ht_corr < 0) ht_corr = 0;

	// correct sprinkling frequency: higher temperature -> frequent watering
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
    int   line;

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
                else if(strcmp(name, "FLT_ON_TIME") == 0)
                	FLT_ON_TIME = atoi(token) * 60;
                else if(strcmp(name, "FLT_ON_TEMP") == 0)
                	FLT_ON_TEMP = atoi(token);
                else if(strcmp(name, "FLT_START_HOUR") == 0)
                	FLT_START_HOUR = atoi(token);
                else if(strcmp(name, "HEAT_ENABLED") == 0)
                	HEAT_ENABLED = atoi(token);
            }
            else {
                printf ("Error in line %d: value expected\n", line);
            }
        }

        ++line;
    }
}

/*
 * Set weekly heating program
 */
static void heat_prog(void)
{
	int i, month, day, prg;
	char buf[MAX_LINE_LEN + 1];

    if((fc = fopen(EXCEPT_FILE, "r")) == NULL){
        printf ("Failed to open exceptions.dat\n");
        return;
    }
	ptm->tm_mday -= ptm->tm_wday;	// back to Sunday
	mktime(ptm);	// update month if needed
	for(i = 0; i < 7; i++){
		// normal program
		if(i == 0 || i == 6)	// weekend
			days[i] = 0;
		else
			days[i] = 1;
		// handle exceptions
		while(fgets(buf, sizeof(buf), fc) != NULL){	// no more exception
			if(sscanf(buf, "%d-%d:%d", &month, &day, &prg) == 3){
				if((ptm->tm_mon == (month - 1)) && (ptm->tm_mday == day)){
					days[i] = prg;	// special date, overwrite program
					break;
				}
			}
			else if(sscanf(buf, "%d:%d", &day, &prg) == 2){
				if(ptm->tm_wday == day){
					days[i] = prg;	// special day, overwrite program
					break;
				}
			}
		}
		rewind(fc);
		ptm->tm_mday++;	// next day
		if(mktime(ptm) == -1)	// update month
			puts("mktime error");
	}
	ptm = localtime(&t);	// restore time struct
	fclose(fc);
}

// free resources on exit
static void cleanup(int dummy)
{
	close(fd);
	shmdt(wmrs);
    exit(EXIT_SUCCESS);
}

int main(void)
{
	time_t last_check_cnf = 0, last_check_exc = 0;
	struct stat cfst;
	int T_flag, time_flag = 0, p_flag = 1;
	int shmid;
	
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

	printf("HomeControl V2.3 - Starting on %d/%d/%d (%d)\n", ptm->tm_year+1900, ptm->tm_mon+1, ptm->tm_mday, T_flag);

	T_flag = 0; // do not log T on startup
	
	/* create shared memory for WMRS communication */
	if((shmid = shmget(1962, sizeof(wmrs_t), IPC_CREAT | 0666)) < 0) die("shmget");
	if((wmrs = shmat(shmid, NULL, SHM_RDONLY)) == (void *)-1) die("shmat");

	if((fd = open(CTRL_PORT, O_RDWR)) < 0) die("open CTRL_PORT");
	sp_last_t = t;
	precip = WEXITSTATUS(system("/mnt/1/precip.sh"));
	precLast = wmrs->precTot;	// reset daily precip

	signal(SIGTERM, cleanup);

	// main loop
	for(; ; sleep(30)){
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
			    fprintf(fc,"   FLT_ON_TIME = %d min\n", FLT_ON_TIME/60);
			    fprintf(fc,"   FLT_ON_TEMP = %d C\n", FLT_ON_TEMP);
			    fprintf(fc,"FLT_START_HOUR = %d\n", FLT_START_HOUR);
			    fprintf(fc,"  HEAT_ENABLED = %d\n", HEAT_ENABLED);
				fclose(fc);
			}
		}
		else if(cfst.st_mtime > last_check_cnf){
			parse_file(CONFIG_FILE);
			last_check_cnf = cfst.st_mtime;
		}

		t = time(NULL);
		ptm = localtime(&t);

		/* check if exceptions file modified */
		if(stat(EXCEPT_FILE, &cfst) == 0){
			if(cfst.st_mtime > last_check_exc){
				heat_prog();
				last_check_exc = cfst.st_mtime;
			}
		}

		/* on every Sunday */
		if(ptm->tm_wday == 0){
			if(time_flag){
				heat_prog();	// update next week program
				time_flag = 0;
			}
		}
		else
			time_flag = 1;

		get_temp();

		corrections(); // must do before reset Tmax!

		/* Log and reset Tmin, Tmax */
		gmtime_r(&t, &utc);
		switch(utc.tm_hour){
		case 18:	// 18 UTC: log daily temperatures
			if(T_flag){ // do it only once
				T_flag = 0;
				if((fc = fopen("/var/T.dat", "a")) != NULL){
			    	fprintf(fc, "%02d/%02d: Tmin=%-5.1f Tmax=%-5.1f", ptm->tm_mon+1, ptm->tm_mday, Tmin, Tmax);
			    	fclose(fc);
			    }
				Tmin = Tmax = wmrs->s[1].temp;	// reset Tmin, Tmax
			}
			break;
		case 6:		// 6 UTC: log daily precip
			if(T_flag){ // do it only once
				T_flag = 0;
				if((fc = fopen("/var/T.dat", "a")) != NULL){
			    	fprintf(fc, " Prec=%3.1f\n", wmrs->precTot - precLast);
			    	fclose(fc);
			    }
				precLast = wmrs->precTot;	// reset daily precip
				Tmax1 = wmrs->s[1].temp;	// reset Tmax1 (for watering)
			}
			break;
		default:
			T_flag = 1;
			break;
		}

		/* get precipitation forecast for next day */
		switch(ptm->tm_hour){
		case 6:
			if(p_flag){ // do it only once
				p_flag = 0;
	    		precip = WEXITSTATUS(system("/mnt/1/precip.sh"));
	    	}
	    	break;
		default:
			p_flag = 1;
			break;
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
	    	fprintf(fc,"Prec/watering = %d/%d\n",ptm->tm_mon+1,ptm->tm_mday);
	    	fprintf(fc,"Watering freq = %d day%c\n",sp_freq, sp_freq > 1 ? 's' : ' ');
	    	fprintf(fc," Heating corr = %d min\n",ht_corr);
	    	fprintf(fc,"         Tmin = %3.1f\n",Tmin);
	    	fprintf(fc,"         Tmax = %3.1f\n",Tmax);
	    	fprintf(fc,"        Tmax1 = %3.1f\n",Tmax1);
	    	fprintf(fc,"  Est. precip = %d mm\n",precip);
		    fprintf(fc,"Heat. program = %d,%d,%d,%d,%d,%d,%d\n", days[0], days[1], days[2], days[3],
		    		days[4], days[5], days[6]);
	    	fclose(fc);
	    }

		/* send all data to the web server in JSON format*/
//		sprintf(shm, "{\"id\":%d, \"sp\":%d, \"sp_st\":%d, \"temp\":%3.1f}", ++n, sp_round, sp_st, temp);

	}
}
/* SDG */
