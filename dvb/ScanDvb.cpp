/***************************************************************************|
|  OMM - Open Multimedia                                                    |
|                                                                           |
|  Copyright (C) 2009, 2010, 2011                                           |
|  Jörg Bakker                                                              |
|                                                                           |
|  This file is part of OMM.                                                |
|                                                                           |
|  OMM is free software: you can redistribute it and/or modify              |
|  it under the terms of the MIT License                                    |
 ***************************************************************************/

#include <Poco/StringTokenizer.h>

#include "Device.h"
#include "Frontend.h"


int
main(int argc, char** argv)
{
    Omm::Dvb::Device* pDevice = Omm::Dvb::Device::instance();

    if (argc <= 1) {
        Omm::Dvb::Frontend::listInitialTransponderData();
        return 1;
    }
    for (int i = 1; i < argc; ++i) {
        Poco::StringTokenizer initialTransponders(argv[i], "/");
        if (initialTransponders.count() != 2) {
            std::cerr << "usage: scandvb <frontend-type1>/<transponder-list1> <frontend-type2>/<transponder-list2> ... " << std::endl;
            return 1;
        }
        pDevice->addInitialTransponders(initialTransponders[0], initialTransponders[1]);
    }

    pDevice->detectAdapters();
    pDevice->open();
    pDevice->scan();
    pDevice->writeXml(std::cout);

    return 0;
}
