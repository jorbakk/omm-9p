/***************************************************************************|
|  OMM - Open Multimedia                                                    |
|                                                                           |
|  Copyright (C) 2009, 2010                                                 |
|  Jörg Bakker                                                              |
|                                                                           |
|  This file is part of OMM.                                                |
|                                                                           |
|  OMM is free software: you can redistribute it and/or modify              |
|  it under the terms of the MIT License                                    |
 ***************************************************************************/

#ifndef AvStream_INCLUDED
#define AvStream_INCLUDED

#include <fstream>
#include <iostream>
#include <string>
#include <vector>
#include <queue>

#include <stdint.h>

#include <Poco/Logger.h>
#include <Poco/NumberFormatter.h>
#include <Poco/Runnable.h>
#include <Poco/RunnableAdapter.h>
#include <Poco/Notification.h>
#include <Poco/Mutex.h>
#include <Poco/RWLock.h>
#include <Poco/Condition.h>
#include <Poco/Timer.h>
#include <Poco/DateTime.h>
#include <Poco/NotificationCenter.h>

#undef CC_NONE

namespace Omm {
namespace AvStream {

#ifndef NDEBUG
class Log
{
public:
    static Log* instance();

    Poco::Logger& avstream();

private:
    Log();

    static Log*     _pInstance;
    Poco::Logger*   _pAvStreamLogger;
};
#endif //NDEBUG

class RingBuffer
{
public:
    RingBuffer(int size);
    ~RingBuffer();

    /**
    NOTE: this is no generic implemenation of a ring buffer:
    1. read() and write() don't check if num > size
    2. it is not thread safe
    this is all done in the customer ByteQueue
    **/
    void read(char* buffer, int num);
    void write(const char* buffer, int num);

    void clear();

private:
    char*                   _ringBuffer;
    char*                   _readPos;
    char*                   _writePos;
    int                     _size;
};


/**
class ByteQueue - a blocking byte stream with a fixed size
**/
class ByteQueue
{
public:
    ByteQueue(int size);

    /**
    read() and write() block until num bytes have been read or written
    **/
    void read(char* buffer, int num);
    void write(const char* buffer, int num);

    /**
    readSome() and writeSome() read upto num bytes, return the number of bytes read
    and block if queue is empty / full
    **/
    int readSome(char* buffer, int num);
    int writeSome(const char* buffer, int num);

    int size();
    int level();
    void clear();

    bool full();
    bool empty();

private:
    RingBuffer              _ringBuffer;
    int                     _size;
    int                     _level;
    Poco::FastMutex         _lock;
    Poco::Condition         _writeCondition;
    Poco::Condition         _readCondition;
};

} // namespace AvStream
} // namespace Omm

#endif
