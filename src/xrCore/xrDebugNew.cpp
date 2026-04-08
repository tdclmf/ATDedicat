#include "stdafx.h"
#pragma hdrstop

#include "xrdebug.h"
#include "os_clipboard.h"

#include <sal.h>
#include <dxerr.h>

#pragma warning(push)
#pragma warning(disable:4995)
#include <malloc.h>
#include <direct.h>
#pragma warning(pop)

#include "../build_config_defines.h"

extern bool shared_str_initialized;

#ifdef __BORLANDC__
# include "d3d9.h"
# include "d3dx9.h"
# include "D3DX_Wrapper.h"
# pragma comment(lib,"EToolsB.lib")
# define DEBUG_INVOKE DebugBreak()
static BOOL bException = TRUE;
# define USE_BUG_TRAP
#else
#ifndef NO_BUG_TRAP
# define USE_BUG_TRAP
#endif //-!NO_BUG_TRAP
# define DEBUG_INVOKE __asm int 3
static BOOL bException = FALSE;
#endif

#ifndef USE_BUG_TRAP
# include <exception>
#endif

#ifndef _M_AMD64
# ifndef __BORLANDC__
# pragma comment(lib,"dxerr.lib")
# endif
#endif

#include <dbghelp.h> // MiniDump flags

#ifdef USE_BUG_TRAP
# include <BugTrap/source/BugTrap.h> // for BugTrap functionality
#ifndef __BORLANDC__
# pragma comment(lib,"BugTrap.lib") // Link to ANSI DLL
#else
# pragma comment(lib,"BugTrapB.lib") // Link to ANSI DLL
#endif
#endif // USE_BUG_TRAP

#include <new.h> // for _set_new_mode
#include <signal.h> // for signals

#ifdef NO_BUG_TRAP //DEBUG
# define USE_OWN_ERROR_MESSAGE_WINDOW
#else
# define USE_OWN_MINI_DUMP
#endif //-NO_BUG_TRAP //DEBUG

XRCORE_API xrDebug Debug;

static bool error_after_dialog = false;

namespace crash_saving
{
    void (*save_impl)() = nullptr;
	BOOL enabled = TRUE;

    void save()
    {
        if (enabled && save_impl != nullptr)
        {
            (*save_impl)();
        }
    }
}

DialogData::DialogData()
{
    hBackgroundBrush = CreateSolidBrush(RGB(30, 30, 30));
    hButtonBrush = CreateSolidBrush(RGB(60, 60, 60));
    hButtonHoverBrush = CreateSolidBrush(RGB(80, 80, 80));
    hCancelButtonBrush = CreateSolidBrush(RGB(90, 30, 30)); // Красноватый для отмены
    hCancelButtonHoverBrush = CreateSolidBrush(RGB(120, 40, 40)); // Более яркий для hover
}

DialogData::DialogData(std::string ttl, std::string msg) : title(ttl), message(msg)
{
    hBackgroundBrush = CreateSolidBrush(RGB(30, 30, 30));
    hButtonBrush = CreateSolidBrush(RGB(60, 60, 60));
    hButtonHoverBrush = CreateSolidBrush(RGB(80, 80, 80));
    hCancelButtonBrush = CreateSolidBrush(RGB(90, 30, 30)); // Красноватый для отмены
    hCancelButtonHoverBrush = CreateSolidBrush(RGB(120, 40, 40)); // Более яркий для hover
}

void CenterWindow(HWND hWnd) {
    RECT rc;
    GetWindowRect(hWnd, &rc);
    int width = rc.right - rc.left;
    int height = rc.bottom - rc.top;

    int screenWidth = GetSystemMetrics(SM_CXSCREEN);
    int screenHeight = GetSystemMetrics(SM_CYSCREEN);

    SetWindowPos(hWnd, NULL,
        (screenWidth - width) / 2,
        (screenHeight - height) / 2,
        0, 0, SWP_NOZORDER | SWP_NOSIZE);
}

#define DT_LEFT                     0x00000000
#define DT_CENTER                   0x00000001
#define DT_VCENTER                  0x00000004
#define DT_SINGLELINE               0x00000020
#define DT_EXTERNALLEADING          0x00000200
#define DT_NOPREFIX                 0x00000800
typedef int (WINAPI* DrawTextAPtr)(HDC, LPCSTR, int, LPRECT, UINT);

LRESULT CALLBACK DialogProc(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    static DialogData* data = nullptr;
    static HFONT hFont = nullptr;
    static HWND hOkButton = nullptr;
    static HWND hCancelButton = nullptr;
    static bool okButtonHover = false;
    static bool cancelButtonHover = false;

    
    static DrawTextAPtr pDrawTextA = (DrawTextAPtr)GetProcAddress(GetModuleHandleA("user32.dll"), "DrawTextA");

    switch (msg) {
    case WM_CREATE: {
        SetTimer(hWnd, 1, 1000, nullptr);
        // Получаем данные
        CREATESTRUCT* cs = (CREATESTRUCT*)lParam;
        data = (DialogData*)cs->lpCreateParams;

        // Создаем шрифт с белым цветом
        hFont = CreateFontA(
            14, 0, 0, 0, FW_NORMAL,
            FALSE, FALSE, FALSE,
            DEFAULT_CHARSET, OUT_DEFAULT_PRECIS,
            CLIP_DEFAULT_PRECIS, DEFAULT_QUALITY,
            DEFAULT_PITCH | FF_DONTCARE,
            "Courier New"
        );

        // Создаем кнопки
        RECT clientRect;
        GetClientRect(hWnd, &clientRect);

        int buttonWidth = 80;
        int buttonSpacing = 20;
        int totalButtonsWidth = buttonWidth * 2 + buttonSpacing;
        int startX = (clientRect.right - totalButtonsWidth) / 2;

        // Кнопка OK
        hOkButton = CreateWindowA(
            "BUTTON",
            "Ok",
            WS_CHILD | WS_VISIBLE | BS_OWNERDRAW | WS_TABSTOP,
            startX,
            data->totalHeight + 30,
            buttonWidth, 25,
            hWnd,
            (HMENU)1, // ID = 1 для OK
            GetModuleHandle(NULL),
            NULL
        );

        // Кнопка Отмена
        hCancelButton = CreateWindowA(
            "BUTTON",
            "Cancel",
            WS_CHILD | WS_VISIBLE | BS_OWNERDRAW | WS_TABSTOP,
            startX + buttonWidth + buttonSpacing,
            data->totalHeight + 30,
            buttonWidth, 25,
            hWnd,
            (HMENU)2, // ID = 2 для Отмены
            GetModuleHandle(NULL),
            NULL
        );

        // Центрируем окно
        CenterWindow(hWnd);
        return 0;
    }

    case WM_TIMER: {
        if (wParam == 1) {
            SetWindowPos(hWnd, HWND_NOTOPMOST, 0, 0, 0, 0,
                SWP_NOMOVE | SWP_NOSIZE);
            KillTimer(hWnd, 1);
        }
        return 0;
    }

    case WM_CTLCOLORBTN:
    case WM_CTLCOLORSTATIC: {
        HDC hdc = (HDC)wParam;
        SetTextColor(hdc, RGB(255, 255, 255));
        SetBkColor(hdc, RGB(30, 30, 30));
        return (LRESULT)data->hBackgroundBrush;
    }

    case WM_CTLCOLORDLG:
        return (LRESULT)data->hBackgroundBrush;

    case WM_DRAWITEM: {
        LPDRAWITEMSTRUCT dis = (LPDRAWITEMSTRUCT)lParam;
        HDC hdc = dis->hDC;

        // Определяем состояние кнопки
        bool isPressed = (dis->itemState & ODS_SELECTED);
        bool isHover = (dis->itemState & ODS_HOTLIGHT);

        // Выбираем цвет кисти в зависимости от кнопки и состояния
        HBRUSH hBrush;
        if (dis->CtlID == 1) { // Кнопка OK
            if (isPressed) {
                hBrush = CreateSolidBrush(RGB(40, 40, 40));
            }
            else if (isHover) {
                hBrush = (HBRUSH)data->hButtonHoverBrush;
            }
            else {
                hBrush = (HBRUSH)data->hButtonBrush;
            }
        }
        else { // Кнопка Отмена
            if (isPressed) {
                hBrush = CreateSolidBrush(RGB(70, 20, 20));
            }
            else if (isHover) {
                hBrush = (HBRUSH)data->hCancelButtonHoverBrush;
            }
            else {
                hBrush = (HBRUSH)data->hCancelButtonBrush;
            }
        }

        // Заполняем фон кнопки
        FillRect(hdc, &dis->rcItem, hBrush);
        if (isPressed) DeleteObject(hBrush);

        // Рисуем рамку
        HPEN hPen = CreatePen(PS_SOLID, 1, RGB(100, 100, 100));
        HPEN hOldPen = (HPEN)SelectObject(hdc, hPen);
        Rectangle(hdc, dis->rcItem.left, dis->rcItem.top, dis->rcItem.right, dis->rcItem.bottom);
        SelectObject(hdc, hOldPen);
        DeleteObject(hPen);

        // Рисуем текст
        SetTextColor(hdc, RGB(255, 255, 255));
        SetBkMode(hdc, TRANSPARENT);
        const char* buttonText = (dis->CtlID == 1) ? "Ok" : "Cancel";

        pDrawTextA(hdc, buttonText, -1, &dis->rcItem, DT_CENTER | DT_VCENTER | DT_SINGLELINE);

        return TRUE;
    }

    case WM_MOUSEMOVE: {
        // Отслеживаем движение мыши для hover эффекта
        POINT pt = { (int)(short)LOWORD(lParam), (int)(short)HIWORD(lParam) };

        if (hOkButton) {
            RECT buttonRect;
            GetWindowRect(hOkButton, &buttonRect);
            MapWindowPoints(HWND_DESKTOP, hWnd, (LPPOINT)&buttonRect, 2);
            bool newHover = PtInRect(&buttonRect, pt);
            if (newHover != okButtonHover) {
                okButtonHover = newHover;
                InvalidateRect(hOkButton, NULL, FALSE);
            }
        }

        if (hCancelButton) {
            RECT buttonRect;
            GetWindowRect(hCancelButton, &buttonRect);
            MapWindowPoints(HWND_DESKTOP, hWnd, (LPPOINT)&buttonRect, 2);
            bool newHover = PtInRect(&buttonRect, pt);
            if (newHover != cancelButtonHover) {
                cancelButtonHover = newHover;
                InvalidateRect(hCancelButton, NULL, FALSE);
            }
        }
        break;
    }

    case WM_DESTROY:
        if (hFont) DeleteObject(hFont);
        if (data) {
            if (data->hBackgroundBrush) DeleteObject(data->hBackgroundBrush);
            if (data->hButtonBrush) DeleteObject(data->hButtonBrush);
            if (data->hButtonHoverBrush) DeleteObject(data->hButtonHoverBrush);
            if (data->hCancelButtonBrush) DeleteObject(data->hCancelButtonBrush);
            if (data->hCancelButtonHoverBrush) DeleteObject(data->hCancelButtonHoverBrush);
        }
        PostQuitMessage(0);
        return 0;

    case WM_PAINT: {
        PAINTSTRUCT ps;
        HDC hdc = BeginPaint(hWnd, &ps);

        HFONT hOldFont = (HFONT)SelectObject(hdc, hFont);
        SetTextColor(hdc, RGB(255, 255, 255));
        SetBkColor(hdc, RGB(30, 30, 30));
        SetBkMode(hdc, OPAQUE);

        RECT textRect = { 20, 20, data->maxWidth + 40, data->totalHeight + 20 };
        pDrawTextA(hdc, data->message.c_str(), -1, &textRect,
            DT_LEFT | DT_NOPREFIX | DT_EXTERNALLEADING);

        SelectObject(hdc, hOldFont);
        EndPaint(hWnd, &ps);
        return 0;
    }

    case WM_SIZE:
    case WM_VSCROLL:
    case WM_HSCROLL:
    case WM_MOUSEWHEEL:
        return 0;

    case WM_ERASEBKGND: {
        HDC hdc = (HDC)wParam;
        RECT rect;
        GetClientRect(hWnd, &rect);
        FillRect(hdc, &rect, (HBRUSH)data->hBackgroundBrush);
        return 1;
    }

    case WM_COMMAND:
        if (LOWORD(wParam) == 1) { // Кнопка OK
            data->result = 1;
            DestroyWindow(hWnd);
        }
        else if (LOWORD(wParam) == 2) { // Кнопка Отмена
            data->result = 2;
            DestroyWindow(hWnd);
        }
        return 0;

    case WM_KEYDOWN:
        if (wParam == VK_RETURN || wParam == VK_ESCAPE)
        { // Enter - OK
            data->result = 1;
            DestroyWindow(hWnd);
        }
        return 0;
    }

    return DefWindowProcA(hWnd, msg, wParam, lParam);
}

int CustomMessageBox::Show(const std::string& title, const std::string& message) {
    // Регистрируем класс окна
    MessageBeep(MB_ICONHAND);
    WNDCLASSA wc = {};
    wc.lpfnWndProc = DialogProc;
    wc.hInstance = GetModuleHandle(NULL);
    wc.lpszClassName = "CustomMessageBoxClass";
    wc.hbrBackground = CreateSolidBrush(RGB(30, 30, 30)); // Тёмный фон
    wc.hCursor = LoadCursor(NULL, IDC_ARROW);
    wc.lpszMenuName = NULL;
    wc.cbClsExtra = 0;
    wc.cbWndExtra = 0;
    wc.style = CS_HREDRAW | CS_VREDRAW;

    RegisterClassA(&wc);

    // Создаем данные для передачи в окно
    DialogData data = DialogData{ title, message };
    data.result = 0; // По умолчанию - OK

    // Рассчитываем размер окна ДО создания
    CalculateInitialSize(data);

    // Создаем окно поверх всех окон
    HWND hWnd = CreateWindowExA(
        WS_EX_DLGMODALFRAME,
        "CustomMessageBoxClass",
        title.c_str(),
        WS_POPUP | WS_CAPTION | WS_SYSMENU,
        CW_USEDEFAULT, CW_USEDEFAULT,
        data.windowWidth, data.windowHeight,
        NULL, NULL, GetModuleHandle(NULL),
        &data
    );

    if (hWnd) {
        SetWindowPos(hWnd, HWND_TOPMOST, 0, 0, 0, 0,
            SWP_NOMOVE | SWP_NOSIZE);
        // Заставляем окно мигать в панели задач
        FLASHWINFO fwi;
        fwi.cbSize = sizeof(FLASHWINFO);
        fwi.hwnd = hWnd;
        fwi.dwFlags = FLASHW_ALL | FLASHW_TIMERNOFG;
        fwi.uCount = 3;
        fwi.dwTimeout = 0;
        FlashWindowEx(&fwi);

        ShowWindow(hWnd, SW_SHOW);
        UpdateWindow(hWnd);
        SetActiveWindow(hWnd);
        SetForegroundWindow(hWnd);
        SetFocus(hWnd);

        // Message loop
        MSG msg;
        while (GetMessage(&msg, NULL, 0, 0)) {
            TranslateMessage(&msg);
            DispatchMessage(&msg);
        }

        // После завершения цикла сообщений возвращаем результат
        return data.result;
    }
    UnregisterClassA("CustomMessageBoxClass", GetModuleHandle(NULL));
    return 1; // В случае ошибки возвращаем 0 (OK)
}

void CustomMessageBox::CalculateInitialSize(DialogData& data) {
    // Создаем временный DC для расчетов
    HDC hdc = GetDC(NULL);
    HFONT hFont = CreateFontA(
        14, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE,
        DEFAULT_CHARSET, OUT_DEFAULT_PRECIS,
        CLIP_DEFAULT_PRECIS, DEFAULT_QUALITY,
        DEFAULT_PITCH | FF_DONTCARE, "Courier New"
    );

    HFONT hOldFont = (HFONT)SelectObject(hdc, hFont);

    // Разбиваем текст на строки
    std::vector<std::string> lines;
    size_t start = 0;
    size_t end = data.message.find('\n');

    while (end != std::string::npos) {
        lines.push_back(data.message.substr(start, end - start));
        start = end + 1;
        end = data.message.find('\n', start);
    }
    lines.push_back(data.message.substr(start));

    // Находим максимальную ширину текста
    data.maxWidth = 0;
    for (const auto& line : lines) {
        SIZE size;
        GetTextExtentPoint32A(hdc, line.c_str(), (int)line.length(), &size);
        data.maxWidth = std::max(data.maxWidth, (int)size.cx);
    }

    // Вычисляем высоту текста и высоту строки
    TEXTMETRIC tm;
    GetTextMetrics(hdc, &tm);
    data.lineHeight = tm.tmHeight + tm.tmExternalLeading;
    data.totalHeight = data.lineHeight * (int)lines.size();

    // Рассчитываем размер окна (ограничиваем размером экрана)
    int screenWidth = GetSystemMetrics(SM_CXSCREEN);
    int screenHeight = GetSystemMetrics(SM_CYSCREEN);

    // Увеличиваем высоту для двух кнопок
    int contentWidth = data.maxWidth + 70;
    int contentHeight = data.totalHeight + 110;

    // Ограничиваем максимальный размер
    data.windowWidth = std::min(contentWidth, screenWidth - 100);
    data.windowHeight = std::min(contentHeight, screenHeight - 100);

    // Минимальный размер окна
    data.windowWidth = std::max(data.windowWidth, 400);
    data.windowHeight = std::max(data.windowHeight, 200);

    SelectObject(hdc, hOldFont);
    DeleteObject(hFont);
    ReleaseDC(NULL, hdc);
}

// demonized: print stack trace
#include "mezz_stringbuffer.h"
#include "../3rd party/stackwalker/include/StackWalker.h"
class xr_StackWalker : public StackWalker
{
public:
    std::string buffer;
    xr_StackWalker() : StackWalker(StackWalker::StackWalkOptions::RetrieveSymbol
        | StackWalker::StackWalkOptions::RetrieveLine
        | StackWalker::StackWalkOptions::SymBuildPath
    ) {}
protected:    
    virtual void OnOutput(LPCSTR szText) {
        std::string s = szText;
        std::string sLowered = s;
        toLowerCase(sLowered);
        if (sLowered.find(".dll") != std::string::npos) return;
        if (sLowered.find(".drv") != std::string::npos) return;
        if (sLowered.find("__scrt_common_main_seh") != std::string::npos) return;
        if (s.find("ERROR: SymGetSymFromAddr64") != std::string::npos) return;
        if (s.find("ERROR: SymGetLineFromAddr64") != std::string::npos) return;
        if (sLowered.find("filename not available") != std::string::npos) return;
        if (sLowered.find("function-name not available") != std::string::npos) return;

        if (s.find("PDB:") != std::string::npos)
        {
            trim(s);
            s += "\nstack trace:\n";
            buffer += s;
            return;
        }


        size_t srcPos = s.find("xray-monolith\\src\\");
        if (srcPos != std::string::npos) {
            // Находим начало после "src\"
            size_t startPos = srcPos + 18; // длина "\\src\\"
            s = s.substr(startPos);
        }

        trim(s);
        s += "\n";
        buffer += s;
    }
};
extern void printLuaStack();
void LogStackTrace(LPCSTR header = nullptr, bool printStack = false)
{
	if (!shared_str_initialized)
		return;

    if (header)
	    Msg("%s", header);

    if (printStack) {
        printLuaStack();
        Msg("\n");
        auto s = xr_StackWalker();
        s.ShowCallstack();
        Msg(s.buffer.c_str());
    }
}

int xrDebug::ShowStackTrace(std::string title, bool print_stack)
{
    ShowCursor(true);
    ShowWindow(GetActiveWindow(), SW_FORCEMINIMIZE);
    auto s = xr_StackWalker();
    s.ShowCallstack();
    s.buffer += "\nPress «OK» to continue or «Cancel» to abort execution\n";

    if(print_stack && shared_str_initialized)
    {
        Msg(s.buffer.c_str());
        FlushLog();
    }
    CustomMessageBox msg_box = CustomMessageBox();
    return msg_box.Show(title, s.buffer.c_str());
}

void xrDebug::gather_info(const char* expression, const char* description, const char* argument0, const char* argument1,
                          const char* file, int line, const char* function, LPSTR assertion_info,
                          u32 const assertion_info_size)
{
	if (!expression)
		expression = "<no expression>";
	LPSTR buffer_base = assertion_info;
	LPSTR buffer = assertion_info;
	int assertion_size = (int)assertion_info_size;
	LPCSTR endline = "\n";
	LPCSTR prefix = "[error]";
	bool extended_description = (description && !argument0 && strchr(description, '\n'));
	for (int i = 0; i < 2; ++i)
	{
		if (!i)
			buffer += xr_sprintf(buffer, assertion_size - u32(buffer - buffer_base), "%sFATAL ERROR%s%s", endline,
			                     endline, endline);
		buffer += xr_sprintf(buffer, assertion_size - u32(buffer - buffer_base), "%sExpression    : %s%s", prefix,
		                     expression, endline);
		buffer += xr_sprintf(buffer, assertion_size - u32(buffer - buffer_base), "%sFunction      : %s%s", prefix,
		                     function, endline);
		buffer += xr_sprintf(buffer, assertion_size - u32(buffer - buffer_base), "%sFile          : %s%s", prefix, file,
		                     endline);
		buffer += xr_sprintf(buffer, assertion_size - u32(buffer - buffer_base), "%sLine          : %d%s", prefix, line,
		                     endline);

		if (extended_description)
		{
			buffer += xr_sprintf(buffer, assertion_size - u32(buffer - buffer_base), "%s%s%s", endline, description,
			                     endline);
			if (argument0)
			{
				if (argument1)
				{
					buffer += xr_sprintf(buffer, assertion_size - u32(buffer - buffer_base), "%s%s", argument0,
					                     endline);
					buffer += xr_sprintf(buffer, assertion_size - u32(buffer - buffer_base), "%s%s", argument1,
					                     endline);
				}
				else
					buffer += xr_sprintf(buffer, assertion_size - u32(buffer - buffer_base), "%s%s", argument0,
					                     endline);
			}
		}
		else
		{
			buffer += xr_sprintf(buffer, assertion_size - u32(buffer - buffer_base), "%sDescription   : %s%s", prefix,
			                     description, endline);
			if (argument0)
			{
				if (argument1)
				{
					buffer += xr_sprintf(buffer, assertion_size - u32(buffer - buffer_base), "%sArgument 0    : %s%s",
					                     prefix, argument0, endline);
					buffer += xr_sprintf(buffer, assertion_size - u32(buffer - buffer_base), "%sArgument 1    : %s%s",
					                     prefix, argument1, endline);
				}
				else
					buffer += xr_sprintf(buffer, assertion_size - u32(buffer - buffer_base), "%sArguments     : %s%s",
					                     prefix, argument0, endline);
			}
		}

		buffer += xr_sprintf(buffer, assertion_size - u32(buffer - buffer_base), "%s", endline);
		if (!i)
		{
			if (shared_str_initialized)
			{
				Msg("%s", assertion_info);
				FlushLog();
			}
			buffer = assertion_info;
			endline = "\r\n";
			prefix = "";
		}
	}

#ifdef USE_MEMORY_MONITOR
    memory_monitor::flush_each_time(true);
    memory_monitor::flush_each_time(false);
#endif //-USE_MEMORY_MONITOR

	if (!IsDebuggerPresent() && !strstr(GetCommandLine(), "-no_call_stack_assert"))
	{
        LogStackTrace(nullptr, true);

#ifdef USE_OWN_ERROR_MESSAGE_WINDOW
		buffer += xr_sprintf(buffer, assertion_size - u32(buffer - buffer_base), "stack trace:%s%s", endline, endline);
#endif //-USE_OWN_ERROR_MESSAGE_WINDOW

		//        BuildStackTrace();

		//        for (int i = 2; i < g_stackTraceCount; ++i)
		//        {
		//            if (shared_str_initialized)
		//                Msg("%s", g_stackTrace[i]);
		//
		//#ifdef USE_OWN_ERROR_MESSAGE_WINDOW
		//            buffer += xr_sprintf(buffer, assertion_size - u32(buffer - buffer_base), "%s%s", g_stackTrace[i], endline);
		//#endif //-USE_OWN_ERROR_MESSAGE_WINDOW
		//        }

		if (shared_str_initialized)
			FlushLog();

		os_clipboard::copy_to_clipboard(assertion_info);
	}
}

void xrDebug::do_exit(const std::string& message)
{
    FlushLog();

    ShowCursor(true);
    ShowWindow(GetActiveWindow(), SW_FORCEMINIMIZE);
    auto result = MessageBox(
        NULL,
        message.c_str(),
        "Fatal Error",
        MB_OKCANCEL | MB_ICONERROR | MB_SYSTEMMODAL | MB_DEFAULT_DESKTOP_ONLY | MB_SETFOREGROUND
    );
    if (result == IDCANCEL)
        TerminateProcess(GetCurrentProcess(), 1);
    if (result == IDOK)
        ShowCursor(FALSE);
}

#ifdef NO_BUG_TRAP
//AVO: simplified function
void xrDebug::backend(const char* expression, const char* description, const char* argument0, const char* argument1,
                      const char* file, int line, const char* function, bool& ignore_always)
{
    // we save first
    crash_saving::save();

    static xrCriticalSection CS;
    xrCriticalSection::raii guard(&CS);

	string4096 assertion_info;

	gather_info(expression, description, argument0, argument1, file, line, function, assertion_info,
	            sizeof(assertion_info));

	LPCSTR endline = "\r\n";
	LPSTR buffer = assertion_info + xr_strlen(assertion_info);
	buffer += xr_sprintf(buffer, sizeof(assertion_info) - u32(buffer - &assertion_info[0]),
	                     "%sPress «OK» to continue or «Cancel» to abort execution%s", endline, endline);

	if (handler)
		handler();

	FlushLog();
    if(Core.ignore_error_window==0)
    {
        ShowCursor(true);
        ShowWindow(GetActiveWindow(), SW_FORCEMINIMIZE);
        CustomMessageBox msg_box = CustomMessageBox();
        auto result = msg_box.Show("Fatal Error", assertion_info);
        if (result == IDCANCEL)
        {
            if (IsDebuggerPresent())
                DebugBreak();
            else
                TerminateProcess(GetCurrentProcess(), 1);
        }
        if (result == IDOK)
            ShowCursor(FALSE);
    }
}

//-AVO
#else
void xrDebug::backend(const char* expression, const char* description, const char* argument0, const char* argument1, const char* file, int line, const char* function, bool& ignore_always)
{
    static xrCriticalSection CS
#ifdef PROFILE_CRITICAL_SECTIONS
        (MUTEX_PROFILE_ID(xrDebug::backend))
#endif // PROFILE_CRITICAL_SECTIONS
        ;

    CS.Enter();

    error_after_dialog = true;

    string4096 assertion_info;

    gather_info(expression, description, argument0, argument1, file, line, function, assertion_info, sizeof(assertion_info));

#ifdef USE_OWN_ERROR_MESSAGE_WINDOW
    LPCSTR endline = "\r\n";
    LPSTR buffer = assertion_info + xr_strlen(assertion_info);
    buffer += xr_sprintf(buffer, sizeof(assertion_info) - u32(buffer - &assertion_info[0]), "%sPress CANCEL to abort execution%s", endline, endline);

    buffer += xr_sprintf(buffer, sizeof(assertion_info) - u32(buffer - &assertion_info[0]), "Press TRY AGAIN to continue execution%s", endline);
    buffer += xr_sprintf(buffer, sizeof(assertion_info) - u32(buffer - &assertion_info[0]), "Press CONTINUE to continue execution and ignore all the errors of this type%s%s", endline, endline);
#endif // USE_OWN_ERROR_MESSAGE_WINDOW

    if (handler)
        handler();

    if (get_on_dialog())
        get_on_dialog() (true);

    FlushLog();

#ifdef XRCORE_STATIC
    MessageBox (NULL,assertion_info,"X-Ray error",MB_OK|MB_ICONERROR|MB_SYSTEMMODAL);
#else
# ifdef USE_OWN_ERROR_MESSAGE_WINDOW
    ShowCursor(true);
    ShowWindow(GetActiveWindow(), SW_FORCEMINIMIZE);
    int result =
        MessageBox(
        GetTopWindow(NULL),
        assertion_info,
        "Fatal Error",
        /*MB_CANCELTRYCONTINUE*/MB_OK | MB_ICONERROR | /*MB_SYSTEMMODAL |*/ MB_DEFBUTTON1 | MB_SETFOREGROUND
        );

    switch (result)
    {
    case IDCANCEL:
    {
# ifdef USE_BUG_TRAP
        BT_SetUserMessage(assertion_info);
# endif // USE_BUG_TRAP
        DEBUG_INVOKE;
        break;
    }
    case IDTRYAGAIN:
    {
        error_after_dialog = false;
        break;
    }
    case IDCONTINUE:
    {
        error_after_dialog = false;
        ignore_always = true;
        break;
    }
    case IDOK:
    {
        FlushLog();
        TerminateProcess(GetCurrentProcess(), 1);
    }
    default:
        DEBUG_INVOKE;
    }
# else // USE_OWN_ERROR_MESSAGE_WINDOW
# ifdef USE_BUG_TRAP
    BT_SetUserMessage (assertion_info);
# endif // USE_BUG_TRAP
    DEBUG_INVOKE;
# endif // USE_OWN_ERROR_MESSAGE_WINDOW
#endif

    if (get_on_dialog())
        get_on_dialog() (false);

    CS.Leave();
}
#endif

LPCSTR xrDebug::error2string(long code)
{
	char* result = 0;
	static string1024 desc_storage;

#ifdef _M_AMD64
#else
	WCHAR err_result[1024];
    DXGetErrorDescription(code,err_result,sizeof(err_result));
	wcstombs(result, err_result, sizeof(err_result));
#endif
	if (0 == result)
	{
		FormatMessage(FORMAT_MESSAGE_FROM_SYSTEM, 0, code, 0, desc_storage, sizeof(desc_storage) - 1, 0);
		result = desc_storage;
	}
	return result;
}

void xrDebug::error(long hr, const char* expr, const char* file, int line, const char* function, bool& ignore_always)
{
	backend(expr, error2string(hr), 0, 0, file, line, function, ignore_always);
}

void xrDebug::error(long hr, const char* expr, const char* e2, const char* file, int line, const char* function,
                    bool& ignore_always)
{
	backend(expr, error2string(hr), e2, 0, file, line, function, ignore_always);
}

void xrDebug::fail(const char* e1, const char* file, int line, const char* function, bool& ignore_always)
{
	backend(e1, "assertion failed", 0, 0, file, line, function, ignore_always);
}

void xrDebug::fail(const char* e1, const std::string& e2, const char* file, int line, const char* function,
                   bool& ignore_always)
{
	backend(e1, e2.c_str(), 0, 0, file, line, function, ignore_always);
}

void xrDebug::fail(const char* e1, const char* e2, const char* file, int line, const char* function,
                   bool& ignore_always)
{
	backend(e1, e2, 0, 0, file, line, function, ignore_always);
}

void xrDebug::fail(const char* e1, const char* e2, const char* e3, const char* file, int line, const char* function,
                   bool& ignore_always)
{
	backend(e1, e2, e3, 0, file, line, function, ignore_always);
}

void xrDebug::fail(const char* e1, const char* e2, const char* e3, const char* e4, const char* file, int line,
                   const char* function, bool& ignore_always)
{
	backend(e1, e2, e3, e4, file, line, function, ignore_always);
}

bool ignore_verify = true;

//AVO: print, dont crash
void xrDebug::soft_fail(LPCSTR e1, LPCSTR file, int line, LPCSTR function)
{
	if (!ignore_verify)
	Msg("! VERIFY_FAILED: %s[%d] {%s}  %s", file, line, function, e1);
}

void xrDebug::soft_fail(LPCSTR e1, const std::string& e2, LPCSTR file, int line, LPCSTR function)
{
	if (!ignore_verify)
	Msg("! VERIFY_FAILED: %s[%d] {%s}  %s %s", file, line, function, e1, e2.c_str());
}

void xrDebug::soft_fail(LPCSTR e1, LPCSTR e2, LPCSTR file, int line, LPCSTR function)
{
	if (!ignore_verify)
	Msg("! VERIFY_FAILED: %s[%d] {%s}  %s %s", file, line, function, e1, e2);
}

void xrDebug::soft_fail(LPCSTR e1, LPCSTR e2, LPCSTR e3, LPCSTR file, int line, LPCSTR function)
{
	if (!ignore_verify)
	Msg("! VERIFY_FAILED: %s[%d] {%s}  %s %s %s", file, line, function, e1, e2, e3);
}

void xrDebug::soft_fail(LPCSTR e1, LPCSTR e2, LPCSTR e3, LPCSTR e4, LPCSTR file, int line, LPCSTR function)
{
	if (!ignore_verify)
	Msg("! VERIFY_FAILED: %s[%d] {%s}  %s %s %s %s", file, line, function, e1, e2, e3, e4);
}

void xrDebug::soft_fail(LPCSTR e1, LPCSTR e2, LPCSTR e3, LPCSTR e4, LPCSTR e5, LPCSTR file, int line, LPCSTR function)
{
	if (!ignore_verify)
	Msg("! VERIFY_FAILED: %s[%d] {%s}  %s %s %s %s %s", file, line, function, e1, e2, e3, e4, e5);
}

//-AVO

void __cdecl xrDebug::fatal(const char* file, int line, const char* function, const char* F, ...)
{
	string1024 buffer;

	va_list p;
	va_start(p, F);
	vsprintf(buffer, F, p);
	va_end(p);

	bool ignore_always = true;

	backend(nullptr, "fatal error", buffer, 0, file, line, function, ignore_always);
}

typedef void (*full_memory_stats_callback_type)();
XRCORE_API full_memory_stats_callback_type g_full_memory_stats_callback = 0;

int out_of_memory_handler(size_t size)
{
	Msg("* [x-ray]: OOM requesting %lld bytes", size);

	if (g_full_memory_stats_callback)
		g_full_memory_stats_callback();
	else
	{
		Memory.mem_compact();
		size_t process_heap = Memory.mem_usage();
		int eco_strings = (int)g_pStringContainer->stat_economy();
		int eco_smem = (int)g_pSharedMemoryContainer->stat_economy();
		Msg("* [x-ray]: process heap[%llu K]", process_heap / 1024, process_heap / 1024);
		Msg("* [x-ray]: economy: strings[%lld K], smem[%lld K]", eco_strings / 1024, eco_smem);
	}

	Debug.fatal(DEBUG_INFO, "Out of memory. Memory request: %lld K", size / 1024);
	return 1;
}

extern LPCSTR log_name();

XRCORE_API string_path g_bug_report_file;

void CALLBACK PreErrorHandler(INT_PTR)
{
#ifdef USE_BUG_TRAP
    if (!xr_FS || !FS.m_Flags.test(CLocatorAPI::flReady))
        return;

    string_path log_folder;

    __try
    {
        FS.update_path(log_folder, "$logs$", "");
        if ((log_folder[0] != '\\') && (log_folder[1] != ':'))
        {
            string256 current_folder;
            _getcwd(current_folder, sizeof(current_folder));

            string256 relative_path;
            xr_strcpy(relative_path, sizeof(relative_path), log_folder);
            strconcat(sizeof(log_folder), log_folder, current_folder, "\\", relative_path);
        }
    }
    __except (EXCEPTION_EXECUTE_HANDLER)
    {
        xr_strcpy(log_folder, sizeof(log_folder), "logs");
    }

    string_path temp;
    strconcat(sizeof(temp), temp, log_folder, log_name());
    BT_AddLogFile(temp);

    if (*g_bug_report_file)
        BT_AddLogFile(g_bug_report_file);

    BT_SaveSnapshot(0);
#endif // USE_BUG_TRAP
}

#ifdef USE_BUG_TRAP
void SetupExceptionHandler(const bool& dedicated)
{
	UINT prevMode = SetErrorMode(SEM_NOGPFAULTERRORBOX);
	SetErrorMode(prevMode|SEM_NOGPFAULTERRORBOX);
    BT_InstallSehFilter();
#if 1//ndef USE_OWN_ERROR_MESSAGE_WINDOW
    if (!dedicated && !strstr(GetCommandLine(), "-silent_error_mode"))
        BT_SetActivityType(BTA_SHOWUI);
    else
        BT_SetActivityType(BTA_SAVEREPORT);
#else // USE_OWN_ERROR_MESSAGE_WINDOW
    BT_SetActivityType (BTA_SAVEREPORT);
#endif // USE_OWN_ERROR_MESSAGE_WINDOW

    BT_SetDialogMessage(
        BTDM_INTRO2,
        "\
                                                This is X-Ray Engine v1.6 crash reporting client. \
                                                                                                                                                                        To help the development process, \
                                                                                                                                                                                                                                                                                                                                                                                                                                                                please Submit Bug or save report and email it manually (button More...).\
                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                \r\nMany thanks in advance and sorry for the inconvenience."
                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                );

    BT_SetPreErrHandler(PreErrorHandler, 0);
    BT_SetAppName("XRay Engine");
    BT_SetReportFormat(BTRF_TEXT);
    BT_SetFlags(/**/BTF_DETAILEDMODE | /**BTF_EDIETMAIL | /**/BTF_ATTACHREPORT /**| BTF_LISTPROCESSES /**| BTF_SHOWADVANCEDUI /**| BTF_SCREENCAPTURE/**/);

    u32 const minidump_flags =
#ifndef MASTER_GOLD
        (
        MiniDumpWithDataSegs |
        // MiniDumpWithFullMemory |
// MiniDumpWithHandleData |
// MiniDumpFilterMemory |
// MiniDumpScanMemory |
// MiniDumpWithUnloadedModules |
# ifndef _EDITOR
        MiniDumpWithIndirectlyReferencedMemory |
# endif // _EDITOR
// MiniDumpFilterModulePaths |
// MiniDumpWithProcessThreadData |
// MiniDumpWithPrivateReadWriteMemory |
// MiniDumpWithoutOptionalData |
// MiniDumpWithFullMemoryInfo |
// MiniDumpWithThreadInfo |
        // MiniDumpWithCodeSegs |
        0
        );
#else // #ifndef MASTER_GOLD
        dedicated ?
    MiniDumpNoDump :
                   (
                   MiniDumpWithDataSegs |
                   // MiniDumpWithFullMemory |
// MiniDumpWithHandleData |
// MiniDumpFilterMemory |
// MiniDumpScanMemory |
// MiniDumpWithUnloadedModules |
# ifndef _EDITOR
                   MiniDumpWithIndirectlyReferencedMemory |
# endif // _EDITOR
// MiniDumpFilterModulePaths |
// MiniDumpWithProcessThreadData |
// MiniDumpWithPrivateReadWriteMemory |
// MiniDumpWithoutOptionalData |
// MiniDumpWithFullMemoryInfo |
// MiniDumpWithThreadInfo |
                   // MiniDumpWithCodeSegs |
                   0
                   );
#endif // #ifndef MASTER_GOLD

    BT_SetDumpType(minidump_flags);
    BT_SetSupportEMail("cop-crash-report@stalker-game.com");
    // BT_SetSupportServer ("localhost", 9999);
    // BT_SetSupportURL ("www.gsc-game.com");
}
#endif //-USE_BUG_TRAP

//extern void BuildStackTrace(struct _EXCEPTION_POINTERS* pExceptionInfo);
typedef LONG WINAPI UnhandledExceptionFilterType(struct _EXCEPTION_POINTERS* pExceptionInfo);
typedef LONG (__stdcall* PFNCHFILTFN)(EXCEPTION_POINTERS* pExPtrs);
extern "C" BOOL __stdcall SetCrashHandlerFilter(PFNCHFILTFN pFn);

static UnhandledExceptionFilterType* previous_filter = 0;

#ifdef USE_OWN_MINI_DUMP
typedef BOOL (WINAPI* MINIDUMPWRITEDUMP)(HANDLE hProcess, DWORD dwPid, HANDLE hFile, MINIDUMP_TYPE DumpType,
    CONST PMINIDUMP_EXCEPTION_INFORMATION ExceptionParam,
    CONST PMINIDUMP_USER_STREAM_INFORMATION UserStreamParam,
    CONST PMINIDUMP_CALLBACK_INFORMATION CallbackParam
    );

void save_mini_dump (_EXCEPTION_POINTERS* pExceptionInfo)
{
    // firstly see if dbghelp.dll is around and has the function we need
// look next to the EXE first, as the one in System32 might be old
    // (e.g. Windows 2000)
    HMODULE hDll = NULL;
    string_path szDbgHelpPath;

    if (GetModuleFileName( NULL, szDbgHelpPath, _MAX_PATH ))
    {
        char* pSlash = strchr( szDbgHelpPath, '\\' );
        if (pSlash)
        {
            xr_strcpy (pSlash+1, sizeof(szDbgHelpPath)-(pSlash - szDbgHelpPath), "DBGHELP.DLL" );
            hDll = ::LoadLibrary( szDbgHelpPath );
        }
    }

    if (hDll==NULL)
    {
        // load any version we can
        hDll = ::LoadLibrary( "DBGHELP.DLL" );
    }

    LPCTSTR szResult = NULL;

    if (hDll)
    {
        MINIDUMPWRITEDUMP pDump = (MINIDUMPWRITEDUMP)::GetProcAddress( hDll, "MiniDumpWriteDump" );
        if (pDump)
        {
            string_path szDumpPath;
            string_path szScratch;
            string64 t_stemp;

            timestamp (t_stemp);
            xr_strcpy ( szDumpPath, Core.ApplicationName);
            xr_strcat ( szDumpPath, "_" );
            xr_strcat ( szDumpPath, Core.UserName );
            xr_strcat ( szDumpPath, "_" );
            xr_strcat ( szDumpPath, t_stemp );
            xr_strcat ( szDumpPath, ".mdmp" );

            __try
            {
                if (FS.path_exist("$logs$"))
                    FS.update_path (szDumpPath,"$logs$",szDumpPath);
            }
            __except( EXCEPTION_EXECUTE_HANDLER )
            {
                string_path temp;
                xr_strcpy (temp,szDumpPath);
                xr_strcpy (szDumpPath,"logs/");
                xr_strcat (szDumpPath,temp);
            }

            // create the file
            HANDLE hFile = ::CreateFile( szDumpPath, GENERIC_WRITE, FILE_SHARE_WRITE, NULL, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL );
            if (INVALID_HANDLE_VALUE==hFile)
            {
                // try to place into current directory
                MoveMemory (szDumpPath,szDumpPath+5,strlen(szDumpPath));
                hFile = ::CreateFile( szDumpPath, GENERIC_WRITE, FILE_SHARE_WRITE, NULL, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL );
            }
            if (hFile!=INVALID_HANDLE_VALUE)
            {
                _MINIDUMP_EXCEPTION_INFORMATION ExInfo;

                ExInfo.ThreadId = ::GetCurrentThreadId();
                ExInfo.ExceptionPointers = pExceptionInfo;
                ExInfo.ClientPointers = NULL;

                // write the dump
                MINIDUMP_TYPE dump_flags = MINIDUMP_TYPE(MiniDumpNormal | MiniDumpFilterMemory | MiniDumpScanMemory );

                BOOL bOK = pDump( GetCurrentProcess(), GetCurrentProcessId(), hFile, dump_flags, &ExInfo, NULL, NULL );
                if (bOK)
                {
                    xr_sprintf( szScratch, "Saved dump file to '%s'", szDumpPath );
                    szResult = szScratch;
                    // retval = EXCEPTION_EXECUTE_HANDLER;
                }
                else
                {
                    xr_sprintf( szScratch, "Failed to save dump file to '%s' (error %d)", szDumpPath, GetLastError() );
                    szResult = szScratch;
                }
                ::CloseHandle(hFile);
            }
            else
            {
                xr_sprintf( szScratch, "Failed to create dump file '%s' (error %d)", szDumpPath, GetLastError() );
                szResult = szScratch;
            }
        }
        else
        {
            szResult = "DBGHELP.DLL too old";
        }
    }
    else
    {
        szResult = "DBGHELP.DLL not found";
    }
}
#endif //-USE_OWN_MINI_DUMP

void format_message(LPSTR buffer, const u32& buffer_size)
{
	LPVOID message;
	DWORD error_code = GetLastError();

	if (!error_code)
	{
		*buffer = 0;
		return;
	}

	FormatMessage(
		FORMAT_MESSAGE_ALLOCATE_BUFFER |
		FORMAT_MESSAGE_FROM_SYSTEM,
		NULL,
		error_code,
		MAKELANGID(LANG_NEUTRAL, SUBLANG_DEFAULT),
		(LPSTR)&message,
		0,
		NULL
	);

	xr_sprintf(buffer, buffer_size, "[error][%8d] : %s", error_code, message);
	LocalFree(message);
}

#ifndef _EDITOR
#include <errorrep.h>
#pragma comment( lib, "faultrep.lib" )
#endif //-!_EDITOR

#ifdef NO_BUG_TRAP
//AVO: simplify function
extern XRCORE_API DWORD MainThreadID;
extern XRCORE_API DWORD WUIThreadID;
LONG WINAPI UnhandledFilter(_EXCEPTION_POINTERS* pExceptionInfo)
{
    if (shared_str_initialized)
    {
        if (MainThreadID == GetCurrentThreadId() || WUIThreadID == GetCurrentThreadId())
            Msg("MainThreadException:");
        else
            Msg("ThreadException:");
    }

	string256 error_message;
	format_message(error_message, sizeof(error_message));

	CONTEXT save = *pExceptionInfo->ContextRecord;
	//    BuildStackTrace(pExceptionInfo);
	*pExceptionInfo->ContextRecord = save;

	if (shared_str_initialized)
		Msg("stack trace:\n");

	if (!IsDebuggerPresent())
	{
		os_clipboard::copy_to_clipboard("stack trace:\r\n\r\n");
	}

	//    string4096 buffer;
	//    for (int i = 0; i < g_stackTraceCount; ++i)
	//    {
	//        if (shared_str_initialized)
	//            Msg("%s", g_stackTrace[i]);
	//        xr_sprintf(buffer, sizeof(buffer), "%s\r\n", g_stackTrace[i]);
	//#ifdef DEBUG
	//        if (!IsDebuggerPresent())
	//            os_clipboard::update_clipboard(buffer);
	//#endif //-DEBUG
	//    }

	if (*error_message)
	{
		if (shared_str_initialized)
			Msg("\n%s", error_message);

		xr_strcat(error_message, sizeof(error_message), "\r\n");
#ifdef DEBUG
        if (!IsDebuggerPresent())
            os_clipboard::update_clipboard(buffer);
#endif //-DEBUG
	}
    char* exception_type = "Unknown exception";
	if (pExceptionInfo->ExceptionRecord)
	{
        switch (pExceptionInfo->ExceptionRecord->ExceptionCode)
        {
        case EXCEPTION_ACCESS_VIOLATION:
            exception_type = "Error: Memory access violation";
            break;
        case EXCEPTION_ARRAY_BOUNDS_EXCEEDED:
            exception_type = "Error: Array index out of bounds";
            break;
        case EXCEPTION_FLT_DIVIDE_BY_ZERO:
        case EXCEPTION_INT_DIVIDE_BY_ZERO:
            exception_type = "Error: Division by zero";
            break;
        case EXCEPTION_STACK_OVERFLOW:
            exception_type = "Error: Stack overflow";
            break;
        case EXCEPTION_ILLEGAL_INSTRUCTION:
            exception_type = "Error: Illegal instruction";
            break;
        case EXCEPTION_DATATYPE_MISALIGNMENT:
            exception_type = "Error: Unaligned data access";
            break;
        case EXCEPTION_NONCONTINUABLE_EXCEPTION:
            exception_type = "Error: Attempting to continue execution after an unrecoverable exception.";
            break;
        }
	}

    xr_StackWalker s = xr_StackWalker();
    s.ShowCallstack();

    if (shared_str_initialized)
    {
        Msg(exception_type);
        Msg(s.buffer.c_str());
        os_clipboard::copy_to_clipboard(s.buffer.c_str());
        FlushLog();
    }

    ShowCursor(true);
    ShowWindow(GetActiveWindow(), SW_FORCEMINIMIZE);

    s.buffer += "\nPress «OK» to continue or «Cancel» to abort execution\n";
    CustomMessageBox msg_box = CustomMessageBox();
    auto result = msg_box.Show("Fatal Unhandled Error", s.buffer.c_str());

    if (result == IDCANCEL)
    {
# ifdef USE_OWN_MINI_DUMP
        save_mini_dump(pExceptionInfo);
# endif // USE_OWN_MINI_DUMP
        if (IsDebuggerPresent())
            DebugBreak();
        else
            TerminateProcess(GetCurrentProcess(), 1);
    }
    if (result == IDOK)
        ShowCursor(FALSE);
# ifdef USE_OWN_MINI_DUMP
    save_mini_dump(pExceptionInfo);
# endif // USE_OWN_MINI_DUMP

	return (EXCEPTION_EXECUTE_HANDLER);
}

//-AVO
#else
LONG WINAPI UnhandledFilter(_EXCEPTION_POINTERS* pExceptionInfo)
{
    string256 error_message;
    format_message(error_message, sizeof(error_message));

    if (!error_after_dialog && !strstr(GetCommandLine(), "-no_call_stack_assert"))
    {
        CONTEXT save = *pExceptionInfo->ContextRecord;
        BuildStackTrace(pExceptionInfo);
        *pExceptionInfo->ContextRecord = save;

        if (shared_str_initialized)
            Msg("stack trace:\n");

        if (!IsDebuggerPresent())
        {
            os_clipboard::copy_to_clipboard("stack trace:\r\n\r\n");
        }

        string4096 buffer;
        for (int i = 0; i < g_stackTraceCount; ++i)
        {
            if (shared_str_initialized)
                Msg("%s", g_stackTrace[i]);
            xr_sprintf(buffer, sizeof(buffer), "%s\r\n", g_stackTrace[i]);
#ifdef DEBUG
            if (!IsDebuggerPresent())
                os_clipboard::update_clipboard(buffer);
#endif //-DEBUG
        }

        if (*error_message)
        {
            if (shared_str_initialized)
                Msg("\n%s", error_message);

            xr_strcat(error_message, sizeof(error_message), "\r\n");
#ifdef DEBUG
            if (!IsDebuggerPresent())
                os_clipboard::update_clipboard(buffer);
#endif //-DEBUG
        }
    }

    if (shared_str_initialized)
        FlushLog();

#ifndef USE_OWN_ERROR_MESSAGE_WINDOW
# ifdef USE_OWN_MINI_DUMP
    save_mini_dump (pExceptionInfo);
# endif // USE_OWN_MINI_DUMP
#else // USE_OWN_ERROR_MESSAGE_WINDOW
    if (!error_after_dialog)
    {
        if (Debug.get_on_dialog())
            Debug.get_on_dialog() (true);
        MessageBox(
            NULL,
            "Fatal error occured\n\nPress OK to abort program execution",
            "Fatal Error",
            MB_OK | MB_ICONERROR | MB_SYSTEMMODAL
            );
    }

#endif // USE_OWN_ERROR_MESSAGE_WINDOW

#ifndef _EDITOR
    ReportFault(pExceptionInfo, 0);
#endif

    if (!previous_filter)
    {
#ifdef USE_OWN_ERROR_MESSAGE_WINDOW
        if (Debug.get_on_dialog())
            Debug.get_on_dialog() (false);
#endif // USE_OWN_ERROR_MESSAGE_WINDOW

        return (EXCEPTION_CONTINUE_SEARCH);
    }

    previous_filter(pExceptionInfo);

#ifdef USE_OWN_ERROR_MESSAGE_WINDOW
    if (Debug.get_on_dialog())
        Debug.get_on_dialog() (false);
#endif // USE_OWN_ERROR_MESSAGE_WINDOW
    return (EXCEPTION_CONTINUE_SEARCH);
}
#endif //-NO_BUG_TRAP

//////////////////////////////////////////////////////////////////////
#ifdef M_BORLAND
namespace std
{
    extern new_handler _RTLENTRY _EXPFUNC set_new_handler(new_handler new_p);
};

static void __cdecl def_new_handler()
{
    FATAL ("Out of memory.");
}

void xrDebug::_initialize (const bool& dedicated)
{
    handler = 0;
    m_on_dialog = 0;
    std::set_new_handler (def_new_handler); // exception-handler for 'out of memory' condition
    // ::SetUnhandledExceptionFilter (UnhandledFilter); // exception handler to all "unhandled" exceptions
}
#else
typedef int (__cdecl* _PNH)(size_t);
_CRTIMP int __cdecl _set_new_mode(int);
//_CRTIMP _PNH __cdecl _set_new_handler(_PNH);

#ifdef LEGACY_CODE
#ifndef USE_BUG_TRAP
void _terminate()
{
    if (strstr(GetCommandLine(),"-silent_error_mode"))
        exit (-1);

    string4096 assertion_info;

    Debug.gather_info (
        nullptr,
        "Unexpected application termination",
        0,
        0,
#ifdef ANONYMOUS_BUILD
        "",
        0,
#else
        __FILE__,
        __LINE__,
#endif
#ifndef _EDITOR
        __FUNCTION__,
#else // _EDITOR
        "",
#endif // _EDITOR
        assertion_info
        );

    LPCSTR endline = "\r\n";
    LPSTR buffer = assertion_info + xr_strlen(assertion_info);
    buffer += xr_sprintf(buffer, sizeof(buffer), "Press OK to abort execution%s",endline);

    MessageBox (
        GetTopWindow(NULL),
        assertion_info,
        "Fatal Error",
        MB_OK|MB_ICONERROR|MB_SYSTEMMODAL
        );

    exit (-1);
    // FATAL ("Unexpected application termination");
}
#endif //-!USE_BUG_TRAP
#endif //-LEGACY_CODE

static void handler_base(LPCSTR reason_string)
{
	bool ignore_always = false;
	Debug.backend(
		nullptr,
		reason_string,
		0,
		0,
		DEBUG_INFO,
		ignore_always
	);
}

static void invalid_parameter_handler(
	const wchar_t* expression,
	const wchar_t* function,
	const wchar_t* file,
	unsigned int line,
	uintptr_t reserved
)
{
	bool ignore_always = false;

	string4096 expression_;
	string4096 function_;
	string4096 file_;
	size_t converted_chars = 0;
	// errno_t err =
	if (expression)
		wcstombs_s(
			&converted_chars,
			expression_,
			sizeof(expression_),
			expression,
			(wcslen(expression) + 1) * 2 * sizeof(char)
		);
	else
		xr_strcpy(expression_, "");

	if (function)
		wcstombs_s(
			&converted_chars,
			function_,
			sizeof(function_),
			function,
			(wcslen(function) + 1) * 2 * sizeof(char)
		);
	else
		xr_strcpy(function_, __FUNCTION__);

	if (file)
		wcstombs_s(
			&converted_chars,
			file_,
			sizeof(file_),
			file,
			(wcslen(file) + 1) * 2 * sizeof(char)
		);
	else
	{
		line = __LINE__;
		xr_strcpy(file_, __FILE__);
	}

	Debug.backend(
		expression_,
		"invalid parameter",
		0,
		0,
		file_,
		line,
		function_,
		ignore_always
	);
}

static void pure_call_handler()
{
	handler_base("pure virtual function call");
}

#ifdef XRAY_USE_EXCEPTIONS
static void unexpected_handler()
{
    handler_base("unexpected program termination");
}
#endif // XRAY_USE_EXCEPTIONS

static void abort_handler(int signal)
{
	handler_base("application is aborting");
}

static void floating_point_handler(int signal)
{
	handler_base("floating point error");
}

static void illegal_instruction_handler(int signal)
{
	handler_base("illegal instruction");
}

// static void storage_access_handler (int signal)
// {
// handler_base ("illegal storage access");
// }

static void termination_handler(int signal)
{
	handler_base("termination with exit code 3");
}

void debug_on_thread_spawn()
{
#ifdef USE_BUG_TRAP
    BT_SetTerminate();
#else // USE_BUG_TRAP
	//std::set_terminate (_terminate);
#endif // USE_BUG_TRAP

	_set_abort_behavior(0, _WRITE_ABORT_MSG | _CALL_REPORTFAULT);
	signal(SIGABRT, abort_handler);
	signal(SIGABRT_COMPAT, abort_handler);
	signal(SIGFPE, floating_point_handler);
	signal(SIGILL, illegal_instruction_handler);
	signal(SIGINT, 0);
	// signal (SIGSEGV, storage_access_handler);
	signal(SIGTERM, termination_handler);

	_set_invalid_parameter_handler(&invalid_parameter_handler);

	_set_new_mode(1);
	_set_new_handler(&out_of_memory_handler);
	// std::set_new_handler (&std_out_of_memory_handler);

	_set_purecall_handler(&pure_call_handler);

#if 0// should be if we use exceptions
    std::set_unexpected(_terminate);
#endif
}

void xrDebug::_initialize(const bool& dedicated)
{
	static bool is_dedicated = dedicated;

	*g_bug_report_file = 0;

	debug_on_thread_spawn();

#ifdef USE_BUG_TRAP
    SetupExceptionHandler(is_dedicated);
#endif // USE_BUG_TRAP
	previous_filter = ::SetUnhandledExceptionFilter(UnhandledFilter); // exception handler to all "unhandled" exceptions

#if 0
    struct foo
    {
        static void recurs(const u32& count)
        {
            if (!count)
                return;

            _alloca (4096);
            recurs (count - 1);
        }
    };
    foo::recurs (u32(-1));
    std::terminate ();
#endif // 0
}
#endif
