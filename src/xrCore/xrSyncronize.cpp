#include "stdafx.h"

xrCriticalSection::xrCriticalSection()
{
	InitializeCriticalSectionEx(&cs, 2000, CRITICAL_SECTION_NO_DEBUG_INFO);
}

xrCriticalSection::~xrCriticalSection()
{
	DeleteCriticalSection(&cs);
}

void	xrCriticalSection::Enter()
{
	PROF_EVENT("CS_Enter")
	EnterCriticalSection(&cs);
}

void	xrCriticalSection::Leave()
{
	PROF_EVENT("CS_Leave");
	LeaveCriticalSection(&cs);
}

BOOL	xrCriticalSection::TryEnter()
{
	PROF_EVENT("CS_TryEnter");
	return TryEnterCriticalSection(&cs);
}

xrCriticalSection::raii::raii(xrCriticalSection* critical_section)
	: critical_section(critical_section)
{
	VERIFY(critical_section);
	critical_section->Enter();
}

xrCriticalSection::raii::raii(xrCriticalSection& critical_section)
	: critical_section(&critical_section)
{
	VERIFY(&critical_section);
	critical_section.Enter();
}

xrCriticalSection::raii::~raii()
{
	critical_section->Leave();
}

xrSRWLock::xrSRWLock()
{
	InitializeSRWLock(&srwlock);
}

void xrSRWLock::EnterWrite()
{
	PROF_EVENT("xrSRWLock::EnterWrite");
	AcquireSRWLockExclusive(&srwlock);
}

void xrSRWLock::LeaveWrite()
{
	PROF_EVENT("xrSRWLock::LeaveWrite");
	ReleaseSRWLockExclusive(&srwlock);
}

void xrSRWLock::EnterRead()
{
	PROF_EVENT("xrSRWLock::EnterRead");
	AcquireSRWLockShared(&srwlock);
}

void xrSRWLock::LeaveRead()
{
	PROF_EVENT("xrSRWLock::LeaveRead");
	ReleaseSRWLockShared(&srwlock);
}

BOOL xrSRWLock::TryEnterWrite()
{
	return TryAcquireSRWLockExclusive(&srwlock);
}

BOOL xrSRWLock::TryEnterRead()
{
	return TryAcquireSRWLockShared(&srwlock);
}

xrSRWLock::raii::raii(xrSRWLock& lock, bool read)
	: psrwlock(&lock), read(read)
{
	if (read)
		lock.EnterRead();
	else
		lock.EnterWrite();
}

xrSRWLock::raii::raii(xrSRWLock* lock, bool read)
	: psrwlock(lock), read(read)
{
	if (read)
		lock->EnterRead();
	else
		lock->EnterWrite();
}

xrSRWLock::raii::~raii()
{
	if (read)
		psrwlock->LeaveRead();
	else
		psrwlock->LeaveWrite();
}

xrSRWLockGuard::xrSRWLockGuard(xrSRWLock& lock, bool read)
	: psrwlock(&lock), read(read)
{
	if (read)
		lock.EnterRead();
	else
		lock.EnterWrite();
}

xrSRWLockGuard::xrSRWLockGuard(xrSRWLock* lock, bool read)
	: psrwlock(lock), read(read)
{
	if (read)
		lock->EnterRead();
	else
		lock->EnterWrite();
}

xrSRWLockGuard::~xrSRWLockGuard()
{
	if (read)
		psrwlock->LeaveRead();
	else
		psrwlock->LeaveWrite();
}