#include <windows.h>
#include <cassert>
#include <iostream>
#include <libloaderapi.h>
#include <Psapi.h>
#include <winternl.h>
#include <xstring>

using namespace std;

typedef struct object_type_information
{
	UNICODE_STRING type_name;
	ULONG total_number_of_handles;
	ULONG total_number_of_objects;
} object_type_information, *pobject_type_information;

typedef struct object_all_information
{
	ULONG number_of_objects;
	object_type_information object_type_information[1];
} object_all_information, *pobject_all_information;

using NtCloseTypedef = NTSTATUS(*)(HANDLE);
using NtQueryInformationProcessTypedef = NTSTATUS(*)(HANDLE, UINT, PVOID, ULONG, PULONG);
using NtQueryObjectTypedef = NTSTATUS(*)(HANDLE, UINT, PVOID, ULONG, PULONG);
using NtQuerySystemInformationTypedef = NTSTATUS(*)(ULONG, PVOID, ULONG, PULONG);

int str_cmp_impl(const char* x, const char* y)
{
	while (*x)
	{
		if (*x != *y)
			break;
		x++;
		y++;
	}

	return *static_cast<const char*>(x) - *static_cast<const char*>(y);
}

int check_remote_debugger_present_api()
{
	auto dbg_present = 0;

	CheckRemoteDebuggerPresent(GetCurrentProcess(), &dbg_present);

	return dbg_present;
}

int nt_close_invalid_handle()
{
	const auto nt_close = reinterpret_cast<NtCloseTypedef>(GetProcAddress(GetModuleHandleW(L"ntdll.dll"), "NtClose"));

	__try
	{
		nt_close(reinterpret_cast<HANDLE>(0x99999999ULL));
	}
	__except (EXCEPTION_EXECUTE_HANDLER)
	{
		return 1;
	}

	return 0;
}

int nt_query_information_process_debug_flags()
{
	const auto debug_flags = 0x1f;

	const auto query_info_process = reinterpret_cast<NtQueryInformationProcessTypedef>(GetProcAddress(
		GetModuleHandleW(L"ntdll.dll"), "NtQueryInformationProcess"));

	auto debug_inherit = 0;

	const auto status = query_info_process(GetCurrentProcess(), debug_flags, &debug_inherit,
	                                       sizeof(DWORD),
	                                       nullptr);

	if (status == 0x00000000 && debug_inherit == 0)
	{
		return 1;
	}

	return 0;
}

int nt_query_information_process_debug_object()
{
	const auto debug_object_handle = 0x1e;

	const auto query_info_process = reinterpret_cast<NtQueryInformationProcessTypedef>(GetProcAddress(
		GetModuleHandleW(L"ntdll.dll"), "NtQueryInformationProcess"));

	HANDLE debug_object = nullptr;

	const auto information_length = sizeof(ULONG) * 2;

	const auto status = query_info_process(GetCurrentProcess(), debug_object_handle, &debug_object,
	                                       information_length,
	                                       nullptr);

	if (status == 0x00000000 && debug_object)
	{
		return 1;
	}

	return 0;
}


int nt_query_object_all_types_information()
{
	const auto query_object = reinterpret_cast<NtQueryObjectTypedef>(GetProcAddress(
		GetModuleHandleW(L"ntdll.dll"), "NtQueryObject"));

	unsigned long size;

	auto status = query_object(nullptr, 3, &size, sizeof(ULONG), &size);

	const auto address = VirtualAlloc(nullptr, static_cast<size_t>(size), MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);

	if (address == nullptr)
	{
		return 0;
	}

	status = query_object(reinterpret_cast<HANDLE>(- 1), 3, address, size, nullptr);
	if (status != 0)
	{
		VirtualFree(address, 0, MEM_RELEASE);
		return 0;
	}

	const auto all_information = static_cast<pobject_all_information>(address);

	auto location = reinterpret_cast<UCHAR*>(all_information->object_type_information);

	const auto num_objects = all_information->number_of_objects;

	for (auto i = 0; i < static_cast<int>(num_objects); i++)
	{
		const auto type_info = reinterpret_cast<pobject_type_information>(location);

		if (str_cmp_impl(static_cast<const char*>("DebugObject"),
		                 reinterpret_cast<const char*>(type_info->type_name.Buffer)) == 0)
		{
			if (type_info->total_number_of_objects > 0)
			{
				VirtualFree(address, 0, MEM_RELEASE);

				return 1;
			}

			VirtualFree(address, 0, MEM_RELEASE);

			return 0;
		}

		location = reinterpret_cast<unsigned char*>(type_info->type_name.Buffer);

		location += type_info->type_name.MaximumLength;

		auto tmp = reinterpret_cast<ULONG_PTR>(location) & -static_cast<int>(sizeof(void*));

		if (static_cast<ULONG_PTR>(tmp) != reinterpret_cast<ULONG_PTR>(location))
			tmp += sizeof(void*);

		location = reinterpret_cast<unsigned char*>(tmp);
	}

	VirtualFree(address, 0, MEM_RELEASE);

	return 0;
}

int process_job()
{
	auto found_problem = 0;

	const auto struct_size = sizeof(JOBOBJECT_BASIC_PROCESS_ID_LIST) + sizeof(ULONG_PTR) * 1024;

	auto process_id_list = static_cast<JOBOBJECT_BASIC_PROCESS_ID_LIST*>(malloc(struct_size));

	if (process_id_list)
	{
		SecureZeroMemory(process_id_list, struct_size);

		process_id_list->NumberOfProcessIdsInList = 1024;

		if (QueryInformationJobObject(nullptr, JobObjectBasicProcessIdList, process_id_list, struct_size, nullptr))
		{
			auto processes = 0;

			for (auto i = 0; i < static_cast<int>(process_id_list->NumberOfAssignedProcesses); i++)
			{
				const auto process_id = process_id_list->ProcessIdList[i];

				if (process_id == static_cast<ULONG_PTR>(GetCurrentProcessId()))
				{
					processes++;
				}
				else
				{
					const auto process = OpenProcess(PROCESS_QUERY_INFORMATION, 0, static_cast<DWORD>(process_id));

					if (process != nullptr)
					{
						const auto process_name_buffer_size = 4096;

						const auto process_name = static_cast<LPTSTR>(malloc(sizeof(TCHAR) * process_name_buffer_size));

						if (process_name)
						{
							SecureZeroMemory(process_name, sizeof(TCHAR) * process_name_buffer_size);

							if (GetProcessImageFileName(process, process_name, process_name_buffer_size) > 0)
							{
								wstring str(process_name);

								if (str.find(static_cast<wstring>(L"\\Windows\\System32\\conhost.exe")) != string::npos)
								{
									processes++;
								}
							}

							free(process_name);
						}

						CloseHandle(process);
					}
				}
			}

			found_problem = processes != static_cast<int>(process_id_list->NumberOfAssignedProcesses);
		}

		free(process_id_list);
	}

	return found_problem;
}

int titanhide()
{
	const auto module = GetModuleHandleW(L"ntdll.dll");

	const auto information = reinterpret_cast<NtQuerySystemInformationTypedef>(GetProcAddress(
		module, "NtQuerySystemInformation"));

	SYSTEM_CODEINTEGRITY_INFORMATION sci;

	sci.Length = sizeof sci;

	information(SystemCodeIntegrityInformation, &sci, sizeof sci, nullptr);

	const auto ret = sci.CodeIntegrityOptions & CODEINTEGRITY_OPTION_TESTSIGN || sci.CodeIntegrityOptions &
		CODEINTEGRITY_OPTION_DEBUGMODE_ENABLED;

	return ret;
}

void log()
{
}

template <typename First, typename ...Rest>
void log(First&& message, Rest&&...rest)
{
	cout << forward<First>(message);
	log(forward<Rest>(rest)...);
}

int main()
{
	if (nt_close_invalid_handle() != 0)
	{
		log("CloseHandle with an invalid handle detected\r\n");
	}

	if (check_remote_debugger_present_api() != 0)
	{
		log("CheckRemoteDebuggerPresent detected\r\n");
	}

	if (nt_query_information_process_debug_flags() != 0)
	{
		log("NtQueryInformationProcess with ProcessDebugFlags detected\r\n");
	}

	if (nt_query_information_process_debug_object() != 0)
	{
		log("NtQueryInformationProcess with ProcessDebugObject detected\r\n");
	}

	if (nt_query_object_all_types_information() != 0)
	{
		log("NtQueryObject with ObjectAllTypesInformation detected\r\n");
	}

	if (process_job() != 0)
	{
		log("If process is in a job detected\r\n");
	}

	if (titanhide() != 0)
	{
		log("TitanHide detected\r\n");
	}

	log("Foo program. Check source code.\r\n");

	getchar();

	return 0;
}
