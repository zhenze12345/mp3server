#include <WinSock2.h>
#include <Shlwapi.h>
#include <windows.h>
#include <stdio.h>
#include "mpg123.h"

#define SERVICE_NAME "Mp3Server"
#define CONFIG_FILE "config.ini"
#define STATUS_FILE "status.ini"
#define SOUND_BUFFER_LEN 16000
#define HEAD_NUM 2

SERVICE_STATUS_HANDLE hStatus;
SERVICE_STATUS ServiceStatus;
WIN32_FIND_DATA fileInfo;
HANDLE hFile = INVALID_HANDLE_VALUE;
char dir[MAX_PATH] = {0};
mpg123_handle *mh = NULL;
char glob_file[MAX_PATH] = {0};
HWAVEOUT waveOut;
char *p[HEAD_NUM];
LPWAVEHDR pHeader[HEAD_NUM];
BOOL playingFlag = FALSE;
SOCKET sock;
SOCKET client = -1;
CRITICAL_SECTION cs;
BOOL stopFlag = FALSE;
BOOL pauseFlag = FALSE;
int lastFrame = 0;

void SaveStatus()
{
	char statusFile[MAX_PATH] = {0};
	char *q;
	int ret;
	off_t frameNumber = 0;
	char frameNumBuffer[128] = {0};

	GetModuleFileName(NULL, statusFile, sizeof(statusFile));
	q = strrchr(statusFile, '\\');
	if(q == NULL)
	{
		return;
	}
	q++;
	*q = '\0';
	strcat(statusFile, STATUS_FILE);

	if(stopFlag)
	{
		if(WritePrivateProfileString("state", "stop", "1", statusFile) == FALSE)
		{
			return;
		}
	}
	else 
	{
		if(WritePrivateProfileString("state", "stop", "0", statusFile) == FALSE)
		{
			return;
		}
	}

	if(pauseFlag)
	{
		if(WritePrivateProfileString("state", "pause", "1", statusFile) == FALSE)
		{
			return;
		}
	}
	else 
	{
		if(WritePrivateProfileString("state", "pause", "0", statusFile) == FALSE)
		{
			return;
		}
	}

	if(WritePrivateProfileString("music", "name", fileInfo.cFileName, statusFile) == FALSE)
	{
		return;
	}

	if(stopFlag != TRUE)
	{
		frameNumber = mpg123_tellframe(mh);
	}
	_snprintf(frameNumBuffer, sizeof(frameNumBuffer), "%d", frameNumber);

	if(WritePrivateProfileString("music", "frame", frameNumBuffer, statusFile) == FALSE)
	{
		return;
	}
}

void ControlHandler(DWORD request)
{
	switch(request)
	{
	case SERVICE_CONTROL_STOP:
	case SERVICE_CONTROL_SHUTDOWN:
		SaveStatus();
		ServiceStatus.dwCurrentState = SERVICE_STOPPED;
		SetServiceStatus(hStatus, &ServiceStatus);
		return;
	default:
		break;
	}

	SetServiceStatus(hStatus, &ServiceStatus);
}

int GetNextFile()
{
	DWORD error;

	if(hFile == INVALID_HANDLE_VALUE)
	{
		goto READAGAIN;
	}

	FindNextFile(hFile, &fileInfo);
	error = GetLastError();
	if(error == ERROR_SUCCESS)
	{
		return 0;
	}

	if(error == ERROR_NO_MORE_FILES)
	{
		CloseHandle(hFile);
		goto READAGAIN;
	}

	return 1;

READAGAIN:
	hFile = FindFirstFile(glob_file, &fileInfo);
	if(hFile == INVALID_HANDLE_VALUE)
	{
		return 2;
	}

	return 0;
}

DWORD InitSerivce()
{
	DWORD ret;
	DWORD i;
	char ip[20] = {0};
	int port;
	WSADATA wsaData;
	SOCKADDR_IN inAddr;
	char configFile[MAX_PATH] = {0};
	char *q;
	FILE *f;

	GetModuleFileName(NULL, configFile, sizeof(configFile));
	q = strrchr(configFile, '\\');
	if(q == NULL)
	{
		return 10;
	}
	q++;
	*q = '\0';
	strcat(configFile, CONFIG_FILE);

	ret = GetPrivateProfileString("music", "dir", "", dir, sizeof(dir), configFile);
	if (ret == 0)
	{
		return 1;
	}

	ret = GetPrivateProfileString("network", "ip", "", ip, sizeof(ip), configFile);
	if (ret == 0)
	{
		return 1;
	}

	port = GetPrivateProfileInt("network", "port", 0, configFile);
	if(port == 0)
	{
		return 1;
	}

	_snprintf(glob_file, sizeof(glob_file), "%s\\*.mp3", dir);	

	ret = mpg123_init();
	if(ret != MPG123_OK)
	{
		return 2;
	}

	mh = mpg123_new(NULL, &ret);
	if(mh == NULL)
	{
		return 3;
	}

	for(i = 0; i < HEAD_NUM; i++)
	{
		p[i] = (LPSTR)malloc(SOUND_BUFFER_LEN);
		pHeader[i] = (LPWAVEHDR)malloc(sizeof(WAVEHDR));
		if(p[i] == NULL ||
			pHeader[i] == NULL)
		{
			return 4;
		}
	}

	ret = WSAStartup(0x0202, &wsaData);
	if(ret != 0)
	{
		return 5;
	}

	sock = socket(AF_INET, SOCK_STREAM, 0);
	if(sock == INVALID_SOCKET)
	{
		return 6;	
	}

	inAddr.sin_family = AF_INET;
	inAddr.sin_addr.s_addr = inet_addr(ip);
	inAddr.sin_port = htons(port);

	ret = bind(sock, (const struct sockaddr *)&inAddr, sizeof(inAddr));
	if(ret == SOCKET_ERROR)
	{
		return 7;
	}

	ret = listen(sock, 1);
	if(ret == SOCKET_ERROR)
	{
		return 8;
	}

	if(!PathFileExists(dir))
	{
		return 9;
	}

	InitializeCriticalSection(&cs);

	return 0;
}

void CALLBACK WaveOutProc(HWAVEOUT hWaveOut, UINT uMsg, DWORD_PTR dwInstance, DWORD_PTR dwParam1, DWORD_PTR dwParam2)
{
	DWORD readBytes = 0;
	LPWAVEHDR pWaveHdr;
	LPSTR p;

	switch(uMsg)
	{
	case WOM_DONE:
		if(playingFlag == FALSE)
		{
			return;
		}
		pWaveHdr = (LPWAVEHDR)dwParam1;
		p = pWaveHdr->lpData;
		EnterCriticalSection(&cs);
		mpg123_read(mh, p, SOUND_BUFFER_LEN, &readBytes);
		LeaveCriticalSection(&cs);
		if(readBytes == 0)
		{
			playingFlag = FALSE;
			return;
		}

		waveOutUnprepareHeader(hWaveOut, pWaveHdr, sizeof(WAVEHDR));

		ZeroMemory(pWaveHdr, sizeof(WAVEHDR));
		pWaveHdr->dwBufferLength = readBytes;
		pWaveHdr->lpData = p;
		waveOutPrepareHeader(hWaveOut, pWaveHdr, sizeof(WAVEHDR));
		waveOutWrite(hWaveOut, pWaveHdr, sizeof(WAVEHDR));

		break;
	}
}

int GetMusicStatus()
{
	char buff[MAX_PATH] = {0};
	DWORD ret;

	_snprintf(buff, MAX_PATH, "%s\\\\%s", dir, fileInfo.cFileName);
	ret = mpg123_open(mh, buff);
	if(ret != MPG123_OK)
	{
		return 1;
	}

	mpg123_seek_frame(mh, lastFrame, SEEK_SET);
	return 0;
}

int PlayMusic()
{
	char buff[MAX_PATH] = {0};
	DWORD ret;
	long rate;
	int channels;
	WAVEFORMATEX wfx;
	DWORD i;
	int readBytes;
	MMRESULT result;
	
	if(pauseFlag == TRUE && lastFrame == 0)
	{
		goto CONTINUE;
	}

	_snprintf(buff, MAX_PATH, "%s\\\\%s", dir, fileInfo.cFileName);
	ret = mpg123_open(mh, buff);
	if(ret != MPG123_OK)
	{
		return 1;
	}
	
	ret = mpg123_getformat(mh, &rate, &channels, NULL);
	if(ret != MPG123_OK)
	{
		return 2;
	}

	ZeroMemory(&wfx, sizeof(wfx));
	wfx.wFormatTag = WAVE_FORMAT_PCM;
	wfx.nChannels = channels;
	wfx.nSamplesPerSec = rate;
	wfx.wBitsPerSample = 16;
	wfx.cbSize = 0;
	wfx.nBlockAlign = wfx.wBitsPerSample * wfx.nChannels / 8;
	wfx.nAvgBytesPerSec = wfx.nChannels * wfx.nSamplesPerSec * wfx.wBitsPerSample / 8;

	if(lastFrame != 0)
	{
		mpg123_seek_frame(mh, lastFrame, SEEK_SET);
		lastFrame = 0;
	}

	ret = waveOutOpen(&waveOut, -1, &wfx, WaveOutProc, NULL, CALLBACK_FUNCTION);
	if(ret != MMSYSERR_NOERROR)
	{
		return 3;
	}

CONTINUE:
	for(i = 0; i < HEAD_NUM; i++)
	{
		ret = mpg123_read(mh, p[i], SOUND_BUFFER_LEN, &readBytes);
		if(ret != MPG123_OK)
		{
			return 4;
		}

		ZeroMemory(pHeader[i], sizeof(WAVEHDR));
		pHeader[i]->dwBufferLength = readBytes;
		pHeader[i]->lpData = p[i];

		result = waveOutPrepareHeader(waveOut, pHeader[i], sizeof(WAVEHDR));
		if(result != MMSYSERR_NOERROR)
		{
			return 5;
		}

		result = waveOutWrite(waveOut, pHeader[i], sizeof(WAVEHDR));
		if(result != MMSYSERR_NOERROR)
		{
			return 6;
		}
	}

	playingFlag = TRUE;
	stopFlag = FALSE;
	pauseFlag = FALSE;
	return 0;
}

void StopMusic()
{
	DWORD i;
	playingFlag = FALSE;

	for(i = 0; i < HEAD_NUM; i++)
	{
		while(waveOutUnprepareHeader(waveOut, pHeader[i], sizeof(WAVEHDR)) == WAVERR_STILLPLAYING)
		{
			Sleep(10);
		}
	}

	waveOutReset(waveOut);
	waveOutClose(waveOut);
}

int DealMsg(char *buff, int len)
{
	if(strcmp(buff, "next") == 0)
	{
		StopMusic();
		stopFlag = TRUE;
		pauseFlag = FALSE;
		if(GetNextFile() != 0 ||
			PlayMusic() != 0)
		{
			return 1;
		}
	}
	else if(strcmp(buff, "again") == 0)
	{
		StopMusic();
		stopFlag = TRUE;
		pauseFlag = FALSE;
		if(PlayMusic() != 0)
		{
			return 1;
		}
	}
	else if(strcmp(buff, "list") == 0)
	{
		WIN32_FIND_DATA tmpFileInfo;
		HANDLE tmpHFile;
		char tmpBuff[10240] = {0};
		int i = 0;

		tmpHFile = FindFirstFile(glob_file, &tmpFileInfo);
		if(tmpHFile == INVALID_HANDLE_VALUE)
		{
			return 1;
		}

		do 
		{
			strcat(tmpBuff, tmpFileInfo.cFileName);
			strcat(tmpBuff, "\n");
		} while (FindNextFile(tmpHFile, &tmpFileInfo));

		send(client, tmpBuff, strlen(tmpBuff), 0);
		CloseHandle(tmpHFile);
	}
	else if(strcmp(buff, "current") == 0)
	{
		int ret;
		double second;
		double second_left;
		char currentBuffer[256] = {0};
		char *p;

		if (stopFlag == TRUE)
		{
			p = "stop";
		}
		else if(pauseFlag == TRUE)
		{
			p = "pause";
		}
		else 
		{
			p = "play";
		}

		ret = mpg123_position(mh, NULL, NULL, NULL, NULL, &second, &second_left);
		if(ret != MPG123_OK)
		{
			return 1;
		}

		_snprintf(currentBuffer, sizeof(currentBuffer), "%s %s %lf %lf", p, fileInfo.cFileName, second, second_left);
		send(client, currentBuffer, strlen(currentBuffer), 0);
	}
	else if(strncmp(buff, "seek ", 5) == 0)
	{
		double secondToSet;
		off_t currentFrame;
		off_t frameLeft;
		double currentSecond;
		double secondLeft;
		int ret;
		double totalSecond;
		off_t totalFrame;
		off_t frameToSet;

		if(buff[5] < '1' || buff[5] > '9')
		{
			return 0;
		}

		secondToSet = atof((const char *)&buff[5]);
		frameToSet = mpg123_timeframe(mh, secondToSet);
		if(frameToSet < 0)
		{
			return 1;
		}
		EnterCriticalSection(&cs);
		ret = mpg123_seek_frame(mh, frameToSet, SEEK_SET);
		LeaveCriticalSection(&cs);
		if(ret < 0)
		{
			return 1;
		}
	}
	else if(strncmp(buff, "select ", 7) == 0)
	{
		char path[MAX_PATH] = {0};
		strcpy(path, dir);
		strcat(path, "\\\\");
		strcat(path, &buff[7]);

		if(!PathFileExists(path))
		{
			return 0;
		}

		StopMusic();
		stopFlag = TRUE;
		pauseFlag = FALSE;
		while(strcmp(fileInfo.cFileName, &buff[7]) != 0)
		{
			GetNextFile();
		}
		if(PlayMusic() != 0)
		{
			return 1;
		}
	}
	else if(strcmp(buff, "stop") == 0)
	{
		if(pauseFlag == FALSE)
		{
			StopMusic();
			mpg123_seek_frame(mh, 0, SEEK_SET);
			stopFlag = TRUE;
		}
	}
	else if(strcmp(buff, "play") == 0)
	{
		if(playingFlag == FALSE)
			PlayMusic();
	}
	else if(strcmp(buff, "pause") == 0)
	{
		if (stopFlag == FALSE)
		{
			playingFlag = FALSE;
			pauseFlag = TRUE;
		}
	}

	return 0;
}

int RecvMsg()
{
	fd_set fds;
	struct timeval tv;
	DWORD ret;
	SOCKET maxFD;
	SOCKADDR_IN inAddr;
	int inAddrLen = 0;
	char buff[1024] = {0};

	tv.tv_sec = 0;
	tv.tv_usec = 10000;
	FD_ZERO(&fds);

	if(client == -1)
	{
		FD_SET(sock, &fds);
		maxFD = sock;
	}
	else
	{
		FD_SET(client, &fds);
		maxFD = client;
	}

	ret = select(maxFD + 1, &fds, NULL, NULL, &tv);
	if(ret > 0)
	{
		if(maxFD == sock)
		{
			ZeroMemory(&inAddr, sizeof(inAddr));
			inAddrLen = sizeof(inAddr);
			client = accept(sock, (struct sockaddr *)&inAddr, &inAddrLen);
			if(client == SOCKET_ERROR)
			{
				client = -1;
				return 0;
			}
			return 0;
		}
		else
		{
			ret = recv(client, (char *)buff, sizeof(buff), 0);
			if(ret == SOCKET_ERROR)
			{
				closesocket(client);
				client = -1;
				return 0;
			}
			else if(ret == 0)
			{
				closesocket(client);
				client = -1;
				return 0;
			}

			if(DealMsg(buff, ret) != 0)
			{
				return 1;
			}

			return 0;
		}
	}
	return 0;
}

int ReadStatus()
{
	char statusFile[MAX_PATH] = {0};
	char fileName[MAX_PATH] = {0};
	char musicPath[MAX_PATH] = {0};
	char *q;
	int stop;
	int pause;
	int frameNumber;
	int ret;

	GetModuleFileName(NULL, statusFile, sizeof(statusFile));
	q = strrchr(statusFile, '\\');
	if(q == NULL)
	{
		return -1;
	}
	q++;
	*q = '\0';
	strcat(statusFile, STATUS_FILE);

	stop = GetPrivateProfileInt("state", "stop", -1, statusFile);
	if(stop == -1)
	{
		return -1;
	}

	pause = GetPrivateProfileInt("state", "pause", -1, statusFile);
	if(pause == -1)
	{
		return -1;
	}

	ret = GetPrivateProfileString("music", "name", "", fileName, sizeof(fileName), statusFile);
	if(ret == 0)
	{
		return -1;
	}

	frameNumber = GetPrivateProfileInt("music", "frame", -1, statusFile);
	if(frameNumber == -1)
	{
		return -1;
	}

	_snprintf(musicPath, MAX_PATH, "%s\\%s", dir, fileName);
	if(!PathFileExists(musicPath))
	{
		return -1;
	}

	while(strcmp(fileInfo.cFileName, fileName) != 0)
	{
		GetNextFile();
	}

	stopFlag = stop;
	pauseFlag = pause;
	lastFrame = frameNumber;
	return frameNumber;
}

void ServiceMain(int argc, char **argv)
{
	DWORD error;
	ServiceStatus.dwServiceType = SERVICE_WIN32;
	ServiceStatus.dwCurrentState = SERVICE_START_PENDING;
	ServiceStatus.dwControlsAccepted = 
		SERVICE_ACCEPT_STOP |
		SERVICE_ACCEPT_SHUTDOWN;
	ServiceStatus.dwWin32ExitCode = 0;
	ServiceStatus.dwServiceSpecificExitCode = 0;
	ServiceStatus.dwCheckPoint = 0;
	ServiceStatus.dwWaitHint = 0;

	hStatus = RegisterServiceCtrlHandler(SERVICE_NAME, (LPHANDLER_FUNCTION)ControlHandler);
	if(hStatus == (SERVICE_STATUS_HANDLE)0)
	{
		return;
	}

	if(InitSerivce() != 0)
	{
		goto STOPSERVER;
	}

	error = ReadStatus();
	if(error == -1)
	{
		if(GetNextFile() != 0 ||
			PlayMusic() != 0)
		{
			goto STOPSERVER;
		}
	}
	else
	{
		if(pauseFlag == FALSE && stopFlag == FALSE)
		{
			if(PlayMusic() != 0)
			{
				goto STOPSERVER;
			}
		}
		else
		{
			if(GetMusicStatus() != 0)
			{
				goto STOPSERVER;
			}
		}
	}

	ServiceStatus.dwCurrentState = SERVICE_RUNNING;
	SetServiceStatus(hStatus, &ServiceStatus);

	while(ServiceStatus.dwCurrentState == SERVICE_RUNNING)
	{
		if(RecvMsg() != 0)
		{
			goto STOPSERVER;
		}

		if(playingFlag == FALSE && stopFlag == FALSE && pauseFlag == FALSE)
		{
			StopMusic();
			if(GetNextFile() != 0 ||
				PlayMusic() != 0)
			{
				goto STOPSERVER;
			}
		}
	}

STOPSERVER:
	ServiceStatus.dwCurrentState = SERVICE_STOPPED;
	ServiceStatus.dwWin32ExitCode = -1;
	SetServiceStatus(hStatus, &ServiceStatus);
	return;
}

int main(int argc, char **argv)
{
	SERVICE_TABLE_ENTRY ServiceTable[2];
	ServiceTable[0].lpServiceName = SERVICE_NAME;
	ServiceTable[0].lpServiceProc = (LPSERVICE_MAIN_FUNCTION)ServiceMain;

	ServiceTable[1].lpServiceName = NULL;
	ServiceTable[1].lpServiceProc = NULL;

	StartServiceCtrlDispatcher(ServiceTable);
	//ServiceMain(argc, argv);
}