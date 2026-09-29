/**********************************************************************
* Filename    : SevenSegmentDisplay.c
* Description : Control SevenSegmentDisplay by 74HC595
* Author      : www.freenove.com
* modification: 2024/07/29
**********************************************************************/
#include <wiringPi.h>
#include <stdio.h>
#include <wiringShift.h>

#define   dataPin   22   //DS Pin of 74HC595(Pin14)
#define   latchPin  27   //ST_CP Pin of 74HC595(Pin12)
#define   clockPin  17   //CH_CP Pin of 74HC595(Pin11)
//encoding for character 0-F of common anode SevenSegmentDisplay. 
unsigned long num[]={0x00c0,0x00f9,0x00a4,0x00b0,0x0099,0x0092,0x0082,0x00f8,0x0080,0x0090,0x0088,0x0083,0x00c6,0x00a1,0x0086,0x008e};

void _shiftOut(int dPin,int cPin,int order,int val){   
    int i;  int cond;
    for(i = 0; i < 16; i++){
        digitalWrite(cPin,LOW);
        cond = (order == LSBFIRST)?((0x01&(val>>i)) == 0x01):((0x8000&(val<<i)) == 0x8000);
	digitalWrite(dPin,cond ? HIGH : LOW);
	delayMicroseconds(10);
        digitalWrite(cPin,HIGH);
        delayMicroseconds(10);
	}
}

void outData(int data)
{
	digitalWrite(latchPin,LOW);
	_shiftOut(dataPin,clockPin,MSBFIRST,data);//Output the figures and the highest level is transfered preferentially. 
	digitalWrite(latchPin,HIGH);
}

void dWrite(int i, int dgt)
{
	outData(num[i]|((1<<dgt)<<8));
}

void strWrite(char* s, int len)
{
    for(int i = 0; i < len; i++){
        dWrite(s[i]-55,i);
	delayMicroseconds(1);
    }
}

void numWrite(int num)
{
    for(int i = 3; i>=0; i--){
        dWrite(num%10,i); num=num/10;
	delayMicroseconds(1);
    }
}

void initDisplay()
{	
	pinMode(dataPin,OUTPUT);
	pinMode(latchPin,OUTPUT);
	pinMode(clockPin,OUTPUT);
}

