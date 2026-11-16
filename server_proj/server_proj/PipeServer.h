##pragma once
#ifndef PIPE_SERVER_INCLUDE
#define PIPE_SERVER_INCLUDE

#include <windows.h>

#define PIPE_ERROR                  -1
#define PIPE_NOT_CONNECTED          1
#define PIPE_CONNECTED              2
#define PIPE_LOST_CONNECT           3

#define PIPE_OPERATION_ERROR        -2
#define PIPE_JUST_CONNECTED         10
#define PIPE_NO_OPERATION           11

#define PIPE_READ_SUCCESS           12
#define PIPE_READ_PART              13


#define DEF_BUF_SIZE                16384
#define DEF_WAIT_TIME               20000

//------------------------------------------------------------------

template <class T> class CPipeServer
{
    // Опис недоступних користувачу полів класу

    // Дескриптор іменованого каналу
    HANDLE hPipe;

    // Екземпляр структури OVERLAPPED для роботи в асинхронному режимі
    OVERLAPPED Overl;

    DWORD PipeState;
    bool fPendingIOComplete;
    bool CanCloseFlag;
    DWORD PipeCurOperState;
    PSECURITY_DESCRIPTOR pSD;

    //------------------------------------------------------------------

    void CheckError()
    {
        DWORD Error = GetLastError();

        switch (Error)
        {
        case ERROR_IO_PENDING:
            PipeCurOperState = PIPE_NO_OPERATION;
            fPendingIOComplete = false;
            break;

        case ERROR_BROKEN_PIPE:
            PipeState = PIPE_LOST_CONNECT;
            fPendingIOComplete = true;
            break;

        default:
            PipeCurOperState = PIPE_OPERATION_ERROR;
            fPendingIOComplete = true;
            break;
        }
    }

    //------------------------------------------------------------------

    void ClearOVERL()
    {
        Overl.Offset = Overl.OffsetHigh = Overl.Internal = Overl.InternalHigh = 0;
    }

    //------------------------------------------------------------------

public:

    CPipeServer()
    {
        hPipe = INVALID_HANDLE_VALUE;
        PipeState = PIPE_NOT_CONNECTED;
        PipeCurOperState = PIPE_NO_OPERATION;
        ClearOVERL();
        fPendingIOComplete = true;
        Overl.hEvent = NULL;
        CanCloseFlag = false;
        pSD = NULL;
    }

    ~CPipeServer()
    {
        if (hPipe != INVALID_HANDLE_VALUE)
        {
            CancelIo(hPipe);
            CloseHandle(hPipe);
        }

        if (pSD)
            HeapFree(GetProcessHeap(), HEAP_NO_SERIALIZE, pSD);
    }

    //------------------------------------------------------------------

    DWORD CreatePipeAndWaitClient(char* PipeName, HANDLE hEvent, bool ByteMode = true, DWORD BufSize = DEF_BUF_SIZE, DWORD DefWaitTime = DEF_WAIT_TIME)
    {
        if (hEvent)
        {
            ClearOVERL();
            Overl.hEvent = hEvent;

            if (!pSD)
                pSD = HeapAlloc(GetProcessHeap(), HEAP_NO_SERIALIZE | HEAP_ZERO_MEMORY, SECURITY_DESCRIPTOR_MIN_LENGTH);

            if (pSD && InitializeSecurityDescriptor(pSD, SECURITY_DESCRIPTOR_REVISION) && SetSecurityDescriptorDacl(pSD, TRUE, NULL, FALSE))
            {
                SECURITY_ATTRIBUTES sa = { sizeof(sa), pSD, true };

                hPipe = CreateNamedPipeA(PipeName,
                    PIPE_ACCESS_DUPLEX | FILE_FLAG_OVERLAPPED,
                    ByteMode ? PIPE_TYPE_BYTE | PIPE_READMODE_BYTE : PIPE_TYPE_MESSAGE | PIPE_READMODE_MESSAGE,
                    PIPE_UNLIMITED_INSTANCES,
                    BufSize,
                    BufSize,
                    DefWaitTime,
                    &sa
                );

                PipeState = WaitClient();
            }
        }

        return PipeState;
    }

    //------------------------------------------------------------------

    DWORD WaitClient()
    {
        if (IsOpen())
        {
            PipeCurOperState = PIPE_NO_OPERATION;
            ClearOVERL();

            if (ConnectNamedPipe(hPipe, &Overl) == FALSE)
            {
                switch (GetLastError())
                {
                case ERROR_IO_PENDING:
                    fPendingIOComplete = false;
                    return PipeState;

                case ERROR_PIPE_CONNECTED:
                    SetEvent(Overl.hEvent);
                    PipeState = PIPE_CONNECTED;
                    CanCloseFlag = false;
                    fPendingIOComplete = true;
                    return PipeState;
                }
            }
            CanCloseFlag = false;
            fPendingIOComplete = true;
        }
        PipeState = PIPE_ERROR;

        return PipeState;
    }

    //------------------------------------------------------------------

    DWORD DisconnectClient()
    {
        if (IsOpen() && DisconnectNamedPipe(hPipe))
        {
            PipeState = PIPE_NOT_CONNECTED;
            return WaitClient();
        }
        else
        {
            fPendingIOComplete = true;
            return (PipeState = PIPE_ERROR);
        }
    }

    //------------------------------------------------------------------

    bool ReadMessage(T& Message)
    {
        DWORD NBytesRead;

        if (IsOpen())
        {
            if (ReadFile(hPipe, (LPVOID)(&Message), sizeof(T), &NBytesRead, &Overl) == TRUE)
            {
                CanCloseFlag = true;
                PipeCurOperState = (NBytesRead == sizeof(T)) ? PIPE_READ_SUCCESS : PIPE_READ_PART;
                fPendingIOComplete = false;
                return true;
            }
            else
            {
                CheckError();
            }
        }
        return false;
    }

    //------------------------------------------------------------------
    // ДОДАНИЙ МЕТОД ДЛЯ ВІДПРАВКИ ВІДПОВІДІ (1 або 0)
    //------------------------------------------------------------------

    bool WriteResponse(char response)
    {
        DWORD NBytesWritten;
        if (IsOpen())
        {
            OVERLAPPED ov = { 0 };
            ov.hEvent = CreateEvent(NULL, TRUE, FALSE, NULL);

            if (WriteFile(hPipe, &response, sizeof(char), &NBytesWritten, &ov) == FALSE)
            {
                if (GetLastError() == ERROR_IO_PENDING)
                {
                    GetOverlappedResult(hPipe, &ov, &NBytesWritten, TRUE);
                }
            }
            CloseHandle(ov.hEvent);
            return true;
        }
        return false;
    }

    //------------------------------------------------------------------

    bool GetPendingResult(DWORD& NBytesRead)
    {
        if (IsOpen() && GetOverlappedResult(hPipe, &Overl, &NBytesRead, FALSE) == TRUE)
        {
            if (PipeState == PIPE_NOT_CONNECTED)
            {
                PipeState = PIPE_CONNECTED;
                PipeCurOperState = PIPE_JUST_CONNECTED;
            }
            else
            {
                CanCloseFlag = true;
            }

            fPendingIOComplete = true;
            return true;
        }
        else
        {
            CheckError();
        }

        return false;
    }

    //------------------------------------------------------------------

    bool IsOpen()
    {
        return hPipe != INVALID_HANDLE_VALUE;
    }

    DWORD GetState()
    {
        return PipeState;
    }

    DWORD GetOperState()
    {
        return PipeCurOperState;
    }

    bool GetIOComplete()
    {
        return fPendingIOComplete;
    }

    bool CanClose()
    {
        return CanCloseFlag;
    }
};

#endif