#include <windows.h>
#include <stdio.h>
#include <wchar.h>
#include <uchar.h>
#include "libs\SimpleString.h"
#include "version.h"

typedef enum {
	MODE_NORMAL = 0,
	MODE_CMD_C,
	MODE_CMD_K
} LAUNCHMODE, *PLAUNCHMODE;

#ifndef SEE_MASK_NOASYNC
#define SEE_MASK_NOASYNC 0x00000100
#endif

#define countof(x) (sizeof(x)/sizeof(x[0]))

VOID WINAPI PrintErrorAndExit( );
__forceinline BOOL IsFlag( PCWSTR pszArg );
__forceinline BOOL CheckFlagI( PCWSTR pszArg, WCHAR ch );
__forceinline BOOL ReadEnvironmentVariable( PCWSTR pszName, PWSTR pszBuffer, DWORD cchBuffer );

int wmain()
{
	LAUNCHMODE mode = MODE_NORMAL;
	BOOL fWait = FALSE;
	BOOL fNoPushD = FALSE;
	BOOL fUnicode = FALSE;
	BOOL fShowUsage = FALSE;

	BOOL fInQuotes = FALSE;
	PWSTR pszCmdLine = GetCommandLineW();

	if (!pszCmdLine)
		return -1;

	// Skip past the program name; i.e., the first (and possibly quoted) token.
	// This is exactly how Microsoft's own CRT discards the first token.

	while (*pszCmdLine > L' ' || (*pszCmdLine && fInQuotes))
	{
		if (*pszCmdLine == L'\"')
			fInQuotes = ~fInQuotes;

		++pszCmdLine;
	}

	// Process the flags; when this loop ends, the pointer position will be at
	// the start of the command line that elevate will execute.

	while (TRUE)
	{
		// Skip past any white space preceding the token.
		while (*pszCmdLine && *pszCmdLine <= L' ')
			++pszCmdLine;

		if (!IsFlag(pszCmdLine))
			break;
		else if (mode == MODE_NORMAL && CheckFlagI(pszCmdLine, L'c'))
			mode = MODE_CMD_C;
		else if (mode == MODE_NORMAL && CheckFlagI(pszCmdLine, L'k'))
			mode = MODE_CMD_K;
		else if (!fNoPushD && CheckFlagI(pszCmdLine, L'n'))
			fNoPushD = TRUE;
		else if (!fUnicode && CheckFlagI(pszCmdLine, L'u'))
			fUnicode = TRUE;
		else if (!fWait && CheckFlagI(pszCmdLine, L'w'))
			fWait = TRUE;
		else
			fShowUsage = TRUE;

		pszCmdLine += 2;
	}

	if (fShowUsage || ((fNoPushD || fUnicode) && mode == MODE_NORMAL) || (*pszCmdLine == 0 && mode != MODE_CMD_K))
	{
		static const WCHAR szUsageTemplateGeneral[] = L"  -%c  %s.\n";

		wprintf(L"Usage: elevate [(-c | -k) [-n] [-u]] [-w] command\n\n");
		wprintf(L"Options:\n");
		wprintf(szUsageTemplateGeneral, L'c', L"Launches a terminating command processor; equivalent to \"cmd /c command\"");
		wprintf(szUsageTemplateGeneral, L'k', L"Launches a persistent command processor; equivalent to \"cmd /k command\"");
		wprintf(szUsageTemplateGeneral, L'n', L"When using -c or -k, do not pushd the current directory before execution");
		wprintf(szUsageTemplateGeneral, L'u', L"When using -c or -k, use Unicode; equivalent to \"cmd /u\"");
		wprintf(szUsageTemplateGeneral, L'w', L"Waits for termination; equivalent to \"start /wait command\"");

		return -1;
	}
	else
	{
		BOOL fSuccess;

		WCHAR szBuffer[MAX_PATH];
		PWSTR pszParamBuffer = NULL;
		PWSTR pszParams=NULL;

		SHELLEXECUTEINFOW sei;
		ZeroMemory(&sei, sizeof(sei));
		sei.cbSize = sizeof(SHELLEXECUTEINFOW);
		sei.fMask = SEE_MASK_NOCLOSEPROCESS | SEE_MASK_NOASYNC | SEE_MASK_FLAG_NO_UI;
		sei.lpVerb = L"runas";
		sei.nShow = SW_SHOWNORMAL;

		if (mode == MODE_NORMAL)
		{
			/* Normal mode: Through the insertion of NULL characters, splice the
			 * remainder of the pszCmdLine string into program and parameters,
			 * while also removing quotes from the program parameter.
			 *
			 * This eliminates the need to copy strings around, avoiding all
			 * associated issues (e.g., accounting for buffer sizes).
			 */

			// Step 1: Strip quotes and walk to the end of the program string.

			sei.lpFile = pszCmdLine;
			fInQuotes = FALSE;

			while (*pszCmdLine > L' ' || (*pszCmdLine && fInQuotes))
			{
				if (*pszCmdLine == L'\"')
				{
					fInQuotes = ~fInQuotes;

					// If we just entered quotes, scooch past the opening quote,
					// and if we are exiting quotes, delete the closing quote.

					if (fInQuotes)
						++sei.lpFile;
					else
						*pszCmdLine = 0;
				}

				++pszCmdLine;
			}

			// Step 2: Walk to the start of the parameters string, replacing
			// the preceding whitespace with string-delimiting NULLs.

			while (*pszCmdLine && *pszCmdLine <= L' ')
				*pszCmdLine++ = 0;

			sei.lpParameters = pszCmdLine;
		}
		else
		{
			/* ComSpec mode: The entire remainder of pszCmdLine (plus a
			 * preceding switch) is the parameters string, and the program
			 * string will, if possible, come from %ComSpec%.
			 */

			sei.lpFile = (ReadEnvironmentVariable(L"ComSpec", szBuffer, countof(szBuffer))) ?
				szBuffer :
				L"cmd.exe"; // Fallback

			if (!fNoPushD)
			{
				// We want <pushd "CurrentDirectory" & command>

				UINT_PTR cchCmdLine = SSLenW(pszCmdLine);
				UINT32 cchDirectory = GetCurrentDirectoryW(0, NULL);

				if (!cchDirectory)
					PrintErrorAndExit();

				if ((pszParamBuffer = LocalAlloc(LMEM_FIXED, (cchDirectory + cchCmdLine + 20) * sizeof(WCHAR))))
				{
					#define SZ_PUSHD_PRE L"pushd \""
					#define CCH_PUSHD_PRE 7
					#define SZ_PUSHD_POST L"\" & "
					#define CCH_PUSHD_POST 4

					pszParams = pszParamBuffer;

					// I like reducing the number of linked import dependencies
					pszParamBuffer += 6;
					pszParamBuffer = SSChainNCpyW(pszParamBuffer, SZ_PUSHD_PRE, CCH_PUSHD_PRE);
					pszParamBuffer += GetCurrentDirectoryW(cchDirectory, pszParamBuffer);
					pszParamBuffer = SSChainNCpyW(pszParamBuffer, SZ_PUSHD_POST, CCH_PUSHD_POST);
					SSChainNCpyW(pszParamBuffer, pszCmdLine, cchCmdLine + 1);
				}
				else
				{
					PrintErrorAndExit();
				}
			}
			else
			{
				pszParams = pszCmdLine - 6;
			}

			if (fUnicode)
				SSCpy4ChW(pszParams, L'/', L'u', L' ', L'/');
			else
				SSCpy4ChW(pszParams, L' ', L' ', L' ', L'/');

			pszParams[4] = (mode == MODE_CMD_C) ? L'c' : L'k';
			pszParams[5] = L' ';

			sei.lpParameters = pszParams;
		}

		fSuccess = ShellExecuteExW(&sei);

		if (!fNoPushD)
			LocalFree(pszParams);

		if (fSuccess)
		{
			// Success: Wait, if necessary, and clean up.

			if (sei.hProcess)
			{
				if (fWait)
					WaitForSingleObject(sei.hProcess, INFINITE);

				CloseHandle(sei.hProcess);
			}

			return 0;
		}
		else
		{
			PrintErrorAndExit();
		}
	}
}

VOID WINAPI PrintErrorAndExit( )
{
	DWORD dwErrorCode = GetLastError();

	if (dwErrorCode)
	{
		PWSTR pszErrorMessage = NULL;

		DWORD cchMessage = FormatMessageW(
			FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_IGNORE_INSERTS | FORMAT_MESSAGE_FROM_SYSTEM,
			NULL,
			dwErrorCode,
			0,
			(PWSTR)&pszErrorMessage,
			0,
			NULL
		);

		if (cchMessage) {

			HANDLE hanStderr = GetStdHandle(STD_ERROR_HANDLE);
			if (GetFileType(hanStderr)== FILE_TYPE_CHAR)
			{
				WriteConsoleW(hanStderr, pszErrorMessage, cchMessage, NULL, NULL);
			}
			else 
			{
				fputws(pszErrorMessage, stderr);
			}
		}
		else
			fwprintf(stderr, L"Error [%08X].\n", dwErrorCode);

		LocalFree(pszErrorMessage);
	}
	else
	{
		fwprintf(stderr, L"Unspecified error.\n");
	}

	ExitProcess(1);
}

__forceinline BOOL IsFlag( PCWSTR pszArg )
{
	return(
		(pszArg[0] | 0x02) == L'/' &&
		(pszArg[1]       ) != 0 &&
		(pszArg[2]       ) <= L' '
	);
}

__forceinline BOOL CheckFlagI( PCWSTR pszArg, WCHAR ch )
{
	return(
		(pszArg[1] | 0x20) == ch
	);
}

__forceinline BOOL ReadEnvironmentVariable( PCWSTR pszName, PWSTR pszBuffer, DWORD cchBuffer )
{
	// A simple GetEnvironmentVariable wrapper with error checking
	DWORD cchCopied = GetEnvironmentVariableW(pszName, pszBuffer, cchBuffer);
	return(cchCopied && cchCopied < cchBuffer);
}
