/*
 * wmrs200log.h
 *
 *  Created on: Dec 29, 2010
 *      Author: zoli
 */

#ifndef WMRS200LOG_H_
#define WMRS200LOG_H_

#include <time.h>

typedef struct {
	float temp;
	int rh;
	float dew;
	char sBatt;
} sensor_t;

typedef struct {
	sensor_t s[2];
	int relP;
	int absP;
	float wind;
	float gust;
	int windDir;
	char wBatt;
	float prec, prec1, prec24, precTot;
	char pBatt;
	time_t timestamp;
	float tHist[24];
	int rhHist[24];
} wmrs_t;

#endif /* WMRS200LOG_H_ */
