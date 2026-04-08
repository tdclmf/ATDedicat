#include "stdafx.h"
extern xrCriticalSection m_deffered_window_cs;
extern xr_vector<std::function<void()>> m_deffered_window_input;
#include <TlHelp32.h>
#include <DbgHelp.h> // Для работы со стеками
#pragma comment(lib, "DbgHelp.lib")

void PrintCallStack(HANDLE hThread, CONTEXT* pContext)
{
	HANDLE hProcess = GetCurrentProcess();

	STACKFRAME64 stackFrame = { 0 };
	stackFrame.AddrPC.Offset = pContext->Rip;
	stackFrame.AddrPC.Mode = AddrModeFlat;
	stackFrame.AddrFrame.Offset = pContext->Rsp;
	stackFrame.AddrFrame.Mode = AddrModeFlat;
	stackFrame.AddrStack.Offset = pContext->Rsp;
	stackFrame.AddrStack.Mode = AddrModeFlat;

	//// Получаем пути
	//wchar_t exepath[MAX_PATH] = { 0 };
	//GetModuleFileNameW(GetModuleHandleW(NULL), exepath, MAX_PATH);
	//
	//// Находим последний разделитель пути
	//wchar_t* last_slash = wcsrchr(exepath, L'\\');
	//if (last_slash)
	//{
	//	*last_slash = L'\0'; // Обрезаем после последнего слеша
	//}
	//
	//std::wstring exeDir = exepath;
	//std::wstring winDir(MAX_PATH, L'\0');
	//UINT winDirSize = GetWindowsDirectoryW(&winDir[0], MAX_PATH);
	//winDir.resize(winDirSize);
	//
	//// Формируем строку путей поиска
	//std::wstring symbolPath =
	//	L".;" + exeDir + L";" +
	//	winDir + L"\\SysWOW64;" +
	//	winDir + L"\\System32;" +
	//	L"SRV*" + exeDir + L"\\Symbols*https://msdl.microsoft.com/download/symbols";
	//
	//// Инициализируем символы
	//SymInitializeW(hProcess, symbolPath.c_str(), TRUE);

	SymInitialize(hProcess, NULL, TRUE);

	while (StackWalk64(
		IMAGE_FILE_MACHINE_AMD64,
		hProcess,
		hThread,
		&stackFrame,
		pContext,
		NULL,
		SymFunctionTableAccess64,
		SymGetModuleBase64,
		NULL
	))
	{
		DWORD64 displacement = 0;
		char symbolBuffer[sizeof(IMAGEHLP_SYMBOL64) + 255] = { 0 };
		IMAGEHLP_SYMBOL64* pSymbol = (IMAGEHLP_SYMBOL64*)symbolBuffer;
		pSymbol->SizeOfStruct = sizeof(IMAGEHLP_SYMBOL64);
		pSymbol->MaxNameLength = 255;

		if (SymGetSymFromAddr64(hProcess, stackFrame.AddrPC.Offset, &displacement, pSymbol))
		{
			IMAGEHLP_LINE64 line;
			line.SizeOfStruct = sizeof(line);
			DWORD displacementl = 0;

			if (SymGetLineFromAddr64(hProcess, stackFrame.AddrPC.Offset, &displacementl, &line))
				Msg("%s (%d): %s", line.FileName, line.LineNumber, pSymbol->Name);
			else
				Msg("0x%08X: %s", (DWORD)stackFrame.AddrPC.Offset, pSymbol->Name);
		}
		else
			Msg("0x%08X: [Unknown]", (DWORD)stackFrame.AddrPC.Offset);
	}

	SymCleanup(hProcess);
}

void DumpThreadCallStack(DWORD threadId)
{
	HANDLE hThread = OpenThread(THREAD_GET_CONTEXT | THREAD_SUSPEND_RESUME, FALSE, threadId);
	if (!hThread) return;

	// Приостанавливаем поток
	SuspendThread(hThread);

	CONTEXT context = { 0 };
	context.ContextFlags = CONTEXT_FULL;

	if (GetThreadContext(hThread, &context))
	{
		// Анализ стека
		if(Device.mmThreadID == threadId)
			Msg("MainThread[%u]: ", threadId);
		else
			Msg("Thread[%u]: ", threadId);
		PrintCallStack(hThread, &context);
		Msg("\r\n");
	}

	// Возобновляем поток
	ResumeThread(hThread);
	CloseHandle(hThread);
}

void DumpAllThreadsCallStacks(DWORD processId, DWORD excludedThreadId = 0)
{
	DWORD dwOpts = SymGetOptions();
	SymSetOptions(dwOpts | SYMOPT_DEFERRED_LOADS | SYMOPT_LOAD_LINES);

	HANDLE hSnapshot = CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD, 0);
	if (hSnapshot == INVALID_HANDLE_VALUE) return;

	THREADENTRY32 te32;
	te32.dwSize = sizeof(THREADENTRY32);

	if (Thread32First(hSnapshot, &te32)) {
		do {
			if (te32.th32OwnerProcessID == processId &&
				te32.th32ThreadID != excludedThreadId) {
				DumpThreadCallStack(te32.th32ThreadID);
			}
		} while (Thread32Next(hSnapshot, &te32));
	}
	CloseHandle(hSnapshot);
}


// Приостанавливает все потоки процесса, кроме текущего
void SuspendAllThreadsExceptCurrent() {
	DWORD currentThreadId = GetCurrentThreadId();
	DWORD currentProcessId = GetCurrentProcessId();

	HANDLE hSnapshot = CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD, 0);
	if (hSnapshot == INVALID_HANDLE_VALUE) {
		return;
	}

	THREADENTRY32 te32;
	te32.dwSize = sizeof(THREADENTRY32);

	if (Thread32First(hSnapshot, &te32)) {
		do {
			// Если поток принадлежит нашему процессу и это не текущий поток
			if (te32.th32OwnerProcessID == currentProcessId &&
				te32.th32ThreadID != currentThreadId) {
				HANDLE hThread = OpenThread(THREAD_SUSPEND_RESUME, FALSE, te32.th32ThreadID);
				if (hThread != NULL) {
					SuspendThread(hThread);
					CloseHandle(hThread);
				}
			}
		} while (Thread32Next(hSnapshot, &te32));
	}

	CloseHandle(hSnapshot);
}

// Возобновление всех потоков
void ResumeAllThreads() {
	DWORD currentThreadId = GetCurrentThreadId();
	DWORD currentProcessId = GetCurrentProcessId();

	HANDLE hSnapshot = CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD, 0);
	if (hSnapshot == INVALID_HANDLE_VALUE) {
		return;
	}

	THREADENTRY32 te32;
	te32.dwSize = sizeof(THREADENTRY32);

	if (Thread32First(hSnapshot, &te32)) {
		do {
			// Если поток принадлежит нашему процессу и это не текущий поток
			if (te32.th32OwnerProcessID == currentProcessId &&
				te32.th32ThreadID != currentThreadId) {
				HANDLE hThread = OpenThread(THREAD_SUSPEND_RESUME, FALSE, te32.th32ThreadID);
				if (hThread != NULL) {
					ResumeThread(hThread);
					CloseHandle(hThread);
				}
			}
		} while (Thread32Next(hSnapshot, &te32));
	}

	CloseHandle(hSnapshot);
}
bool CRenderDevice::on_message(HWND hWnd, UINT uMsg, WPARAM wParam, LPARAM lParam, LRESULT& result)
{
	switch (uMsg)
	{
	case WM_SYSKEYDOWN:
		{
			return true;
		}
	case WM_ACTIVATE:
		{
#ifdef INGAME_EDITOR
        if (editor())
        {
            Device.b_is_Active = TRUE;
            break;
        }
#endif // #ifdef INGAME_EDITOR
			OnWM_Activate(wParam, lParam);
			return (false);
		}
	case WM_SETCURSOR:
		{
#ifdef INGAME_EDITOR
        if (editor())
            break;
#endif // #ifdef INGAME_EDITOR

			result = 1;
			return (true);
		}
	case WM_SYSCOMMAND:
		{
#ifdef INGAME_EDITOR
        if (editor())
            break;
#endif // #ifdef INGAME_EDITOR

			// Prevent moving/sizing and power loss in fullscreen mode
			switch (wParam)
			{
			case SC_MOVE:
			case SC_SIZE:
			case SC_MAXIMIZE:
			case SC_MONITORPOWER:
				result = 1;
				return (true);
			}
			return (false);
		}
	case WM_CLOSE:
		{
		xrCriticalSection::raii guard(&m_deffered_window_cs);
		m_deffered_window_input.push_back([]() {Engine.Event.Defer("KERNEL:disconnect"); Engine.Event.Defer("KERNEL:quit"); });
#ifdef INGAME_EDITOR
        if (editor())
            break;
#endif // #ifdef INGAME_EDITOR

			result = 0;
			return (true);
		}
	case WM_HOTKEY: // prevent 'ding' sounds caused by Alt+key combinations
	{
		static bool optick_capture_started = false;
		if (wParam == 1)
		{
			if (!optick_capture_started)
			{
				//SuspendAllThreadsExceptCurrent();
#ifdef OPTICK_ENABLE
				auto result = MessageBoxA(
					NULL,
					"Start Optick Profiler Capture?",
					"Optick Profiler Capture",
					MB_OKCANCEL | MB_ICONINFORMATION | MB_SYSTEMMODAL | MB_DEFAULT_DESKTOP_ONLY | MB_SETFOREGROUND
				);
				if (result == IDOK)
				{
					OPTICK_START_CAPTURE();
					optick_capture_started = true;
				}
				if(!optick_capture_started)
#endif
				{
					auto result2 = MessageBoxA(
						NULL,
						"Dump All Threads CallStacks?",
						"All Threads CallStacks Dumper",
						MB_OKCANCEL | MB_ICONINFORMATION | MB_SYSTEMMODAL | MB_DEFAULT_DESKTOP_ONLY | MB_SETFOREGROUND
					);
					if (result2 == IDOK)
					{
						DumpAllThreadsCallStacks(GetCurrentProcessId(), GetCurrentThreadId());
						FlushLog();
					}
				}
			}
			else
			{
#ifdef OPTICK_ENABLE
				//SuspendAllThreadsExceptCurrent();
				auto result = MessageBoxA(
					NULL,
					"Stop Optick Profiler Capture?",
					"Optick Profiler Capture",
					MB_OKCANCEL | MB_ICONINFORMATION | MB_SYSTEMMODAL | MB_DEFAULT_DESKTOP_ONLY | MB_SETFOREGROUND
				);
				if (result == IDOK)
				{
					optick_capture_started = false;
					ResumeAllThreads();
					OPTICK_STOP_CAPTURE();
					SuspendAllThreadsExceptCurrent();
					OPTICK_SAVE_CAPTURE("profiler_capture.opt");
					TRY
					FlushLog();
					CATCH
					TerminateProcess(GetCurrentProcess(), 0);
				}
#endif
			}
			//ResumeAllThreads();
		}
	}return true;
	case WM_SYSCHAR:
		result = 0;
		return true;

	case WM_CHAR:
	{
		xrCriticalSection::raii guard(&m_deffered_window_cs);
		m_deffered_window_input.push_back([=]() {Device.imgui().InputChar(wParam); });
		return false;
	}

	case WM_INPUTLANGCHANGE:
	{
		xrCriticalSection::raii guard(&m_deffered_window_cs);
		m_deffered_window_input.push_back([]() {Device.imgui().UpdateInputLang(); });
		return false;
	}
	}

	return (false);
}

//-----------------------------------------------------------------------------
// Name: WndProc()
// Desc: Static msg handler which passes messages to the application class.
//-----------------------------------------------------------------------------
LRESULT CALLBACK WndProc(HWND hWnd, UINT uMsg, WPARAM wParam, LPARAM lParam)
{
	LRESULT result;
	if (Device.on_message(hWnd, uMsg, wParam, lParam, result))
		return (result);

	return (DefWindowProc(hWnd, uMsg, wParam, lParam));
}
