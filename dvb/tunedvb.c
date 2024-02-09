#include <u.h>
#include <stdio.h>
#include <time.h>
#include <libc.h>

#include "dvb.h"


int
main(int argc, char **argv)
{
	if (argc < 3) {
		fprintf(stderr, "usage: tunedvb <config.xml> <service>\n");
		return EXIT_FAILURE;
	}
	const char *config_xml = argv[1];
	const char *service_name = argv[2];
	struct DvbStream *stream = nil;
	int bytes_read = 0;
	int nbuf = 100 * dvb_transport_stream_packet_size;
	char buf[nbuf];
	char outf_name[128];
	int noutf_name = 0, outf = -1;
	clock_t tstart = clock(), telapsed = 0;
	int tmax = 5; // sec

	dvb_init(config_xml);
	dvb_open();
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
	dvb_free_stream(stream);
	close(outf);
	dvb_close();
	return EXIT_SUCCESS;
}
