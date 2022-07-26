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

#define __STDC_CONSTANT_MACROS
#define __STDC_FORMAT_MACROS

#include <Poco/Exception.h>
#include <Poco/Thread.h>
#include <Poco/ClassLoader.h>
#include <Poco/FormattingChannel.h>
#include <Poco/PatternFormatter.h>
#include <Poco/SplitterChannel.h>
#include <Poco/ConsoleChannel.h>

#include <fstream>
#include <cstring>
#include <cmath>

#include "AvStream.h"
#include "Log.h"

namespace Omm {
namespace AvStream {


RingBuffer::RingBuffer(int size) :
_ringBuffer(new char[size]),
_readPos(_ringBuffer),
_writePos(_ringBuffer),
_size(size)
{
}


RingBuffer::~RingBuffer()
{
    delete _ringBuffer;
}


void
RingBuffer::read(char* buffer, int num)
{
    int relReadPos = _readPos - _ringBuffer;
    if (relReadPos + num >= _size) {
        int firstHalf = _size - relReadPos;
        int secondHalf = num - firstHalf;
        memcpy(buffer, _readPos, firstHalf);
        memcpy(buffer + firstHalf, _ringBuffer, secondHalf);
        _readPos = _ringBuffer + secondHalf;
    }
    else {
        memcpy(buffer, _readPos, num);
        _readPos += num;
    }
}


void
RingBuffer::write(const char* buffer, int num)
{
    int relWritePos = _writePos - _ringBuffer;
    if (relWritePos + num >= _size) {
        int firstHalf = _size - relWritePos;
        int secondHalf = num - firstHalf;
        memcpy(_writePos, buffer, firstHalf);
        memcpy(_ringBuffer, buffer + firstHalf, secondHalf);
        _writePos = _ringBuffer + secondHalf;
    }
    else {
        memcpy(_writePos, buffer, num);
        _writePos += num;
    }
}


void
RingBuffer::clear()
{
    _readPos = _ringBuffer;
    _writePos = _ringBuffer;
}


ByteQueue::ByteQueue(int size) :
_ringBuffer(size),
_size(size),
_level(0)
{
}


void
ByteQueue::read(char* buffer, int num)
{
    LOG(avstream, trace, "byte queue read, num bytes: " + Poco::NumberFormatter::format(num));
    int bytesRead = 0;
    while (bytesRead < num) {
        LOG(avstream, trace, "byte queue read -> readSome, trying to read: " + Poco::NumberFormatter::format(num - bytesRead) + " bytes");
        bytesRead += readSome(buffer + bytesRead, num - bytesRead);
    }
    LOG(avstream, trace, "byte queue read finished.");
}


void
ByteQueue::write(const char* buffer, int num)
{
    LOG(avstream, trace, "byte queue write, num bytes: " + Poco::NumberFormatter::format(num));
    int bytesWritten = 0;
    while (bytesWritten < num) {
        LOG(avstream, trace, "byte queue write -> writeSome, trying to write: " + Poco::NumberFormatter::format(num - bytesWritten) + " bytes");
        bytesWritten += writeSome(buffer + bytesWritten, num - bytesWritten);
    }
    LOG(avstream, trace, "byte queue write finished.");
}


int
ByteQueue::readSome(char* buffer, int num)
{
    _lock.lock();
    if (_level == 0) {
        LOG(avstream, trace, "byte queue readSome() try to read " + Poco::NumberFormatter::format(num) + " bytes, level: " + Poco::NumberFormatter::format(_level));
        // block byte queue for further reading
        _readCondition.wait<Poco::FastMutex>(_lock);
        LOG(avstream, trace, "byte queue readSome() wait over, now reading " + Poco::NumberFormatter::format(num) + " bytes, level: " + Poco::NumberFormatter::format(_level));
    }

    int bytesRead = (_level < num) ? _level : num;
    _ringBuffer.read(buffer, bytesRead);
    _level -= bytesRead;

    LOG(avstream, trace, "byte queue readSome() read " + Poco::NumberFormatter::format(bytesRead) + " bytes, level: " + Poco::NumberFormatter::format(_level));

    // we've read some bytes, so we can put something in, again
    if (_level < _size) {
        _writeCondition.broadcast();
    }
    _lock.unlock();
    return bytesRead;
}


int
ByteQueue::writeSome(const char* buffer, int num)
{
    _lock.lock();
    if (_level == _size) {
        // block byte queue for further writing
        LOG(avstream, trace, "byte queue writeSome() try to write " + Poco::NumberFormatter::format(num) + " bytes, level: " + Poco::NumberFormatter::format(_level));
        _writeCondition.wait<Poco::FastMutex>(_lock);
        LOG(avstream, trace, "byte queue writeSome() wait over, now writing " + Poco::NumberFormatter::format(num) + " bytes, level: " + Poco::NumberFormatter::format(_level));
    }

    int bytesWritten = (_size - _level < num) ? (_size - _level) : num;
    _ringBuffer.write(buffer, bytesWritten);
    _level += bytesWritten;

    LOG(avstream, trace, "byte queue writeSome() wrote " + Poco::NumberFormatter::format(bytesWritten) + " bytes, level: " + Poco::NumberFormatter::format(_level));

    // we've written some bytes, so we can get something out, again
    if (_level > 0) {
        _readCondition.broadcast();
    }
    _lock.unlock();
    return bytesWritten;
}


int
ByteQueue::size()
{
    Poco::ScopedLock<Poco::FastMutex> lock(_lock);
    return _size;
}


int
ByteQueue::level()
{
    Poco::ScopedLock<Poco::FastMutex> lock(_lock);
    return _level;
}


void
ByteQueue::clear()
{
    LOG(avstream, trace, "byte queue clear");
    char buf[_level];
    read(buf, _level);
}


bool
ByteQueue::full()
{
    Poco::ScopedLock<Poco::FastMutex> lock(_lock);
    LOG(avstream, trace, "byte queue check full() at level: " + Poco::NumberFormatter::format(_level));
    return (_level == _size);
}


bool
ByteQueue::empty()
{
    Poco::ScopedLock<Poco::FastMutex> lock(_lock);
    LOG(avstream, trace, "byte queue check empty() at level: " + Poco::NumberFormatter::format(_level));
    return (_level == 0);
}


} // namespace AvStream
} // namespace Omm

