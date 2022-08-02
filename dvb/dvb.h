#ifndef __OMMDVB__
#define __OMMDVB__

struct DvbTransponder;
struct DvbService;
struct DvbStream;

int dvb_init(const char *conf_xml);
void dvb_open();
void dvb_close();
DvbTransponder* dvb_first_transponder(const char *service_name);
DvbService* dvb_service(DvbTransponder *transponder, const char *service_name);
int dvb_service_status(DvbService *service);
int dvb_service_scrambled(DvbService *service);
int dvb_service_has_audio(DvbService *service);
int dvb_service_has_sdvideo(DvbService *service);
DvbStream* dvb_stream(const char *service_name);
int dvb_read(DvbStream *stream, char *buf, int nbuf);
void free_stream(DvbStream *stream);

#endif
