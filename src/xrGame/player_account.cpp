#include "stdafx.h"
#include "player_account.h"
#include "MainMenu.h"

player_account::player_account() : m_player_name(""), m_clan_name(""), m_online_account(false)
{
}

player_account::~player_account() { }
void player_account::load_account()
{
}

void player_account::net_Import(NET_Packet& P)
{
	P.r_stringZ(m_player_name);
}

void player_account::net_Export(NET_Packet& P)
{
	P.w_stringZ(m_player_name);
}

void player_account::skip_Import(NET_Packet& P)
{
	xr_string skip;
	P.r_stringZ(skip);
}

void player_account::set_player_name(char const* new_name)
{
	m_player_name = new_name;
}
