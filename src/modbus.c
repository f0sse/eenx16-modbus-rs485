
/*
 * modbus.c
 * lucas@pamorana.net (2024)
 *
 * Gather metrics from ABB Energy Meters (A43),
 * using a custom Raspberry Pi HAT, and post
 * the data to an InfluxDB server.
 */

#include <stddef.h>
#include <stdint.h>
#include <inttypes.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <signal.h>
#include <time.h>

#include <modbus/modbus-rtu.h>
#include <modbus/modbus-version.h>

#include "influx.h"

#undef zDEBUG
#ifdef DEBUG
#define zDEBUG 1
#else
#define zDEBUG 0
#endif

/*
 * SERIAL
 */
#define UART_DEV   "/dev/ttyAMA4"
#define BAUD       9600
#define PARITY     'N' /* 'N', 'E', 'O' */
#define BITS_BYTE  8
#define BITS_STOP  1

/*
 * INFLUXDB
 */

#define FLUX_URL "https://8f.nu"
#define FLUX_ORG "Kandidatarbete"
#define FLUX_BKT "electricity"
#define FLUX_PRC INFLUX_PRECISION_S


static uint32_t regs2uint32 (uint16_t regs[static 2])
{
	uint32_t
		up = (uint32_t) regs[0],
		lo = (uint32_t) regs[1];

	return ((uint32_t) up << 16) | lo;
}

static uint64_t regs2uint64 (uint16_t regs[static 4])
{
	uint64_t
		ms   = (uint64_t) regs[0],
		midl = (uint64_t) regs[1],
		midr = (uint64_t) regs[2],
		ls   = (uint64_t) regs[3];

	return   ( (uint64_t) ms    << 48 )
	       | ( (uint64_t) midl  << 32 )
	       | ( (uint64_t) midr  << 16 )
	       | ( (uint64_t) ls    << 0  )
	       ;
}

/* global writer handle for signal handler cleanup */
static struct influx_writer *writer = NULL;

/* also made global*/
static modbus_t *mb = NULL;

void signal_handler (int sig)
{
	switch (sig)
	{
	case SIGINT:
	case SIGTERM:
		/*
		 * can race if SIGINT and SIGTERM are delivered simultaneously.
		 * bad practice, but I don't care right now.
		 */
		influx_writer_destroy(writer);
		modbus_close(mb);
		modbus_free(mb);
		exit(EXIT_FAILURE);
	}
}

#define INTERVAL 5 /* in seconds */

/* computes a - b */
static struct timespec ts_diff (struct timespec a, struct timespec b)
{
	long diff_sec;
	long diff_nsec;

	struct timespec diff;

	diff_sec  = a.tv_sec - b.tv_sec;
	diff_nsec = 1000000000 * diff_sec + (a.tv_nsec - b.tv_nsec);

	diff.tv_nsec = diff_nsec % 1000000000L;
	diff.tv_sec  = diff_nsec / 1000000000L;

	return diff;
}

static void increment_time (struct timespec *spec)
{
	struct timespec
		now,
		d;

	clock_gettime(CLOCK_MONOTONIC_RAW, &now);

	d = ts_diff(*spec, now);

	while (d.tv_sec < 0 || (d.tv_sec == 0 && d.tv_nsec < 0))
	{
		spec->tv_nsec  = 0;

		if (spec->tv_sec % INTERVAL == 0)
			spec->tv_sec += INTERVAL;
		else
			spec->tv_sec += spec->tv_sec % INTERVAL;

		d = ts_diff(*spec, now);
	}
}

static void wait_until_and_increment(struct timespec *then)
{
	useconds_t wait;
	struct timespec now, diff;

	/*
	 * advance the future timepoint in increments of INTERVAL seconds
	 * until we have a positive difference
	 */
	increment_time(then);

	/* get current time counter */
	clock_gettime(CLOCK_MONOTONIC_RAW, &now);

	/* calculate difference in microseconds to target */
	diff = ts_diff(*then, now);

	wait = (useconds_t) diff.tv_sec  * 1000000U
	     + (useconds_t) diff.tv_nsec / 1000U;

	usleep(wait);
}

int main (int argc, char *argv[])
{
	int rc;

	struct sigaction sa = \
	{
		.sa_flags   = SA_RESTART, /* restart system calls */
		.sa_mask    = 0,
		.sa_handler = signal_handler
	};

	struct timespec ts_next;

	const char *const restrict argv0 = *argv++; argc--;

	if ((sigaction(SIGINT,  &sa, NULL) == -1)
	||  (sigaction(SIGTERM, &sa, NULL) == -1)
	){
		perror("sigaction");
		return 1;
	}

	mb = modbus_new_rtu(UART_DEV, BAUD, PARITY, BITS_BYTE, BITS_STOP);

	if (mb == NULL)
	{
		perror("modbus_new_rtu");
		return EXIT_FAILURE;
	}

	modbus_rtu_set_serial_mode (mb, MODBUS_RTU_RS485);
	modbus_rtu_set_rts         (mb, MODBUS_RTU_RTS_DOWN);
	modbus_rtu_set_rts_delay   (mb, 1); /* [us] between setting RTS and Tx */
	modbus_set_debug           (mb, zDEBUG);
	modbus_set_slave           (mb, 0x1);

	if (modbus_connect(mb) == -1)
	{
		fprintf(stderr, "Connection failed: %s\n", modbus_strerror(errno));
		modbus_free(mb);
		return EXIT_FAILURE;
	}

	/* just check that CLOCK_MONOTONIC_RAW exists on this system */
	if (clock_gettime(CLOCK_MONOTONIC_RAW, &ts_next))
	{
		perror("clock_gettime");
		modbus_close(mb);
		modbus_free(mb);
		return EXIT_FAILURE;
	}

	writer = influx_writer_create(FLUX_URL, FLUX_ORG, FLUX_BKT, FLUX_PRC);

	if (writer == NULL)
	{
		perror("influx_writer_create");
		modbus_close(mb);
		modbus_free(mb);
		return EXIT_FAILURE;
	}

	for (;;)
	{
		char *lines = NULL;

		lines = malloc(sizeof(char));

		/* waits untill next interval, according to "INTERVAL" */
		wait_until_and_increment(&ts_next);

		if (lines == NULL)
			continue;

		*lines = '\0';

		modbus_flush(mb);

		for (int i=1; i < 4; i++) /* 3 meters */
		{
			uint16_t regs [MODBUS_MAX_READ_REGISTERS];

			uint32_t instants [16] = {0};
			uint64_t totals   [16] = {0};
			uint64_t phases   [16] = {0};

			modbus_set_slave (mb, i);

			/*
			 * instantaneous values begin at 0x5B00,
			 * and each value is 2 modbus registers
			 * wide, which makes it a 32-bit value.
			 *
			 * addr.   description     what   res.  unit  type
			 * 0x5B00  Voltage         L1-N   0,1   V     Unsigned
			 * 0x5B02  Voltage         L2-N   0,1   V     Unsigned
			 * 0x5B04  Voltage         L3-N   0,1   V     Unsigned
			 * 0x5B06  Voltage         L1-L2  0,1   V     Unsigned
			 * 0x5B08  Voltage         L3-L2  0,1   V     Unsigned
			 * 0x5B0A  Voltage         L1-L3  0,1   V     Unsigned
			 * 0x5B0C  Current         L1     0,01  A     Unsigned
			 * 0x5B0E  Current         L2     0,01  A     Unsigned
			 * 0x5B10  Current         L3     0,01  A     Unsigned
			 * 0x5B12  Current         N      0,01  A     Unsigned
			 * 0x5B14  Active power    Total  0,01  W     Signed
			 * 0x5B16  Active power    L1     0,01  W     Signed
			 * 0x5B18  Active power    L2     0,01  W     Signed
			 * 0x5B1A  Active power    L3     0,01  W     Signed
			 *
			 * this reading spans 28 registers in total.
			 */

			rc = modbus_read_registers(mb, 0x5B00, 28, regs);

			if (rc < 0)
			{
				fprintf(stderr, "%s\n", modbus_strerror(errno));
				break;
			}

			if (rc == 28)
				for (int j=0; j < 28/2; j++)
					instants[j] = regs2uint32(&regs[j*2]);
			else
			{
				fprintf(stderr, "modbus_read_registers: only %u of 28 registers received\n", rc);
				break;
			}

			/*
			 * total energy accumulators begin at 0x5000.
			 * each measurement is 4 modbus registers wide,
			 * which makes it a 64-bit value.
			 *
			 *   addr.   description             res.   unit      type
			 *   0x5000  Active import           0,01   kWh       Unsigned
			 *   0x5004  Active export           0,01   kWh       Unsigned
			 *   0x5008  Active net              0,01   kWh       Signed
			 *   0x500C  Reactive import         0,01   kvarh     Unsigned
			 *   0x5010  Reactive export         0,01   kVArh     Unsigned
			 *   0x5014  Reactive net            0,01   kVArh     Signed
			 *   0x5018  Apparent import         0,01   kVAh      Unsigned
			 *   0x501C  Apparent export         0,01   kVAh      Unsigned
			 *   0x5020  Apparent net            0,01   kVAh      Signed
			 *   0x5024  Active import CO2       0,001  kg        Unsigned
			 *   0x5034  Active import Currency  0,001  currency  Unsigned
			 *
			 * this block spans 56 registers in total.
			 */

			rc = modbus_read_registers(mb, 0x5000, 56, regs);

			if (rc < 0)
			{
				fprintf(stderr, "%s\n", modbus_strerror(errno));
				break;
			}

			if (rc == 56)
				for (int j=0; j < 56/4; j++)
					totals[j] = regs2uint64(&regs[j*4]);
			else
			{
				fprintf(stderr, "modbus_read_registers: only %u of 56 registers received\n", rc);
				break;
			}

			/*
			 * per-phase energy accumulators begin at 0x5460.
			 * each measurement is 4 modbus registers wide,
			 * which makes it a 64-bit value.
			 *
			 *   addr.   description    line  res.  unit  type
			 *   0x5460  Active import  L1    0,01  kWh   Unsigned
			 *   0x5464  Active import  L2    0,01  kWh   Unsigned
			 *   0x5468  Active import  L3    0,01  kWh   Unsigned
			 *   0x546C  Active export  L1    0,01  kWh   Unsigned
			 *   0x5470  Active export  L2    0,01  kWh   Unsigned
			 *   0x5474  Active export  L3    0,01  kWh   Unsigned
			 *   0x5478  Active net     L1    0,01  kWh   Signed
			 *   0x547C  Active net     L2    0,01  kWh   Signed
			 *   0x5480  Active net     L3    0,01  kWh   Signed
			 *
			 * this selected block spans 36 registers in total.
			 */

			rc = modbus_read_registers(mb, 0x5460, 36, regs);

			if (rc < 0)
			{
				fprintf(stderr, "%s\n", modbus_strerror(errno));
				break;
			}

			if (rc == 36)
				for (int j=0; j < 36/4; j++)
					phases[j] = regs2uint64(&regs[j*4]);
			else
			{
				fprintf(stderr, "modbus_read_registers: only %u of 36 registers received\n", rc);
				break;
			}

			/*
			 * convert into measurements to sent to influxdb
			 */
			if (1) {
				struct influx_field_list
					*instant_fields,
					*total_fields,
					*phase_fields;

				struct field
					**compact_instants,
					**compact_totals,
					**compact_phases;

				char
					*line_total,
					*line_instant,
					*line_phase;

				struct tag tag = \
				{
					.name="meter",
					.value=""
				};

				struct tag *tags[] = \
				{
					&tag,
					NULL
				};

				instant_fields = influx_field_list_create();
				  total_fields = influx_field_list_create();
				  phase_fields = influx_field_list_create();

				tag.value = fstring("%d", i);

				/*
				 * instantaneous values
				 */

				/* voltages */
				influx_field_list_append(instant_fields, "voltage_l1_n",  (double) instants[0] / 10.0);
				influx_field_list_append(instant_fields, "voltage_l2_n",  (double) instants[1] / 10.0);
				influx_field_list_append(instant_fields, "voltage_l3_n",  (double) instants[2] / 10.0);
				influx_field_list_append(instant_fields, "voltage_l1_l2", (double) instants[3] / 10.0);
				influx_field_list_append(instant_fields, "voltage_l3_l2", (double) instants[4] / 10.0);
				influx_field_list_append(instant_fields, "voltage_l1_l3", (double) instants[5] / 10.0);

				/* currents */
				influx_field_list_append(instant_fields, "current_l1", (double) instants[6] / 100.0);
				influx_field_list_append(instant_fields, "current_l2", (double) instants[7] / 100.0);
				influx_field_list_append(instant_fields, "current_l3", (double) instants[8] / 100.0);
				influx_field_list_append(instant_fields, "current_n",  (double) instants[9] / 100.0);

				/* active power */
				influx_field_list_append(instant_fields, "active_tot", (double) ((int32_t) instants[10]) / 100.0);
				influx_field_list_append(instant_fields, "active_l1",  (double) ((int32_t) instants[11]) / 100.0);
				influx_field_list_append(instant_fields, "active_l2",  (double) ((int32_t) instants[12]) / 100.0);
				influx_field_list_append(instant_fields, "active_l3",  (double) ((int32_t) instants[13]) / 100.0);

				/*
				 * total energy accumulators
				 */

				influx_field_list_append(total_fields, "import",   (double) ((uint64_t) totals[ 0]) / 100.00);
				influx_field_list_append(total_fields, "export",   (double) ((uint64_t) totals[ 1]) / 100.00);
				influx_field_list_append(total_fields, "netto",    (double) (( int64_t) totals[ 2]) / 100.00);
				influx_field_list_append(total_fields, "currency", (double) ((uint64_t) totals[13]) / 1000.0);

				/*
				 * per-phase energy accumulators
				 */

				influx_field_list_append(phase_fields, "import_l1", (double) ((uint64_t) phases[0]) / 100.00);
				influx_field_list_append(phase_fields, "import_l2", (double) ((uint64_t) phases[1]) / 100.00);
				influx_field_list_append(phase_fields, "import_l3", (double) ((uint64_t) phases[2]) / 100.00);
				influx_field_list_append(phase_fields, "export_l1", (double) ((uint64_t) phases[3]) / 100.00);
				influx_field_list_append(phase_fields, "export_l2", (double) ((uint64_t) phases[4]) / 100.00);
				influx_field_list_append(phase_fields, "export_l3", (double) ((uint64_t) phases[5]) / 100.00);
				influx_field_list_append(phase_fields, "netto_l1",  (double) ((uint64_t) phases[6]) / 100.00);
				influx_field_list_append(phase_fields, "netto_l2",  (double) ((uint64_t) phases[7]) / 100.00);
				influx_field_list_append(phase_fields, "netto_l3",  (double) ((uint64_t) phases[8]) / 100.00);

				/* create compact lists */
				compact_instants = influx_field_list_compact(instant_fields);
				compact_totals   = influx_field_list_compact(total_fields);
				compact_phases   = influx_field_list_compact(phase_fields);

				influx_field_list_destroy(instant_fields);
				influx_field_list_destroy(total_fields);
				influx_field_list_destroy(phase_fields);

				line_instant = influx_writer_line("instant",           tags, compact_instants, FLUX_PRC);
				line_total   = influx_writer_line("accumulator_total", tags, compact_totals,   FLUX_PRC);
				line_phase   = influx_writer_line("accumulator_phase", tags, compact_phases,   FLUX_PRC);

				lines = fstringa(lines, "%s%s", *lines ? "\n" : "", line_instant);
				lines = fstringa(lines, "%s%s", *lines ? "\n" : "", line_total);
				lines = fstringa(lines, "%s%s", *lines ? "\n" : "", line_phase);

				influx_field_compact_free(compact_instants);
				influx_field_compact_free(compact_totals);
				influx_field_compact_free(compact_phases);

				free(tag.value);
				free(line_instant);
				free(line_total);
				free(line_phase);
			}
		} /* <-- for (electricity meters) */

		/*
		 * upload this interval's metrics to influxdb:
		 */
		if (1) {
			const char *l[] = { lines, NULL };

			int ret = influx_writer_write(writer, l, NULL);

			if (ret < 0)
				perror("influx_writer_write");
		}

		free(lines);
	}
}
