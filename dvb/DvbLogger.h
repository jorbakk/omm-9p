/***************************************************************************|
|  OMM - Open Multimedia                                                    |
|                                                                           |
|  Copyright (C) 2009, 2010, 2011, 2012, 2022                               |
|  Jörg Bakker                                                              |
|                                                                           |
|  This file is part of OMM.                                                |
|                                                                           |
|  OMM is free software: you can redistribute it and/or modify              |
|  it under the terms of the MIT License                                    |
 ***************************************************************************/

#ifndef DvbLogger_INCLUDED
#define DvbLogger_INCLUDED

#include <Poco/Logger.h>
#include <Poco/Format.h>
#include <Poco/StringTokenizer.h>
#include <Poco/TextConverter.h>
#include <Poco/TextEncoding.h>
#include <Poco/UTF8Encoding.h>

#include "../Log.h"

namespace Omm {
namespace Dvb {


#ifndef NDEBUG
class Log
{
public:
    static Log* instance();

    Poco::Logger& dvb();

private:
    Log();

    static Log*     _pInstance;
    Poco::Logger*   _pDvbLogger;
};
#endif // NDEBUG


}  // namespace Omm
}  // namespace Dvb

#endif
