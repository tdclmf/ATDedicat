#ifndef xrSyncronizeH
#define xrSyncronizeH
#pragma once

// Desc: Simple wrapper for critical section
struct XRCORE_API xrCriticalSection
{
    CRITICAL_SECTION cs;

    xrCriticalSection(xrCriticalSection const& copy) = delete; //noncopyable
    xrCriticalSection& operator=(const xrCriticalSection&) = delete;

    xrCriticalSection();
    ~xrCriticalSection();

    void Enter();
    void Leave();
    BOOL TryEnter();

    struct XRCORE_API raii
    {
        raii(xrCriticalSection*);
        raii(xrCriticalSection&);
        ~raii();
        xrCriticalSection* critical_section;
    };
};

class XRCORE_API xrSRWLock
{
public:
    class XRCORE_API raii
    {
    public:
        raii(xrSRWLock& lock, bool read = false);
        raii(xrSRWLock* lock, bool read = false);
        ~raii();

    private:
        xrSRWLock* psrwlock;
        bool read;
    };

private:
    SRWLOCK srwlock;

public:
    xrSRWLock();
    ~xrSRWLock() {};

    void EnterWrite();
    void LeaveWrite();

    void EnterRead();
    void LeaveRead();

    BOOL TryEnterWrite();
    BOOL TryEnterRead();
};
//Write functions guard: lock.EnterWrite(); ... lock.LeaveWrite();
//Read functions guard: lock.EnterRead(); ... lock.LeaveRead();

class XRCORE_API xrSRWLockGuard
{
private:
    xrSRWLock* psrwlock;
    bool read;

public:
    xrSRWLockGuard(xrSRWLock& lock, bool read = false);
    xrSRWLockGuard(xrSRWLock* lock, bool read = false);
    ~xrSRWLockGuard();
};
//Write functions guard: xrSRWLockGuard guard(lock); ...
//Read functions guard: xrSRWLockGuard guard(lock, true); ...

#endif // xrSyncronizeH