
/*
 * Copyright (c) 2007
 *      Shrew Soft Inc.  All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 * 1. Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in the
 *    documentation and/or other materials provided with the distribution.
 * 3. Redistributions in any form must be accompanied by information on
 *    how to obtain complete source code for the software and any
 *    accompanying software that uses the software.  The source code
 *    must either be included in the distribution or be available for no
 *    more than the cost of distribution plus a nominal fee, and must be
 *    freely redistributable under reasonable conditions.  For an
 *    executable file, complete source code means the source code for all
 *    modules it contains.  It does not include source code for modules or
 *    files that typically accompany the major components of the operating
 *    system on which the executable file runs.
 *
 * THIS SOFTWARE IS PROVIDED BY SHREW SOFT INC ``AS IS'' AND ANY EXPRESS
 * OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED
 * WARRANTIES OF MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE, OR
 * NON-INFRINGEMENT, ARE DISCLAIMED.  IN NO EVENT SHALL SHREW SOFT INC
 * BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR
 * CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF
 * SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS
 * INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN
 * CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE)
 * ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF
 * THE POSSIBILITY OF SUCH DAMAGE.
 *
 * AUTHOR : Matthew Grooms
 *          mgrooms@shrew.net
 *
 */

#include <stdio.h>
#include <string.h>
#include <sys/ioctl.h>
#include <net/if.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <netinet/ip.h> /* superset of previous */
#include <unistd.h>

#include "ikec.h"

#define TIMER_SIGBP SIGRTMAX

typedef enum
{
    AUTO_CONNECT_STATE_INIT
    , AUTO_CONNECT_STATE_CONNECTING
    , AUTO_CONNECT_STATE_CONNECTED
} autoConnectState_t;

IKEC theIkec;
autoConnectState_t autoConnectState = AUTO_CONNECT_STATE_INIT;

#define MAX_AUTOCONNECT_COUNT   (6)
int autoConnectCount = 0;

timer_t autorunTimerID;
struct itimerspec autorunTimerSpec;

static void timerSignalHandler(int sig, siginfo_t *si, void *uc);

int timerStart(uint32_t msec)
{
    int returnVal = 0;
    time_t sec;
    long nsec;

    sec = (time_t)(msec / 1000);
    nsec = (long)(msec % 1000) * 1000000;

    autorunTimerSpec.it_value.tv_sec = sec;
    autorunTimerSpec.it_value.tv_nsec = nsec;
    autorunTimerSpec.it_interval.tv_sec = 0;
    autorunTimerSpec.it_interval.tv_nsec = 0;

    if (timer_settime(autorunTimerID, 0, &autorunTimerSpec, NULL) == -1)
    {
        theIkec.log(STATUS_FAIL, "Can't start timer\n");
        returnVal = 1;
    }
    return returnVal;
}

int timerInit(void)
{
    struct sigaction sa;
    struct sigevent sev;
    int returnVal = 0;

    sa.sa_flags = SA_SIGINFO;
    sa.sa_sigaction = timerSignalHandler;
    sigemptyset(&sa.sa_mask);
    if (sigaction(TIMER_SIGBP, &sa, NULL) == -1)
    {
        theIkec.log(STATUS_FAIL, "Can't initialize timer signal\n");
        returnVal = 1;
    }
    sev.sigev_notify = SIGEV_SIGNAL;
    sev.sigev_signo = TIMER_SIGBP;
    sev.sigev_value.sival_ptr = &autorunTimerID;

    if (timer_create(CLOCK_REALTIME, &sev, &autorunTimerID) == -1)
    {
        theIkec.log(STATUS_FAIL, "Can't create timer\n");
        returnVal = 1;
    }

    return returnVal;
}

static void timerSignalHandler(int sig, siginfo_t *si, void *uc)
{
    CLIENT_STATE theClientState = theIkec.state();
    uint32_t timerRestartValue = 1000;
    struct tm *pTm;
    time_t now;
    char tbuff[256];

    time(&now);
    pTm = localtime(&now);
    strftime(tbuff, sizeof(tbuff), "%F %T", pTm);

    switch (autoConnectState)
    {
        case AUTO_CONNECT_STATE_INIT:
        case AUTO_CONNECT_STATE_CONNECTING:
            if (CLIENT_STATE_CONNECTING == theClientState)
            {
                if (autoConnectCount >= MAX_AUTOCONNECT_COUNT) exit(1);
                ++autoConnectCount;
                theIkec.log(STATUS_INFO, "AutoConnect[%s]: Client Connection In Process (%d)\n", tbuff, autoConnectCount);
                autoConnectState = AUTO_CONNECT_STATE_CONNECTING;
                timerRestartValue = 10000;
            }
            else if (CLIENT_STATE_CONNECTED == theClientState)
            {
                autoConnectCount = 0;
                theIkec.log(STATUS_INFO, "AutoConnect[%s]: Client Connected\n", tbuff);
                autoConnectState = AUTO_CONNECT_STATE_CONNECTED;
                timerRestartValue = 1000;
            }
            else
            {
                theIkec.log(STATUS_INFO, "AutoConnect[%s]: Client Not Connected.  Connecting...\n", tbuff);
                theIkec.vpn_connect( true );
                autoConnectState = AUTO_CONNECT_STATE_CONNECTING;
                timerRestartValue = 10000;
            }
            break;

        case AUTO_CONNECT_STATE_CONNECTED:
        default:
            if (CLIENT_STATE_DISCONNECTED == theClientState)
            {
                theIkec.log(STATUS_WARN, "AutoConnect[%s]: Client Disconnected.  Reconnecting...\n", tbuff);
                theIkec.vpn_connect( true );
                autoConnectState = AUTO_CONNECT_STATE_CONNECTING;
                timerRestartValue = 10000;
            }
            break;
    }
    timerStart(timerRestartValue);
}

int setMtu(const char *iface, int mtu)
{
    int retVal = 0;
    struct ifreq ifr;
    int sockfd = socket(AF_INET, SOCK_STREAM, IPPROTO_IP);
    if (sockfd <= 0)
    {
        return -2;
    }
    ifr.ifr_addr.sa_family = AF_INET;//address family
    strncpy(ifr.ifr_name, iface, sizeof(ifr.ifr_name));//interface name where you want to set the MTU
    ifr.ifr_mtu = mtu; //your MTU size here
    if (ioctl(sockfd, SIOCSIFMTU, (caddr_t)&ifr) < 0)
    {
        //failed to set MTU. handle error.
        retVal = -1;
    }
    close(sockfd);
    return retVal;
}

int main( int argc, char ** argv )
{

	signal( SIGPIPE, SIG_IGN );

	theIkec.log( 0,
		"## : VPN Connect, ver %d.%d.%d\n"
		"## : Test D\n"
		"## : Copyright %i Shrew Soft Inc.\n"
		"## : press the <h> key for help\n",
		CLIENT_VER_MAJ,
		CLIENT_VER_MIN,
		CLIENT_VER_BLD,
		CLIENT_YEAR );

	// read our command line args

	if( theIkec.read_opts( argc, argv ) != OPT_RESULT_SUCCESS )
	{
		theIkec.show_help();
		return -1;
	}

	if (theIkec.mtu())
	{
        // non-zero MTU.  User must have entered an command-line option
        int setMtuResult = setMtu(theIkec.iface(), theIkec.mtu());
        switch(setMtuResult)
        {
            case -1:
                theIkec.log(STATUS_FAIL,"Failed to set MTU.  Run as sudo\n");
                break;
            case -2:
                theIkec.log(STATUS_FAIL,"Error getting socket\n");
                break;
            case 0:
                theIkec.log(STATUS_INFO, "%s MTU=%d\n", theIkec.iface(), theIkec.mtu());
                break;
            default:
                theIkec.log(STATUS_FAIL,"setMtu() returned %d\n", setMtuResult);
                break;
        }
	}

	// load our site configuration

	if( theIkec.config_load() )
	{
		// autoconnect if requested

		if( theIkec.auto_connect() )
		{
            timerInit();
            timerStart(1000);
			// theIkec.vpn_connect( true );
        }
    }
    else
    {
        return 1;
    }

	// process user input

	bool exit = false;

	while( !exit )
	{
		char next;
		if( !theIkec.read_key( next ) )
			next = 127;

		switch( next )
		{
			case 'c': // <c> connect
				theIkec.vpn_connect( true );
				break;

			case 'd': // <d> disconnect
				theIkec.vpn_disconnect();
				break;

			case 'h': // <h> help
			case '?': // <?> help
				theIkec.log( 0, "%s",
					"Use the following keys to control client connectivity\n"
					" - : <c> connect\n"
					" - : <d> disconnect\n"
					" - : <h> help\n"
					" - : <s> status\n"
					" - : <q> quit\n" );
				break;

			case 'q': // <q> quit
				exit = true;
				break;

			case 's': // <s> status
				theIkec.show_stats();
				break;

            case 127: //
                break;
		}
	}

	return 0;
}
