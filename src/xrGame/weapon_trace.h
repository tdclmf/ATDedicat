#pragma once

extern ENGINE_API BOOL g_wpn_trace;

// Weapon trace is temporarily silenced to keep logs readable during networking/AI debugging.
#define WPN_TRACE(...) \
	do \
	{ \
		/* if (g_wpn_trace) \
			Msg("[WPN_TRACE] " __VA_ARGS__); */ \
	} while (0)
