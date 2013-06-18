// main.cpp	- Kodak DC210 camera
// See kdcpi-0.0.3 for comms protocol

// Based on example from http://playground.arduino.cc/Interfacing/CPPWindows

// This code is distributed in the hope that it will be useful, but WITHOUT ANY WARRANTY;
// without even the implied warranty of MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.
// See the GNU General Public License for more details.

#include <stdio.h>
#include <ctype.h>
#include <tchar.h>
#include "SerialClass.h"
#include <string>

// CONFIGURATION

#define VERBOSITY 0		// 0, 1, 2 (for debugging)
#define LOGGING 0		// 0, 1 (for debugging)

#define PICFILE_DBG "picdebug.jpg"	// More debugging
#define PICFILE_DUMP "picdump.jpg"
#define PICFILE_DEFAULT "picture.jpg"

// From kdcpi-0.0.3
// Control bytes
#define PKT_CTRL_RECV    0x01
#define PKT_CTRL_SEND    0x00
#define PKT_CTRL_EOF     0x80
#define PKT_CTRL_CANCEL  0xFF

// Kodak System Codes
#define DC_COMMAND_COMPLETE  0x00
#define DC_COMMAND_ACK       0xD1
#define DC_CORRECT_PACKET    0xD2
#define DC_COMMAND_NAK       0xE1
#define DC_ILLEGAL_PACKET    0xE3
#define DC_BUSY              0xF0

// Commands common to all implemented Kodak cameras
#define DC_SET_SPEED         0x41

// DC210
#define DC210_LOW_RES_THUMBNAIL 0
#define DC210_HIGH_RES_THUMBNAIL 1
#define DC210_EPOC 852094800

// Kodak System Commands
#define DC210_SET_RESOLUTION      0x36
#define DC210_PICTURE_DOWNLOAD    0x64
#define DC210_PICTURE_INFO        0x65
#define DC210_PICTURE_THUMBNAIL   0x66
#define DC210_SET_SOMETHING       0x75
#define DC210_TAKE_PICTURE        0x7C
#define DC210_ERASE               0x7A
#define DC210_ERASE_IMAGE_IN_CARD 0x7B
#define DC210_INITIALIZE          0x7E
#define DC210_STATUS              0x7F
#define DC210_SET_CAMERA_ID       0x9E

// Make these global
int checksum = 0;
char incomingData[8192];		// Ensure its big enough for 1K download block
char outData[256];
char fullData[4*1024*1024];		// Used for status, picture info, picture transfer
								// TODO allocate memory instead (4MB should suffice for DC210 though)

// Status ... unpack('a1 C9 a2 N1 C1 a1 C7 n2 a28 C1 a32 a30',$data)

int undef1;				// a1

int cameraTypeId;		// C9...
int firmwareMajor;
int firmwareMinor;
int blah1;
int blah2;
int blah3;
int blah4;
int batteryStatusId;
int acStatusId;

int undef2;				// a2..
int cameratime;			// N1 (long)
int zoomMode;			// C1
int undef3;				// A1

int flashCharged;		// C7...
int compressionModeId;
int flashMode;
int exposureCompensation;
int pictureSize;
int fileType;
int undef4;

int totalPicturesTaken;	// n2 (shorts)
int totalFlashesFired;

char undef5[29];		// A28
int numPictures;		// C1
char undef6[33];		// A32
char cameraIdent[31];	// A30

// Picture Info ... unpack('a3C3n1N2a16a12',$data);
char pi_undef[4];		// a3
int pi_resolution;		// C3
int pi_compression;
int pi_undef1;
int pi_pictureNumber;	// n1 (short)
int pi_fileSize;		// N2 (long)
int pi_elapsedTime;
char pi_undef2[17];			// a16
char pi_fileName[13];		// a12


// End globals

int myprintf(char *fmt, ...)
{
	// Replacement for printf() so I can reuse code easily by just substituting logout for printf
	// NB Can pass it a unicode string by using the "%S" (capital S) format specifier. This ONLY works for
	//    simple ascii code-points. It appears that vsnprintf() of unicode string ("%S") internally uses
	//    wcstombs() and thus gives incorrect result with non-ascii code points eg 0x0102 (A with rounded hat)
	//    INSTEAD use logoutW() for unicode strings.

	// NB Do not do this...
	// sprintf(fmt);	// BAD - this can CRASH if attempt to print eg %s parameter as only fmt is passed
	// Instead use vsprintf()

#define LOGOUT_MAX 8192
	char buf[LOGOUT_MAX];	// Buffer

	buf[0] = 0;				// Alternatively ZeroMemory the entire buffer
	va_list args;
	va_start (args, fmt);

	// BUG When passed a unicode string (with fmt=="...%S...") which actually contains non-ascii code points
	//     vsnprintf() truncates the output and returns -1. It appears that vsnprintf() of unicode string
	//     internally uses wcstombs() and thus gives incorrect result with non-ascii code points.
	//     This is fixed in logoutW() which uses _vsnwprintf()

	if (vsnprintf (buf, LOGOUT_MAX-4, fmt, args) == -1)
		buf[LOGOUT_MAX-5] = 0;	// Buffer Overrun, terminate it
	va_end (args);

	static FILE *logfile;
	if (LOGGING)
	{
		if (!logfile)
		{
			logfile = fopen("serial.log", "w");
			if (!logfile)
			{
				printf("ERROR opening logfile for write\n");
				exit(1);
			}
		}
	}
	printf(buf);
	LOGGING && fprintf(logfile,buf);
	fflush(logfile);
	
	return 0;		// NB must return value since using && shortcut operator in calls
}

void revint(int *n)	// Reverse network order of int
{
	int t = ((*n << 24) & 0xFF000000) | ((*n << 8) & 0xFF0000) | ((*n >> 8) & 0xFF00)  | ((*n >> 24) & 0xFF);
	*n = t;
}

void rev_short_as_int(int *n)	// Reverse network order of short value stored as int
{
	int t = ((*n >> 8) & 0x00FF)  | ((*n << 8) & 0xFF00);
	*n = t;
}

void unpack_status()
{
	// Just do a few for now
	undef1 = fullData[0];
	cameraTypeId = fullData[1];
	firmwareMajor = fullData[2];
	firmwareMinor = fullData[3];
	batteryStatusId = fullData[8];
	acStatusId = fullData[9];

	// NB These are big-endian so want to reverse them
	memcpy(&cameratime, fullData+12, 4);
	memcpy(&totalPicturesTaken, fullData+25, 2);	// shorts
	memcpy(&totalFlashesFired, fullData+27, 2);
	memset(undef5, 0, 29);
	memcpy(&undef5, fullData+29, 28);
	memcpy(&numPictures, fullData+57, 4);		// single byte value
	
	// (VERBOSITY > 0) && myprintf("undef1=%d cameraTypeId=%d firmwareMajor=%d firmwareMinor=%d\n", undef1, cameraTypeId, firmwareMajor, firmwareMinor);

	revint(&cameratime);
	(VERBOSITY > -1) && myprintf("batteryStatusId=%d acStatusId=%d time=%d\n", batteryStatusId, acStatusId, cameratime);

	// (VERBOSITY > 0) && myprintf("totalPicturesTaken=%08x totalFlashesFired=%08x numPictures=%08x\n", totalPicturesTaken, totalFlashesFired, numPictures);
	
	rev_short_as_int(&totalPicturesTaken);
	rev_short_as_int(&totalFlashesFired);
	// (VERBOSITY > 0) && myprintf("totalPicturesTaken=%08x totalFlashesFired=%08x numPictures=%08x\n", totalPicturesTaken, totalFlashesFired, numPictures);
	(VERBOSITY > -1) && myprintf("totalPicturesTaken=%d totalFlashesFired=%d numPictures=%d\n", totalPicturesTaken, totalFlashesFired, numPictures);
}

void unpack_picinfo()
{
	// NB These are big-endian so want to reverse them
	memcpy(&pi_resolution, fullData+3, 1);
	memcpy(&pi_compression, fullData+4, 1);
	memcpy(&pi_pictureNumber, fullData+6, 2);	// short
	memcpy(&pi_fileSize, fullData+8, 4);
	memcpy(&pi_elapsedTime, fullData+12, 4);
	memset(pi_fileName, 0, 13);
	memcpy(&pi_fileName, fullData+32, 12);

	rev_short_as_int(&pi_pictureNumber);
	revint(&pi_fileSize);
	(VERBOSITY > -1) && myprintf("picnum=%d resolution=%d compression=%d fileName=%s fileSize=%d\n",
		pi_pictureNumber, pi_resolution, pi_compression, pi_fileName, pi_fileSize);
}

void send_command(Serial* SP, int cmd, int arg1, int arg2, int arg3, int arg4)
{
	// All commands are 8 bytes
	// my $data = pack("C8",$command,0x00,$arg1,$arg2,$arg3,$arg4,0x00,0x1A);
	sprintf(outData,"%c%c%c%c%c%c%c%c",cmd,0x00,arg1,arg2,arg3,arg4,0x00,0x1A);
	(VERBOSITY > 1) && myprintf("send_command %02X [%s]\n", cmd, outData);
	SP->WriteData(outData,8);	// NB do NOT use strlen to get length due to nulls
}

void update_checksum(char *data, int len)
{
	while (len--)
		checksum ^= *data++;
}

void usage()
{
	myprintf("Usage: serial COM4 status|list|get picnum|get all|get start end [nobaud]\n");
	myprintf("If rerunning and camera does not sync, try \"nobaud\" flag\nIf it still fails power-cycle camera.\n");
	exit(1);
}

int _tmain(int argc, _TCHAR* argv[])
{
	// Process arguments, ought really to use getopt here (nobaud is an outlier, ought to be a switch)
	
	if (argc < 3 || argc > 6)
		usage();

	if (strlen(argv[1]) < 3 || strlen(argv[1]) > 5)
	{
		myprintf("Port name too %s, 3-5 chars only\n", strlen(argv[1]) < 3 ? "short" : "long");
		usage();
	}
	
	char comport[20];
	sprintf(comport,"\\\\.\\%s", argv[1]);
	comport[4] = toupper(comport[4]);
	comport[5] = toupper(comport[5]);
	comport[6] = toupper(comport[6]);

	if (strncmp(comport+4,"COM",3))
	{
		myprintf("Port name invalid, must be COM\n");
		usage();
	}

	int wantPicNum = 0;
	int wantLastPicNum = 0;
	int	cmd_status = 0;
	int	cmd_list = 0;
	int	cmd_get = 0;		// Only used in command line check, valus is inferred in state machine
							// due to non-prescence of cmd_status or cmd_list
	int	cmd_all = 0;
	int cmd_range = 0;
	int	no_setbaud = 0;
	
	if (!_stricmp(argv[2],"status"))
		cmd_status = 1;
	else if (!_stricmp(argv[2],"list"))
		cmd_list = 1;
	else if (!_stricmp(argv[2],"get"))
	{
		cmd_get = 1;
		if (argc < 4)
			usage();

		else if (!_stricmp(argv[3],"all"))
		{
			cmd_all = 1;
		}
		else
		{
			cmd_all = 1;		// Use same flag for range
			cmd_range = 1;
			char *pargvpic = argv[3];
			while (*pargvpic)
			if (!isdigit(*pargvpic++))
			{
				myprintf("Picnum \"%s\" is not a valid integer\n", argv[3]);
				usage();
			}
			wantPicNum = atoi(argv[3]);

			if (argc > 4 && _stricmp(argv[4],"nobaud"))
			{
				char *pargvpic = argv[4];
				while (*pargvpic)
				if (!isdigit(*pargvpic++))
				{
					myprintf("Picnum \"%s\" is not a valid integer\n", argv[4]);
					usage();
				}
				wantLastPicNum = atoi(argv[4]);
			}
		}
	}
	else
	{
		usage();
	}

	int numargs = argc;	// Used in checks below
	if (!_stricmp(argv[argc-1],"nobaud"))
	{
		no_setbaud = 1;
		numargs--;
	}

	// Be rather more strict about extra parameters
	if ((cmd_status || cmd_list) && numargs > 3)
		usage();
	if (cmd_get)
	{
		if (cmd_all && !cmd_range && numargs > 4)
			usage();
		if (cmd_all && cmd_range && numargs > 5)
			usage();
		if (!cmd_all && numargs > 4)
			usage();
	}

	// DEBUG checking syntax
#if 0
	printf("p1=%d p2=%d status=%d list=%d get=%dall=%d range=%d nobaud=%d\n", wantPicNum,
			wantLastPicNum,	cmd_status,	cmd_list, cmd_get, cmd_all, cmd_range, no_setbaud);
	exit(1);
#endif
	
	myprintf("Connecting to serial port %s\n", argv[1]);

	// Baud rate is set to 9600 in serial.cpp to match DC210 initial rate
	Serial* SP = new Serial(comport);

	if (SP->IsConnected())
	{
		myprintf("We're connected\n");
	}
	else
	{
		myprintf("ERROR not connected\n");
		return 1;
	}

	int dataLength = sizeof(incomingData)-1;	// Not sure it needs -1
	int readResult = 0;
	int bytesDownloaded = 0;
	int seq = 0;		// State machine
	int expectData = 0;	// Expected bytes to be read
	int lastExpectData = 0;
	int moreData = 0;	// For split packets
	int wroteData = 0;	// per packet

	int noACK = 0;		// Flag
	int wait_PKT = 0;	// Sometimes we get an ACK and need to wait on PKT_CTRL_RECV

	FILE *dbgfile = NULL;
	if (0)				// DEBUGGING dump file as we read it
	{
		dbgfile = fopen(PICFILE_DBG,"wb");	// TODO use filename (check it too)
		if (!dbgfile)
		{
			myprintf("ERROR opening output file %s\n", PICFILE_DBG);
			exit(1);
		}
	}
	
	int dbgpos = 0;

	if (no_setbaud)
		SP->SetSpeed(CBR_115200);	// Camera is already at speed, so set port baudrate
	
	while(SP->IsConnected())
	{
		readResult = SP->ReadData(incomingData,dataLength);

		// NB If we get packet handling wrong the camera may hang & need battery's out to reset

		if (readResult > 0)
		{
			// Hex dump (camera response)
			if (VERBOSITY > 1)
			{
				for (int i=0; i<readResult; i++)
					(VERBOSITY > 1) && myprintf("%02X ", (unsigned char)(incomingData[i]));	// Need cast else prints FFFFFFE1 for E1
				(VERBOSITY > 1) && myprintf("\n");
			}

			int cam_status = (unsigned char)(incomingData[0]);		// Need cast
			int cam_byte1 = (unsigned char)(incomingData[1]);		// Need cast
			
			if (!expectData)
				(VERBOSITY > -1) && myprintf("ERROR - no data was expected\n");	// May want to abort

			(VERBOSITY > 1) && myprintf("seq=%d bytes=%d\n", seq,readResult);

			// DC210 camera comms state machine (receive from camera) 
			
			if (seq == 1)
			{
				// Set speed just responds with one byte ACK
				if (readResult!=expectData || cam_status!=DC_COMMAND_ACK)
					{ (VERBOSITY > -1) && myprintf("... UNEXPECTED\n"); exit(1); }
				else
					{ (VERBOSITY > 0) && myprintf("... OK\n"); seq++; }

				// SP->SetSpeed(CBR_19200);
				SP->SetSpeed(CBR_115200);				
				// CARE may need to power cycle camera if program aborts since still in high speed mode
			}
			else if (seq == 3)
			{
				// Other commands respond with ACK, then we have to keep reading if we get DC_BUSY (0xF0) until
				// until DC_COMMAND_COMPLETE (0x00). Here we assume the latter (TODO COPE with busy or split packet)
				if (readResult!=expectData || cam_status!=DC_COMMAND_ACK || cam_byte1!=DC_COMMAND_COMPLETE)
					{ (VERBOSITY > -1) && myprintf("... UNEXPECTED\n"); exit(1); }
				else
					{ (VERBOSITY > 0) && myprintf("... OK\n"); seq++; }
			}
			else if (seq == 5 || seq == 9 || seq == 13)
			{
				(VERBOSITY > 1) && myprintf("expectData=%d readResult=%d moreData=%d noAck=%d wait_PKT=%d\n", expectData, readResult, moreData, noACK, wait_PKT);
				// This is a large packet and may span several reads
				// UGH, packet control. Expect ACK then PKT_CTRL_RECV (0x01) then 256 bytes then CHECKSUM
				// Need to send back  a single byte DC_ILLEGAL_PACKET or DC_CORRECT_PACKET !!
				// NB we include CHECKSUM in the memcpy as its simpler
				if (moreData)
				{
					if (wait_PKT)
					{
						if (cam_status!=PKT_CTRL_RECV)
							{ (VERBOSITY > -1) && myprintf("... UNEXPECTED\n"); exit(1); }
						else
							{ (VERBOSITY > 1) && myprintf("... wait_PKT OK\n"); }
						wait_PKT = 0;
						if (readResult > 1)
						{
							memcpy(fullData + bytesDownloaded + wroteData, incomingData+1, readResult-1);
							update_checksum(incomingData+1, readResult-1);
							wroteData += readResult-1;
						}
					}
					else
					{
						memcpy(fullData + bytesDownloaded + wroteData, incomingData, readResult);
						update_checksum(incomingData, readResult);
						wroteData += readResult;
					}
					moreData -= readResult;
					(VERBOSITY > 1) && myprintf("CONTINUED New value moreData=%d\n", moreData);
					seq++;
				}
				else
				{
					wroteData = 0;
					if (noACK)
					{
						if (readResult<2 || cam_status!=PKT_CTRL_RECV)
							{ (VERBOSITY > -1) && myprintf("... UNEXPECTED\n"); exit(1); }
						else
							{ (VERBOSITY > 1) && myprintf("... OK\n"); seq++; }
					}
					else 
					{
						if (readResult==1)
						{
							// Sometimes we get an ACK and need to wait on PKT_CTRL_RECV
							if (cam_status!=DC_COMMAND_ACK)
								{ (VERBOSITY > -1) && myprintf("... UNEXPECTED\n"); exit(1); }
							else
								{ (VERBOSITY > 1) && myprintf("... BARE ACK, WAIT PKT_CTRL_RECV\n"); wait_PKT=1; seq++; }
								// NB This will be handled in moreData above
						}
						else
						{
							if (readResult<3 || cam_status!=DC_COMMAND_ACK || cam_byte1!=PKT_CTRL_RECV)
								{ (VERBOSITY > -1) && myprintf("... UNEXPECTED\n"); exit(1); }
							else
								{ (VERBOSITY > 1) && myprintf("... OK\n"); seq++; }
						}
					}
					if ((readResult > 2) || (noACK && (readResult > 1)))
					{
						memcpy(fullData + bytesDownloaded + wroteData, incomingData +2-noACK, readResult-2+noACK);
						update_checksum(incomingData +2-noACK, readResult-2+noACK);
						wroteData += readResult-2+noACK;
					}
					moreData = expectData - readResult;
					(VERBOSITY > 1) && myprintf("FIRST New value moreData=%d\n", moreData);
				}
				if (moreData)
					seq--;	// Continue reading
				if (moreData < 0)
				{
					(VERBOSITY > -1) && myprintf("... NEGATIVE moreData=%d (readResult=%d)\n", moreData, readResult);
					// Dump the file here so we at least get something ...
					moreData = 0;
					(VERBOSITY > -1) && myprintf("DUMPING received data to file %s\n", PICFILE_DUMP);
					FILE *ofile = fopen(PICFILE_DUMP,"wb");
					if (!ofile)
					{
						(VERBOSITY > -1) && myprintf("ERROR opening output file s\n", PICFILE_DUMP);
						exit(1);
					}
					fwrite(fullData, pi_fileSize, 1, ofile);
					fclose(ofile);
					(VERBOSITY > -1) && myprintf("%s file written\n", PICFILE_DUMP);
				}
				
				// We send DC_CORRECT_PACKET in seq 6
				if (!moreData)
				{
					if (checksum)		// Checksum should finish on 0
					{
						(VERBOSITY > -1) && myprintf("BAD CHECKSUM\n");
						// TODO send DC_ILLEGAL_PACKET and re-read packet
					}
						
					if (seq == 6) unpack_status();	// NB seq incremented above
					if (seq == 10) unpack_picinfo();
					// if (seq == 14) ;				// Handled in seq==14 below
				}
			}
			else if (seq == 7 || seq == 11 || seq == 15)
			{
				// Responds with one byte DC_COMMAND_COMPLETE (0x00)
				if (readResult!=expectData || cam_status!=DC_COMMAND_COMPLETE)
					{ (VERBOSITY > -1) && myprintf("... UNEXPECTED\n"); exit(1); }
				else
					{ (VERBOSITY > 1) && myprintf("... OK\n"); seq++; }
			}
		}
		else
		{
			(VERBOSITY > 1) && myprintf("No data\n");
		}

		// if (seq >= 13)
		if (seq >= 8)
			Sleep(10);	// Try at 115200 baud
			// Sleep(100);	// OK at 9600 baud
		else
			Sleep(500);	// Tried 200 but iffy at 115200 baud
			// Sleep(500);	// OK at 9600 baud

		// DC210 camera comms state machine (send to camera) 

		if (seq == 0)
		{
			if (no_setbaud)
				seq = 2;
			else
			{
				// (VERBOSITY > -1) && myprintf("Setting speed 19200 baud\n");
				(VERBOSITY > -1) && myprintf("Setting speed 115200 baud\n");
				// NB DC210 always starts at 9600 baud
				seq++;
				expectData = 1; // Set speed just responds with one byte ACK
				// send_command(SP, DC_SET_SPEED, 0x96, 0, 0, 0);		// 9600 baud is default
				// send_command(SP, DC_SET_SPEED, 0x19, 0x20, 0, 0);	// 19200
				// send_command(SP, DC_SET_SPEED, 0x38, 0x40, 0, 0);	// 38400
				// send_command(SP, DC_SET_SPEED, 0x57, 0x60, 0, 0);	// 57600
				send_command(SP, DC_SET_SPEED, 0x11, 0x52, 0, 0);	// 115200

				// NB We call SP->SetSpeed(CBR_115200); in seq==1
				// CARE may need to power cycle camera if program aborts since still in high speed mode
			}
		}
		else if (seq == 2)
		{
			if (0)
				seq = 4;
			else
			{
				// Initialise camera
				(VERBOSITY > -1) && myprintf("Initializing camera\n");
				seq++;
				expectData = 2;
				send_command(SP, DC210_INITIALIZE, 0, 0, 0, 0);
			}
		} 
		else if (seq == 4)
		{
			// Get status
			checksum = 0;
			(VERBOSITY > -1) && myprintf("Getting status\n");
			// Returns 256 byte packet vis ACK, PKT_CTRL_RECV, 256 bytes packet, CHECKSUM
			seq++;
			expectData = 256+3;
			send_command(SP, DC210_STATUS, 0, 0, 0, 0);
		}
		else if (seq == 6)
		{
			// Respond with single byte DC_CORRECT_PACKET (TODO checksumming)
			seq++;
			expectData = 1;	// Expect DC_COMMAND_COMPLETE (0x00)
			sprintf(outData,"%c",DC_CORRECT_PACKET);
			SP->WriteData(outData,strlen(outData));
		}
		else if (seq == 8)
		{
			checksum = 0;
			if (cmd_status)
				break;		// Done

			(VERBOSITY > 0) && myprintf("Listing picture\n");

			bytesDownloaded = 0;		// Reset buffer (in case looping on all pic download)

			// Currently 36 in camera, 0="DCP02099" 35="DCP02134"
			// Pictures are indexed from 0
			// Returns 256 byte packet vis ACK, PKT_CTRL_RECV, 256 bytes packet, CHECKSUM

			// Indexed from 0 - TODO pass this as a parameter
			// int picnum = 35;
			int picnum = wantPicNum;
			if (picnum >= numPictures)
			{
				(VERBOSITY > -1) && myprintf("Cannot info for picture %d (indexed from 0), only %d pictures in camera\n", picnum, numPictures);
				return 1;
			}
			else
			{
				seq++;
				expectData = 256+3;
				send_command(SP, DC210_PICTURE_INFO, 0, picnum, 0, 0);	// NB arg1=msb arg2=lsb
			}
		}
		else if (seq == 10)
		{
			// Respond with single byte DC_CORRECT_PACKET
			seq++;
			expectData = 1;	// Expect DC_COMMAND_COMPLETE (0x00)
			(VERBOSITY > 1) && myprintf("Send DC_CORRECT_PACKET\n");
			sprintf(outData,"%c",DC_CORRECT_PACKET);
			SP->WriteData(outData,strlen(outData));
		}
		else if (seq == 12)
		{
			checksum = 0;
			if (cmd_list)	// Loop over the pictures
			{
				wantPicNum++;
				if (wantPicNum >= numPictures)
					break;
				seq = 8;
			}
			else
			{

			// Download
			// Returns 1024 byte packets vis ACK, PKT_CTRL_RECV, 1024 bytes packet, CHECKSUM

			(VERBOSITY > 0) && myprintf("seq=12 bytesDownloaded %d pi_fileSize %d\n", bytesDownloaded, pi_fileSize);

			if (pi_fileSize <= 1024)		// Just check its more than a block (it will be)
			{
				(VERBOSITY > -1) && myprintf("ERROR pi_fileSize %d too small\n", pi_fileSize);
				return 1;
			}

			// Be sure not to overrun buffer (TODO allocate memory instead)
			if (pi_fileSize > sizeof(fullData) - 8192 * 2)	// A bit arbitary
			{
				(VERBOSITY > -1) && myprintf("ERROR pi_fileSize %d too large\n", pi_fileSize);
				return 1;
			}
			
			int picnum = wantPicNum;
			if (picnum >= numPictures)
			{
				(VERBOSITY > -1) && myprintf("Cannot download picture %d (indexed from 0), only %d pictures in camera\n", picnum, numPictures);
				return 1;
			}

			if (bytesDownloaded >= pi_fileSize)	// This won't occur here as only run once, see seq==14
			{
				(VERBOSITY > -1) && myprintf("ERROR bytesDownloaded >= pi_fileSize not expected for first packet\n");
				(VERBOSITY > -1) && myprintf("Downloaded %d cf %d expected\n", bytesDownloaded, pi_fileSize);
				break;
			}
			// DO NOT DO THIS ... packet is ALWAYS 1024 bytes ...
			// if (pi_fileSize - bytesDownloaded < 1024)
			//	 expectData=pi_fileSize - bytesDownloaded+3;
			// else
			expectData=1024+3;
			lastExpectData = expectData-3;
			seq++;
			send_command(SP, DC210_PICTURE_DOWNLOAD, 0, picnum, 0, 0);	// NB arg1=msb arg2=lsb
			
			} // End else cmd_list
		}
		else if (seq == 14)
		{
			checksum = 0;
			bytesDownloaded += lastExpectData;
			
			if (dbgfile && (dbgpos < bytesDownloaded))					// DEBUGGING - dump data as we go
			{
				fwrite(fullData+dbgpos, bytesDownloaded-dbgpos, 1, dbgfile);
				dbgpos=bytesDownloaded;
				fflush(dbgfile);
			}
			
			(VERBOSITY > 0) && myprintf("bytesDownloaded %d pi_fileSize %d\n", bytesDownloaded, pi_fileSize);
			if (VERBOSITY == 0) { myprintf("."); fflush(stdout); }	// Progress as line of dots
			
			// Respond with single byte DC_CORRECT_PACKET
			if (bytesDownloaded >= pi_fileSize)
			{
				// This is NORMAL since fixed 1024 byte packets
				// if (bytesDownloaded > pi_fileSize)
				//	 (VERBOSITY > -1) && myprintf("Download too much data %d cf %d expected\n", bytesDownloaded, pi_fileSize);
				// else
				(VERBOSITY == 0) && myprintf("\n");	// End line of dots
				(VERBOSITY > -1) && myprintf("Download done\n");
				// Write the file then exit
				char *fname = pi_fileName;
				if (strncmp(pi_fileName,"DCP",3))
				{
					fname = PICFILE_DEFAULT;
					(VERBOSITY > -1) && myprintf("Invalid filename %s, using %s instead\n",pi_fileName,fname);
				}
				FILE *ofile = fopen(fname,"wb");
				if (!ofile)
				{
					(VERBOSITY > -1) && myprintf("ERROR opening output file %s\n", fname);
					exit(1);
				}
				fwrite(fullData, pi_fileSize, 1, ofile);
				fclose(ofile);
				(VERBOSITY > -1) && myprintf("%s file written\n", fname);
				
				noACK = 0;			// Reset for next pic
				bytesDownloaded = 0;
				if (cmd_all)
				{
					seq++;
					expectData = 1;	// Expect DC_COMMAND_COMPLETE (0x00)
					(VERBOSITY > 1) && myprintf("Send DC_CORRECT_PACKET\n");
					sprintf(outData,"%c",DC_CORRECT_PACKET);
					SP->WriteData(outData,strlen(outData));
				}
				else
				{
					break;		// Exit
					// seq++;	// Alternatively just step on to NULL sequence (15)
				}
			}
			else
			{
				// DO NOT DO THIS ... packet is ALWAYS 1024 bytes ...
				// if (pi_fileSize - bytesDownloaded < 1024)
				//	 expectData=pi_fileSize - bytesDownloaded+2;
				// else
				expectData=1024+2;	// NB We lose the ACK in this packet
				lastExpectData = expectData-2;
				noACK = 1;
				(VERBOSITY > 1) && myprintf("Send DC_CORRECT_PACKET\n");
				sprintf(outData,"%c",DC_CORRECT_PACKET);
				SP->WriteData(outData,strlen(outData));
					seq--;		// Go back to finish it
			}
		}
		else if (seq == 16)
		{
			if (cmd_all)	// Sanity check
			{
				wantPicNum++;
				if (wantPicNum >= numPictures || (cmd_range && wantPicNum > wantLastPicNum))
						break;
				seq = 8;		// Loop back for next pic info (not pic download since need size/name)
			}
			else
			{
				(VERBOSITY > 1) && myprintf("ERROR seq==16 but NOT cmd_all\n");
			}
		}	// End if seq
	}	// End While

	if (1)
	{
		// Reset speed else camera will need power cycling on next run
		(VERBOSITY > -1) && myprintf("Resetting speed to 9600 baud\n");
		send_command(SP, DC_SET_SPEED, 0x96, 0, 0, 0);
		Sleep(200);
		// Don't check response
	}
	else
	{
		(VERBOSITY > -1) && myprintf("WARNING camera is still at 115200 baud, use \"nobaud\" flag if rerunning\n");
	}

	return 0;
}
