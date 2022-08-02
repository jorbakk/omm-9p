#include <u.h>
#include <stdio.h>
#include <time.h>
#include <libc.h>

#include "dvb.h"


void
main(int argc, char **argv)
{
	const char *config_xml = argv[1];
	const char *service_name = argv[2];
	struct DvbService *service = nil;
	struct DvbStream *stream = nil;
	struct DvbTransponder *transponder = nil;
	int bytes_read = 0;
	int nbuf = 100 * dvb_transport_stream_packet_size;
	char buf[nbuf];
	char outf_name[128];
	int noutf_name = 0;
	int outf = -1;
	clock_t tstart = clock();
	clock_t telapsed = 0;
	int tmax = 5; // sec

	dvb_init(config_xml);
	dvb_open();
	transponder = dvb_first_transponder(service_name);
	if (transponder == nil) {
		goto quit;
	}
	service = dvb_service(transponder, service_name);
	if (service == nil || 
		dvb_service_status(service) != DvbServiceStatusRunning ||
		dvb_service_scrambled(service) ||
		(!dvb_service_has_audio(service) && !dvb_service_has_sdvideo(service))) {
		goto quit;
	}
	stream = dvb_stream(service_name);
	if (stream == nil) {
		goto quit;
	}
	noutf_name = strlen(service_name) + 4;
	snprint(outf_name, noutf_name, "%s.ts", service_name);
	outf_name[noutf_name] = '\0';
	outf = create(outf_name, OWRITE, 0664);
	while (telapsed < tmax) {
		bytes_read = dvb_read_stream(stream, buf, dvb_transport_stream_packet_size);
		fprintf(stderr, "dvb bytes read: %d\n", bytes_read);
		write(outf, buf, bytes_read);
		telapsed = (clock() - tstart) / CLOCKS_PER_SEC;
	}
quit:
	dvb_free_transponder(transponder);
	dvb_free_service(service);
	dvb_free_stream(stream);
	close(outf);
	dvb_close();
}
