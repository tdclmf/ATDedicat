#ifndef PLAYER_ACCOUNT_H
#define PLAYER_ACCOUNT_H

class player_account
{
public:
	player_account();
	~player_account();

	shared_str const& name() const { return m_player_name; };
	shared_str const& clan_name() const { return m_clan_name; };

	void net_Import(NET_Packet& P);
	void net_Export(NET_Packet& P);
	void skip_Import(NET_Packet& P);
	void load_account();
	void set_player_name(char const* new_name);
protected:
	shared_str m_player_name;
	shared_str m_clan_name;
	bool m_online_account;
};
#endif
