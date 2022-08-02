#include "Device.h"
#include "Frontend.h"
#include "Transponder.h"
#include "Service.h"
#include "TransportStream.h"

extern "C" {
#include "dvb.h"
}

struct DvbTransponder {
	Omm::Dvb::Transponder* pTransponder;
};

struct DvbService {
};

struct DvbStream {
};


int dvb_init(const char *conf_xml)
{
    Omm::Dvb::Device* pDevice = Omm::Dvb::Device::instance();
	pDevice->detectAdapters();
	std::ifstream conf_xml_stream(conf_xml);
    pDevice->readXml(conf_xml_stream);
    return 0;
}


void dvb_open()
{
}


void dvb_close()
{
}


DvbTransponder* dvb_first_transponder(const char *service_name)
{
	DvbTransponder *ret = (DvbTransponder*)malloc(sizeof(DvbTransponder));
	ret->pTransponder = Omm::Dvb::Device::instance()->getFirstTransponder(service_name);
	if (!ret->pTransponder) {
		return NULL;
	}
	return ret;
}


// DvbService* dvb_service(DvbTransponder *transponder, const char *service_name);
// int dvb_service_status(DvbService *service);
// int dvb_service_scrambled(DvbService *service);
// int dvb_service_has_audio(DvbService *service);
// int dvb_service_has_sdvideo(DvbService *service);
// DvbStream* dvb_stream(const char *service_name);
// int dvb_read(DvbStream *stream, char *buf, int nbuf);
// void free_stream(DvbStream *stream);
