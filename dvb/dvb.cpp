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
	Omm::Dvb::Service* pService;
};

struct DvbStream {
	Omm::Dvb::Stream* pStream;
};


int
dvb_init(const char *conf_xml)
{
    Omm::Dvb::Device* pDevice = Omm::Dvb::Device::instance();
	pDevice->detectAdapters();
	std::ifstream conf_xml_stream(conf_xml);
    pDevice->readXml(conf_xml_stream);
    return 0;
}


void
dvb_open()
{
	Omm::Dvb::Device::instance()->open();
}


void
dvb_close()
{
	Omm::Dvb::Device::instance()->close();
}


DvbTransponder*
dvb_first_transponder(const char *service_name)
{
	DvbTransponder *ret = (DvbTransponder*)malloc(sizeof(DvbTransponder));
	ret->pTransponder = Omm::Dvb::Device::instance()->getFirstTransponder(service_name);
	if (!ret->pTransponder) {
		return NULL;
	}
	return ret;
}


DvbService*
dvb_service(DvbTransponder *transponder, const char *service_name)
{
	if (!transponder->pTransponder) {
		return NULL;
	}
	DvbService *ret = (DvbService*)malloc(sizeof(DvbService));
	ret->pService = transponder->pTransponder->getService(service_name);
	if (!ret->pService) {
		return NULL;
	}
	return ret;
}


// int dvb_service_status(DvbService *service);
// int dvb_service_scrambled(DvbService *service);
// int dvb_service_has_audio(DvbService *service);
// int dvb_service_has_sdvideo(DvbService *service);
// DvbStream* dvb_stream(const char *service_name);
// int dvb_read(DvbStream *stream, char *buf, int nbuf);


void
free_transponder(struct DvbTransponder *transponder)
{
	delete transponder->pTransponder;
	free(transponder);
}


void
free_service(struct DvbService *service)
{
	delete service->pService;
	free(service);
}


void
free_stream(DvbStream *stream)
{
	delete stream->pStream;
	free(stream);
}
