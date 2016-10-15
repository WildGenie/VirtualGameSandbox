#include <string>
#include <iostream>

#include "globalvars.hxx"
#include "key_helper.hxx"
#include "process_helper.hxx"

#include "nvidiaHelper.hxx"
#include "scripting.hxx"

BOOL IsProcessRunning(DWORD pid)
{
	HANDLE process = OpenProcess(SYNCHRONIZE, FALSE, pid);
	DWORD ret = WaitForSingleObject(process, 0);
	CloseHandle(process);
	return (ret == WAIT_TIMEOUT);
}

#define NT_SUCCESS(x) ((x) >= 0)
#define STATUS_INFO_LENGTH_MISMATCH 0xc0000004

#define SystemHandleInformation 16
#define ObjectBasicInformation 0
#define ObjectNameInformation 1
#define ObjectTypeInformation 2

typedef NTSTATUS(NTAPI *_NtQuerySystemInformation)(
	ULONG SystemInformationClass,
	PVOID SystemInformation,
	ULONG SystemInformationLength,
	PULONG ReturnLength
	);
typedef NTSTATUS(NTAPI *_NtDuplicateObject)(
	HANDLE SourceProcessHandle,
	HANDLE SourceHandle,
	HANDLE TargetProcessHandle,
	PHANDLE TargetHandle,
	ACCESS_MASK DesiredAccess,
	ULONG Attributes,
	ULONG Options
	);
typedef NTSTATUS(NTAPI *_NtQueryObject)(
	HANDLE ObjectHandle,
	ULONG ObjectInformationClass,
	PVOID ObjectInformation,
	ULONG ObjectInformationLength,
	PULONG ReturnLength
	);

typedef struct _UNICODE_STRING
{
	USHORT Length;
	USHORT MaximumLength;
	PWSTR Buffer;
} UNICODE_STRING, *PUNICODE_STRING;

typedef struct _SYSTEM_HANDLE
{
	ULONG ProcessId;
	BYTE ObjectTypeNumber;
	BYTE Flags;
	USHORT Handle;
	PVOID Object;
	ACCESS_MASK GrantedAccess;
} SYSTEM_HANDLE, *PSYSTEM_HANDLE;

typedef struct _SYSTEM_HANDLE_INFORMATION
{
	ULONG HandleCount;
	SYSTEM_HANDLE Handles[1];
} SYSTEM_HANDLE_INFORMATION, *PSYSTEM_HANDLE_INFORMATION;

typedef enum _POOL_TYPE
{
	NonPagedPool,
	PagedPool,
	NonPagedPoolMustSucceed,
	DontUseThisType,
	NonPagedPoolCacheAligned,
	PagedPoolCacheAligned,
	NonPagedPoolCacheAlignedMustS
} POOL_TYPE, *PPOOL_TYPE;

typedef struct _OBJECT_TYPE_INFORMATION
{
	UNICODE_STRING Name;
	ULONG TotalNumberOfObjects;
	ULONG TotalNumberOfHandles;
	ULONG TotalPagedPoolUsage;
	ULONG TotalNonPagedPoolUsage;
	ULONG TotalNamePoolUsage;
	ULONG TotalHandleTableUsage;
	ULONG HighWaterNumberOfObjects;
	ULONG HighWaterNumberOfHandles;
	ULONG HighWaterPagedPoolUsage;
	ULONG HighWaterNonPagedPoolUsage;
	ULONG HighWaterNamePoolUsage;
	ULONG HighWaterHandleTableUsage;
	ULONG InvalidAttributes;
	GENERIC_MAPPING GenericMapping;
	ULONG ValidAccess;
	BOOLEAN SecurityRequired;
	BOOLEAN MaintainHandleCount;
	USHORT MaintainTypeList;
	POOL_TYPE PoolType;
	ULONG PagedPoolUsage;
	ULONG NonPagedPoolUsage;
} OBJECT_TYPE_INFORMATION, *POBJECT_TYPE_INFORMATION;

PVOID GetLibraryProcAddress(PSTR LibraryName, PSTR ProcName)
{
	return GetProcAddress(GetModuleHandleA(LibraryName), ProcName);
}

HANDLE GetRegistryFileHandle(void)
{
	_NtQuerySystemInformation NtQuerySystemInformation =
		(_NtQuerySystemInformation)GetLibraryProcAddress("ntdll.dll", "NtQuerySystemInformation");
	_NtDuplicateObject NtDuplicateObject =
		(_NtDuplicateObject)GetLibraryProcAddress("ntdll.dll", "NtDuplicateObject");
	_NtQueryObject NtQueryObject =
		(_NtQueryObject)GetLibraryProcAddress("ntdll.dll", "NtQueryObject");
	NTSTATUS status;
	PSYSTEM_HANDLE_INFORMATION handleInfo;
	ULONG handleInfoSize = 0x10000;
	ULONG pid;
	HANDLE processHandle;
	ULONG i;

	HANDLE toreturn = INVALID_HANDLE_VALUE;

	pid = GetCurrentProcessId();

	if (processHandle = OpenProcess(PROCESS_DUP_HANDLE, FALSE, pid))
	{
		handleInfo = (PSYSTEM_HANDLE_INFORMATION)malloc(handleInfoSize);

		/* NtQuerySystemInformation won't give us the correct buffer size,
		so we guess by doubling the buffer size. */
		while ((status = NtQuerySystemInformation(
			SystemHandleInformation,
			handleInfo,
			handleInfoSize,
			NULL
			)) == STATUS_INFO_LENGTH_MISMATCH)
			handleInfo = (PSYSTEM_HANDLE_INFORMATION)realloc(handleInfo, handleInfoSize *= 2);

		/* NtQuerySystemInformation stopped giving us STATUS_INFO_LENGTH_MISMATCH. */
		if (!NT_SUCCESS(status))
		{
			//printf("NtQuerySystemInformation failed!\n");
			return toreturn;
		}

		for (i = 0; i < handleInfo->HandleCount; i++)
		{
			SYSTEM_HANDLE handle = handleInfo->Handles[i];
			HANDLE dupHandle = NULL;
			POBJECT_TYPE_INFORMATION objectTypeInfo;
			PVOID objectNameInfo;
			UNICODE_STRING objectName;
			ULONG returnLength;

			/* Check if this handle belongs to the PID the user specified. */
			if (handle.ProcessId != pid)
				continue;

			/* Duplicate the handle so we can query it. */
			if (!NT_SUCCESS(NtDuplicateObject(
				processHandle,
				(HANDLE)handle.Handle,
				GetCurrentProcess(),
				&dupHandle,
				0,
				0,
				0
				)))
			{
				//printf("[%#x] Error!\n", handle.Handle);
				continue;
			}

			/* Query the object type. */
			objectTypeInfo = (POBJECT_TYPE_INFORMATION)malloc(0x1000);
			if (!NT_SUCCESS(NtQueryObject(
				dupHandle,
				ObjectTypeInformation,
				objectTypeInfo,
				0x1000,
				NULL
				)))
			{
				//printf("[%#x] Error!\n", handle.Handle);
				CloseHandle(dupHandle);
				continue;
			}

			/* Query the object name (unless it has an access of
			0x0012019f, on which NtQueryObject could hang. */
			if (handle.GrantedAccess == 0x0012019f)
			{
				/* We have the type, so display that. */
				//printf(
				//	"[%#x] %.*S: (did not get name)\n",
				//	handle.Handle,
				//	objectTypeInfo->Name.Length / 2,
				//	objectTypeInfo->Name.Buffer
				//	);
				free(objectTypeInfo);
				CloseHandle(dupHandle);
				continue;
			}

			objectNameInfo = malloc(0x1000);
			if (!NT_SUCCESS(NtQueryObject(
				dupHandle,
				ObjectNameInformation,
				objectNameInfo,
				0x1000,
				&returnLength
				)))
			{
				/* Reallocate the buffer and try again. */
				objectNameInfo = realloc(objectNameInfo, returnLength);
				if (!NT_SUCCESS(NtQueryObject(
					dupHandle,
					ObjectNameInformation,
					objectNameInfo,
					returnLength,
					NULL
					)))
				{
					/* We have the type name, so just display that. */
					//printf(
					//	"[%#x] %.*S: (could not get name)\n",
					//	handle.Handle,
					//	objectTypeInfo->Name.Length / 2,
					//	objectTypeInfo->Name.Buffer
					//	);
					free(objectTypeInfo);
					free(objectNameInfo);
					CloseHandle(dupHandle);
					continue;
				}
			}

			/* Cast our buffer into an UNICODE_STRING. */
			objectName = *(PUNICODE_STRING)objectNameInfo;

			/* Print the information! */
			if (objectName.Length)
			{
				/* The object has a name. */
				if (std::wstring(objectName.Buffer).find(L"\\User\\ProgramData\\registry.dat") != std::wstring::npos)
				{
					toreturn = (HANDLE)handle.Handle;
					free(objectTypeInfo);
					free(objectNameInfo);
					CloseHandle(dupHandle);
					break;
				}
				//printf(
				//	"[%#x] %.*S: %.*S\n",
				//	handle.Handle,
				//	objectTypeInfo->Name.Length / 2,
				//	objectTypeInfo->Name.Buffer,
				//	objectName.Length / 2,
				//	objectName.Buffer
				//	);
			}
			else
			{
				/* Print something else. */
				//printf(
				//	"[%#x] %.*S: (unnamed)\n",
				//	handle.Handle,
				//	objectTypeInfo->Name.Length / 2,
				//	objectTypeInfo->Name.Buffer
				//	);
			}

			free(objectTypeInfo);
			free(objectNameInfo);
			CloseHandle(dupHandle);
		}

		free(handleInfo);
		CloseHandle(processHandle);
	}

	return toreturn;
}

bool LaunchGame(void)
{
	std::cout << "Preparing process startup..." << std::flush;

	SetConsoleTitleA(Config.ApplicationProcess.c_str());

	bool r = true;
	while (r)
	{
		if (GetAsyncKeyState(VK_LSHIFT) || GetAsyncKeyState(VK_RSHIFT) || GetAsyncKeyState(VK_SHIFT))
		{
			pressanykey("Press a key to launch...");
		}

		DWORD dwFlags = CREATE_SUSPENDED | INHERIT_PARENT_AFFINITY;
		STARTUPINFO si;
		PROCESS_INFORMATION pi = PROCESS_INFORMATION();
		BOOL fSuccess;
		std::cout << "OK" << std::endl;

		SecureZeroMemory(&si, sizeof(STARTUPINFO));
		si.cb = sizeof(STARTUPINFO);

		std::cout << "Starting process..." << std::flush;
		PerformBeforeGameLoad();
		fSuccess = CreateProcess((MountPoint + Config.ApplicationProcess).c_str(), (LPSTR)("\"" + (MountPoint + Config.ApplicationProcess) + "\" " + Config.ApplicationParameters).c_str(), NULL, NULL, TRUE, dwFlags, NULL, (MountPoint + Config.ApplicationWorkingDir).c_str(), &si, &pi);
		if (!fSuccess)
		{
			std::cout << "Unable to launch process " << (MountPoint + Config.ApplicationProcess).c_str() << " (" << GetLastError() << ")" << std::endl;
			return false;
		}

		ResumeThread(pi.hThread);
		std::cout << "OK" << std::endl;

		int ret = pressanykey("Press any key when done playing to close this window (will detach the Virtual Hard Drive too!)\r\nUse 'R' to restart application\r\nUse 'N' to launch Nvidia Optimus Tool\r\n");
		r = ret == 'R' || ret == 'r';
		bool launchNvidiaHelper = ret == 'N' || ret == 'n';

		while (IsProcessRunning(pi.dwProcessId))
		{
			bool showtext = true;

			if (launchNvidiaHelper)
			{
				launchNvidiaHelper = false;
				goto launchnvhelper;
			}

		back:
			ret = showtext ? pressanykey("Game is still running.\r\n") : pressanykey("");

		launchnvhelper:
			if (ret == 'n' || ret == 'N')
			{
				LaunchNvidiaOptimusTool();
				showtext = false;
				goto back;
			}

			r = ret == 'R' || ret == 'r';
		}

		PerformAfterGameShutdown();
	}
	return true;
}