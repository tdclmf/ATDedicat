////////////////////////////////////////////////////////////////////////////
//	Module 		: object_item_client_server_inline.h
//	Created 	: 27.05.2004
//  Modified 	: 30.06.2004
//	Author		: Dmitriy Iassenev
//	Description : Object item client and server class inline functions
////////////////////////////////////////////////////////////////////////////

#ifndef object_item_client_server_inlineH
#define object_item_client_server_inlineH

#pragma once

#define TEMPLATE_SPECIALIZATION template <typename _client_type, typename _server_type>
#define CSObjectItemClientServer CObjectItemClientServer<_client_type,_server_type>

TEMPLATE_SPECIALIZATION
IC CSObjectItemClientServer::CObjectItemClientServer(const CLASS_ID& clsid, LPCSTR script_clsid) :
	inherited(clsid, script_clsid)
{
}

#ifndef NO_XR_GAME
TEMPLATE_SPECIALIZATION
ObjectFactory::CLIENT_BASE_CLASS*CSObjectItemClientServer::client_object() const
{
	return (xr_new<CLIENT_TYPE>()->_construct());
}
#endif

TEMPLATE_SPECIALIZATION
ObjectFactory::SERVER_BASE_CLASS*CSObjectItemClientServer::server_object(LPCSTR section) const
{
	ObjectFactory::SERVER_BASE_CLASS* o = xr_new<SERVER_TYPE>(section)->init();
	R_ASSERT(o);
	return (o);
}

#undef TEMPLATE_SPECIALIZATION
#undef CSObjectItemClientServer

#ifndef NO_XR_GAME
#	define TEMPLATE_SPECIALIZATION template <typename _client_type_single, typename _client_type_mp, typename _server_type_single, typename _server_type_mp>
#	define CSObjectItemClientServerSingleMp CObjectItemClientServerSingleMp<_client_type_single,_client_type_mp,_server_type_single,_server_type_mp>

TEMPLATE_SPECIALIZATION
IC CSObjectItemClientServerSingleMp::CObjectItemClientServerSingleMp(const CLASS_ID& clsid, LPCSTR script_clsid) :
	inherited(clsid, script_clsid)
{
}

TEMPLATE_SPECIALIZATION
ObjectFactory::CLIENT_BASE_CLASS*CSObjectItemClientServerSingleMp::client_object() const
{
	// Dedicated-single bridge:
	// client side of eGameIDSingle can be forced through MP actor pipeline (OnServer()==false),
	// while classic local single remains single-type (OnServer()==true).
	// For non-single game types always use MP type (vanilla/OMP behavior).
	const bool use_single_type =
		IsGameTypeSingle() &&
		OnServer();
	ObjectFactory::CLIENT_BASE_CLASS* result =
		use_single_type ? xr_new<_client_type_single>() : xr_new<_client_type_mp>();

	return (result->_construct());
}

TEMPLATE_SPECIALIZATION
ObjectFactory::SERVER_BASE_CLASS*CSObjectItemClientServerSingleMp::server_object(LPCSTR section) const
{
	// Server entity type must be selected by game type, not by runtime role.
	// Otherwise dedicated/listen MP can incorrectly receive single server entities.
	ObjectFactory::SERVER_BASE_CLASS* result =
		IsGameTypeSingle() ? xr_new<_server_type_single>(section) : xr_new<_server_type_mp>(section);

	result = result->init();
	R_ASSERT(result);
	return (result);
}

#	undef TEMPLATE_SPECIALIZATION
#	undef CSObjectItemClientServerSingleMp

#endif // NO_XR_GAME

#endif
