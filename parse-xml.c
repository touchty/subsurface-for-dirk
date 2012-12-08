#include <stdio.h>
#include <ctype.h>
#include <string.h>
#include <stdlib.h>
#include <errno.h>
#include <unistd.h>
#define __USE_XOPEN
#include <time.h>
#include <libxml/parser.h>
#include <libxml/tree.h>
#ifdef XSLT
#include <libxslt/transform.h>
#endif
#include <glib/gi18n.h>

#include "dive.h"

int verbose;

struct dive_table dive_table;

/*
 * Add a dive into the dive_table array
 */
void record_dive(struct dive *dive)
{
	int nr = dive_table.nr, allocated = dive_table.allocated;
	struct dive **dives = dive_table.dives;

	if (nr >= allocated) {
		allocated = (nr + 32) * 3 / 2;
		dives = realloc(dives, allocated * sizeof(struct dive *));
		if (!dives)
			exit(1);
		dive_table.dives = dives;
		dive_table.allocated = allocated;
	}
	dives[nr] = fixup_dive(dive);
	dive_table.nr = nr+1;
}

static void delete_dive_renumber(struct dive **dives, int i, int nr)
{
	struct dive *dive = dives[i];
	int number = dive->number, j;

	if (!number)
		return;

	/*
	 * Check that all numbered dives after the deleted
	 * ones are consecutive, return without renumbering
	 * if that is not the case.
	 */
	for (j = i+1; j < nr; j++) {
		struct dive *next = dives[j];
		if (!next->number)
			break;
		number++;
		if (next->number != number)
			return;
	}

	/*
	 * Ok, we hit the end of the dives or a unnumbered
	 * dive - renumber.
	 */
	for (j = i+1 ; j < nr; j++) {
		struct dive *next = dives[j];
		if (!next->number)
			break;
		next->number--;
	}
}

/*
 * Remove a dive from the dive_table array
 */
void delete_dive(struct dive *dive)
{
	int nr = dive_table.nr, i;
	struct dive **dives = dive_table.dives;

	/*
	 * Stupid. We know the dive table is sorted by date,
	 * we could do a binary lookup. Sue me.
	 */
	for (i = 0; i < nr; i++) {
		struct dive *d = dives[i];
		if (d != dive)
			continue;
		/* should we re-number? */
		delete_dive_renumber(dives, i, nr);
		memmove(dives+i, dives+i+1, sizeof(struct dive *)*(nr-i-1));
		dives[nr] = NULL;
		dive_table.nr = nr-1;
		break;
	}
}

static void start_match(const char *type, const char *name, char *buffer)
{
	if (verbose > 2)
		printf("Matching %s '%s' (%s)\n",
			type, name, buffer);
}

static void nonmatch(const char *type, const char *name, char *buffer)
{
	if (verbose > 1)
		printf("Unable to match %s '%s' (%s)\n",
			type, name, buffer);
	free(buffer);
}

typedef void (*matchfn_t)(char *buffer, void *);

static int match(const char *pattern, int plen,
		 const char *name, int nlen,
		 matchfn_t fn, char *buf, void *data)
{
	if (plen > nlen)
		return 0;
	if (memcmp(pattern, name + nlen - plen, plen))
		return 0;
	fn(buf, data);
	return 1;
}


struct units input_units;

/*
 * We're going to default to SI units for input. Yes,
 * technically the SI unit for pressure is Pascal, but
 * we default to bar (10^5 pascal), which people
 * actually use. Similarly, C instead of Kelvin.
 * And kg instead of g.
 */
const struct units SI_units = {
	.length = METERS,
	.volume = LITER,
	.pressure = BAR,
	.temperature = CELSIUS,
	.weight = KG
};

const struct units IMPERIAL_units = {
	.length = FEET,
	.volume = CUFT,
	.pressure = PSI,
	.temperature = FAHRENHEIT,
	.weight = LBS
};

/*
 * Dive info as it is being built up..
 */
static struct divecomputer *cur_dc;
static struct dive *cur_dive;
static dive_trip_t *cur_trip = NULL;
static struct sample *cur_sample;
static struct {
	int active;
	duration_t time;
	int type, flags, value;
	const char *name;
} cur_event;
static struct tm cur_tm;
static int cur_cylinder_index, cur_ws_index;
static int lastndl, laststoptime, laststopdepth, lastcns, lastpo2;

static enum import_source {
	UNKNOWN,
	LIBDIVECOMPUTER,
	DIVINGLOG,
	UDDF,
} import_source;

static void divedate(char *buffer, void *_when)
{
	int d,m,y;
	timestamp_t *when = _when;
	int success = 0;

	success = cur_tm.tm_sec | cur_tm.tm_min | cur_tm.tm_hour;
	if (sscanf(buffer, "%d.%d.%d", &d, &m, &y) == 3) {
		cur_tm.tm_year = y;
		cur_tm.tm_mon = m-1;
		cur_tm.tm_mday = d;
	} else if (sscanf(buffer, "%d-%d-%d", &y, &m, &d) == 3) {
		cur_tm.tm_year = y;
		cur_tm.tm_mon = m-1;
		cur_tm.tm_mday = d;
	} else {
		fprintf(stderr, "Unable to parse date '%s'\n", buffer);
		success = 0;
	}

	if (success)
		*when = utc_mktime(&cur_tm);

	free(buffer);
}

static void divetime(char *buffer, void *_when)
{
	int h,m,s = 0;
	timestamp_t *when = _when;

	if (sscanf(buffer, "%d:%d:%d", &h, &m, &s) >= 2) {
		cur_tm.tm_hour = h;
		cur_tm.tm_min = m;
		cur_tm.tm_sec = s;
		if (cur_tm.tm_year)
			*when = utc_mktime(&cur_tm);
	}
	free(buffer);
}

/* Libdivecomputer: "2011-03-20 10:22:38" */
static void divedatetime(char *buffer, void *_when)
{
	int y,m,d;
	int hr,min,sec;
	timestamp_t *when = _when;

	if (sscanf(buffer, "%d-%d-%d %d:%d:%d",
		&y, &m, &d, &hr, &min, &sec) == 6) {
		cur_tm.tm_year = y;
		cur_tm.tm_mon = m-1;
		cur_tm.tm_mday = d;
		cur_tm.tm_hour = hr;
		cur_tm.tm_min = min;
		cur_tm.tm_sec = sec;
		*when = utc_mktime(&cur_tm);
	}
	free(buffer);
}

union int_or_float {
	double fp;
};

enum number_type {
	NEITHER,
	FLOAT
};

static enum number_type integer_or_float(char *buffer, union int_or_float *res)
{
	char *end;
	long val;
	double fp;

	/* Integer or floating point? */
	val = strtol(buffer, &end, 10);
	if (val < 0 || end == buffer)
		return NEITHER;

	/* Looks like it might be floating point? */
	if (*end == '.') {
		errno = 0;
		fp = g_ascii_strtod(buffer, &end);
		if (!errno) {
			res->fp = fp;
			return FLOAT;
		}
	}

	res->fp = val;
	return FLOAT;
}

static void pressure(char *buffer, void *_press)
{
	double mbar;
	pressure_t *pressure = _press;
	union int_or_float val;

	switch (integer_or_float(buffer, &val)) {
	case FLOAT:
		/* Just ignore zero values */
		if (!val.fp)
			break;
		switch (input_units.pressure) {
		case PASCAL:
			mbar = val.fp / 100;
			break;
		case BAR:
			/* Assume mbar, but if it's really small, it's bar */
			mbar = val.fp;
			if (mbar < 5000)
				mbar = mbar * 1000;
			break;
		case PSI:
			mbar = val.fp * 68.95;
			break;
		}
		if (mbar > 5 && mbar < 500000) {
			pressure->mbar = mbar + 0.5;
			break;
		}
	/* fallthrough */
	default:
		printf("Strange pressure reading %s\n", buffer);
	}
	free(buffer);
}

static void salinity(char *buffer, void *_salinity)
{
	int *salinity = _salinity;
	union int_or_float val;
	switch (integer_or_float(buffer, &val)) {
	case FLOAT:
		*salinity = val.fp * 10.0 + 0.5;
		break;
	default:
		printf("Strange salinity reading %s\n", buffer);
	}
	free(buffer);
}

static void depth(char *buffer, void *_depth)
{
	depth_t *depth = _depth;
	union int_or_float val;

	switch (integer_or_float(buffer, &val)) {
	case FLOAT:
		switch (input_units.length) {
		case METERS:
			depth->mm = val.fp * 1000 + 0.5;
			break;
		case FEET:
			depth->mm = val.fp * 304.8 + 0.5;
			break;
		}
		break;
	default:
		printf("Strange depth reading %s\n", buffer);
	}
	free(buffer);
}

static void weight(char *buffer, void *_weight)
{
	weight_t *weight = _weight;
	union int_or_float val;

	switch (integer_or_float(buffer, &val)) {
	case FLOAT:
		switch (input_units.weight) {
		case KG:
			weight->grams = val.fp * 1000 + 0.5;
			break;
		case LBS:
			weight->grams = val.fp * 453.6 + 0.5;
			break;
		}
		break;
	default:
		printf("Strange weight reading %s\n", buffer);
	}
}

static void temperature(char *buffer, void *_temperature)
{
	temperature_t *temperature = _temperature;
	union int_or_float val;

	switch (integer_or_float(buffer, &val)) {
	case FLOAT:
		/* Ignore zero. It means "none" */
		if (!val.fp)
			break;
		/* Celsius */
		switch (input_units.temperature) {
		case KELVIN:
			temperature->mkelvin = val.fp * 1000;
			break;
		case CELSIUS:
			temperature->mkelvin = (val.fp + 273.15) * 1000 + 0.5;
			break;
		case FAHRENHEIT:
			temperature->mkelvin = (val.fp + 459.67) * 5000/9;
			break;
		}
		break;
	default:
		printf("Strange temperature reading %s\n", buffer);
	}
	free(buffer);
}

static void sampletime(char *buffer, void *_time)
{
	int i;
	int min, sec;
	duration_t *time = _time;

	i = sscanf(buffer, "%d:%d", &min, &sec);
	switch (i) {
	case 1:
		sec = min;
		min = 0;
	/* fallthrough */
	case 2:
		time->seconds = sec + min*60;
		break;
	default:
		printf("Strange sample time reading %s\n", buffer);
	}
	free(buffer);
}

static void duration(char *buffer, void *_time)
{
	sampletime(buffer, _time);
}

static void percent(char *buffer, void *_fraction)
{
	fraction_t *fraction = _fraction;
	union int_or_float val;

	switch (integer_or_float(buffer, &val)) {
	case FLOAT:
		if (val.fp <= 100.0)
			fraction->permille = val.fp * 10 + 0.5;
		break;

	default:
		printf("Strange percentage reading %s\n", buffer);
		break;
	}
	free(buffer);
}

static void gasmix(char *buffer, void *_fraction)
{
	/* libdivecomputer does negative percentages. */
	if (*buffer == '-')
		return;
	if (cur_cylinder_index < MAX_CYLINDERS)
		percent(buffer, _fraction);
}

static void gasmix_nitrogen(char *buffer, void *_gasmix)
{
	/* Ignore n2 percentages. There's no value in them. */
}

static void cylindersize(char *buffer, void *_volume)
{
	volume_t *volume = _volume;
	union int_or_float val;

	switch (integer_or_float(buffer, &val)) {
	case FLOAT:
		volume->mliter = val.fp * 1000 + 0.5;
		break;

	default:
		printf("Strange volume reading %s\n", buffer);
		break;
	}
	free(buffer);
}

static void utf8_string(char *buffer, void *_res)
{
	*(char **)_res = buffer;
}

#define MATCH(pattern, fn, dest) \
	match(pattern, strlen(pattern), name, len, fn, buf, dest)

static void get_index(char *buffer, void *_i)
{
	int *i = _i;
	*i = atoi(buffer);
	free(buffer);
}

static void double_to_permil(char *buffer, void *_i)
{
	int *i = _i;
	*i = g_ascii_strtod(buffer, NULL) * 1000.0 + 0.5;
	free(buffer);
}

static void hex_value(char *buffer, void *_i)
{
	uint32_t *i = _i;
	*i = strtol(buffer, NULL, 16);
	free(buffer);
}

static void get_tripflag(char *buffer, void *_tf)
{
	tripflag_t *tf = _tf;
	*tf = strcmp(buffer, "NOTRIP") ? TF_NONE : NO_TRIP;
	free(buffer);
}

/*
 * Divinglog is crazy. The temperatures are in celsius. EXCEPT
 * for the sample temperatures, that are in Fahrenheit.
 * WTF?
 *
 * Oh, and I think Diving Log *internally* probably kept them
 * in celsius, because I'm seeing entries like
 *
 *	<Temp>32.0</Temp>
 *
 * in there. Which is freezing, aka 0 degC. I bet the "0" is
 * what Diving Log uses for "no temperature".
 *
 * So throw away crap like that.
 *
 * It gets worse. Sometimes the sample temperatures are in
 * Celsius, which apparently happens if you are in a SI
 * locale. So we now do:
 *
 * - temperatures < 32.0 == Celsius
 * - temperature == 32.0  -> garbage, it's a missing temperature (zero converted from C to F)
 * - temperatures > 32.0 == Fahrenheit
 */
static void fahrenheit(char *buffer, void *_temperature)
{
	temperature_t *temperature = _temperature;
	union int_or_float val;

	switch (integer_or_float(buffer, &val)) {
	case FLOAT:
		/* Floating point equality is evil, but works for small integers */
		if (val.fp == 32.0)
			break;
		if (val.fp < 32.0)
			temperature->mkelvin = C_to_mkelvin(val.fp);
		else
			temperature->mkelvin = F_to_mkelvin(val.fp);
		break;
	default:
		fprintf(stderr, "Crazy Diving Log temperature reading %s\n", buffer);
	}
	free(buffer);
}

/*
 * Did I mention how bat-shit crazy divinglog is? The sample
 * pressures are in PSI. But the tank working pressure is in
 * bar. WTF^2?
 *
 * Crazy stuff like this is why subsurface has everything in
 * these inconvenient typed structures, and you have to say
 * "pressure->mbar" to get the actual value. Exactly so that
 * you can never have unit confusion.
 *
 * It gets worse: sometimes apparently the pressures are in
 * bar, sometimes in psi. Dirk suspects that this may be a
 * DivingLog Uemis importer bug, and that they are always
 * supposed to be in bar, but that the importer got the
 * sample importing wrong.
 *
 * Sadly, there's no way to really tell. So I think we just
 * have to have some arbitrary cut-off point where we assume
 * that smaller values mean bar.. Not good.
 */
static void psi_or_bar(char *buffer, void *_pressure)
{
	pressure_t *pressure = _pressure;
	union int_or_float val;

	switch (integer_or_float(buffer, &val)) {
	case FLOAT:
		if (val.fp > 400)
			pressure->mbar = psi_to_mbar(val.fp);
		else
			pressure->mbar = val.fp * 1000 + 0.5;
		break;
	default:
		fprintf(stderr, "Crazy Diving Log PSI reading %s\n", buffer);
	}
	free(buffer);
}

static int divinglog_fill_sample(struct sample *sample, const char *name, int len, char *buf)
{
	return	MATCH(".p.time", sampletime, &sample->time) ||
		MATCH(".p.depth", depth, &sample->depth) ||
		MATCH(".p.temp", fahrenheit, &sample->temperature) ||
		MATCH(".p.press1", psi_or_bar, &sample->cylinderpressure) ||
		0;
}

static int uddf_fill_sample(struct sample *sample, const char *name, int len, char *buf)
{
	return	MATCH(".divetime", sampletime, &sample->time) ||
		MATCH(".depth", depth, &sample->depth) ||
		MATCH(".temperature", temperature, &sample->temperature) ||
		MATCH(".tankpressure", pressure, &sample->cylinderpressure) ||
		0;
}

static void eventtime(char *buffer, void *_duration)
{
	duration_t *duration = _duration;
	sampletime(buffer, duration);
	if (cur_sample)
		duration->seconds += cur_sample->time.seconds;
}

static void try_to_fill_event(const char *name, char *buf)
{
	int len = strlen(name);

	start_match("event", name, buf);
	if (MATCH(".event", utf8_string, &cur_event.name))
		return;
	if (MATCH(".name", utf8_string, &cur_event.name))
		return;
	if (MATCH(".time", eventtime, &cur_event.time))
		return;
	if (MATCH(".type", get_index, &cur_event.type))
		return;
	if (MATCH(".flags", get_index, &cur_event.flags))
		return;
	if (MATCH(".value", get_index, &cur_event.value))
		return;
	nonmatch("event", name, buf);
}

/* We're in the top-level dive xml. Try to convert whatever value to a dive value */
static void try_to_fill_dc(struct divecomputer *dc, const char *name, char *buf)
{
	int len = strlen(name);

	start_match("divecomputer", name, buf);

	if (MATCH(".date", divedate, &dc->when))
		return;
	if (MATCH(".time", divetime, &dc->when))
		return;
	if (MATCH(".model", utf8_string, &dc->model))
		return;
	if (MATCH(".deviceid", hex_value, &dc->deviceid))
		return;
	if (MATCH(".diveid", hex_value, &dc->diveid))
		return;

	nonmatch("divecomputer", name, buf);
}

/* We're in samples - try to convert the random xml value to something useful */
static void try_to_fill_sample(struct sample *sample, const char *name, char *buf)
{
	int len = strlen(name);

	start_match("sample", name, buf);
	if (MATCH(".sample.pressure", pressure, &sample->cylinderpressure))
		return;
	if (MATCH(".sample.cylpress", pressure, &sample->cylinderpressure))
		return;
	if (MATCH(".sample.cylinderindex", get_index, &sample->cylinderindex))
		return;
	if (MATCH(".sample.depth", depth, &sample->depth))
		return;
	if (MATCH(".sample.temp", temperature, &sample->temperature))
		return;
	if (MATCH(".sample.temperature", temperature, &sample->temperature))
		return;
	if (MATCH(".sample.sampletime", sampletime, &sample->time))
		return;
	if (MATCH(".sample.time", sampletime, &sample->time))
		return;
	if (MATCH(".sample.ndl", sampletime, &sample->ndl))
		return;
	if (MATCH(".sample.stoptime", sampletime, &sample->stoptime))
		return;
	if (MATCH(".sample.stopdepth", depth, &sample->stopdepth))
		return;
	if (MATCH(".sample.cns", get_index, &sample->cns))
			return;
	if (MATCH(".sample.po2", double_to_permil, &sample->po2))
			return;

	switch (import_source) {
	case DIVINGLOG:
		if (divinglog_fill_sample(sample, name, len, buf))
			return;
		break;

	case UDDF:
		if (uddf_fill_sample(sample, name, len, buf))
			return;
		break;

	default:
		break;
	}

	nonmatch("sample", name, buf);
}

static const char *country, *city;

static void divinglog_place(char *place, void *_location)
{
	char **location = _location;
	char buffer[256], *p;
	int len;

	len = snprintf(buffer, sizeof(buffer),
		"%s%s%s%s%s",
		place,
		city ? ", " : "",
		city ? city : "",
		country ? ", " : "",
		country ? country : "");

	p = malloc(len+1);
	memcpy(p, buffer, len+1);
	*location = p;

	city = NULL;
	country = NULL;
}

static int divinglog_dive_match(struct dive *dive, const char *name, int len, char *buf)
{
	return	MATCH(".divedate", divedate, &dive->when) ||
		MATCH(".entrytime", divetime, &dive->when) ||
		MATCH(".depth", depth, &dive->maxdepth) ||
		MATCH(".tanktype", utf8_string, &dive->cylinder[0].type.description) ||
		MATCH(".tanksize", cylindersize, &dive->cylinder[0].type.size) ||
		MATCH(".presw", pressure, &dive->cylinder[0].type.workingpressure) ||
		MATCH(".press", pressure, &dive->cylinder[0].start) ||
		MATCH(".prese", pressure, &dive->cylinder[0].end) ||
		MATCH(".comments", utf8_string, &dive->notes) ||
		MATCH(".buddy.names", utf8_string, &dive->buddy) ||
		MATCH(".country.name", utf8_string, &country) ||
		MATCH(".city.name", utf8_string, &city) ||
		MATCH(".place.name", divinglog_place, &dive->location) ||
		0;
}

/*
 * Uddf specifies ISO 8601 time format.
 *
 * There are many variations on that. This handles the useful cases.
 */
static void uddf_datetime(char *buffer, void *_when)
{
	char c;
	int y,m,d,hh,mm,ss;
	timestamp_t *when = _when;
	struct tm tm = { 0 };
	int i;

	i = sscanf(buffer, "%d-%d-%d%c%d:%d:%d", &y, &m, &d, &c, &hh, &mm, &ss);
	if (i == 7)
		goto success;
	ss = 0;
	if (i == 6)
		goto success;

	i = sscanf(buffer, "%04d%02d%02d%c%02d%02d%02d", &y, &m, &d, &c, &hh, &mm, &ss);
	if (i == 7)
		goto success;
	ss = 0;
	if (i == 6)
		goto success;
bad_date:
	printf("Bad date time %s\n", buffer);
	free(buffer);
	return;

success:
	if (c != 'T' && c != ' ')
		goto bad_date;
	tm.tm_year = y;
	tm.tm_mon = m - 1;
	tm.tm_mday = d;
	tm.tm_hour = hh;
	tm.tm_min = mm;
	tm.tm_sec = ss;
	*when = utc_mktime(&tm);
	free(buffer);
}

static int uddf_dive_match(struct dive *dive, const char *name, int len, char *buf)
{
	return	MATCH(".datetime", uddf_datetime, &dive->when) ||
		MATCH(".diveduration", duration, &dive->duration) ||
		MATCH(".greatestdepth", depth, &dive->maxdepth) ||
		0;
}

/*
 * This parses "floating point" into micro-degrees.
 * We don't do exponentials etc, if somebody does
 * gps locations in that format, they are insane.
 */
static degrees_t parse_degrees(char *buf, char **end)
{
	int sign = 1, decimals = 6, value = 0;
	degrees_t ret;

	while (isspace(*buf))
		buf++;
	switch (*buf) {
	case '-':
		sign = -1;
		/* fallthrough */
	case '+':
		buf++;
	}
	while (isdigit(*buf)) {
		value = 10*value + *buf - '0';
		buf++;
	}

	/* Get the first six decimals if they exist */
	if (*buf == '.')
		buf++;
	do {
		value *= 10;
		if (isdigit(*buf)) {
			value += *buf - '0';
			buf++;
		}
	} while (--decimals);

	/* Rounding */
	switch (*buf) {
	case '5' ... '9':
		value++;
	}
	while (isdigit(*buf))
		buf++;

	*end = buf;
	ret.udeg = value * sign;
	return ret;
}

static void gps_location(char *buffer, void *_dive)
{
	char *end;
	struct dive *dive = _dive;

	dive->latitude = parse_degrees(buffer, &end);
	dive->longitude = parse_degrees(end, &end);
	free(buffer);
}

/* We're in the top-level dive xml. Try to convert whatever value to a dive value */
static void try_to_fill_dive(struct dive *dive, const char *name, char *buf)
{
	int len = strlen(name);

	start_match("dive", name, buf);

	switch (import_source) {
	case DIVINGLOG:
		if (divinglog_dive_match(dive, name, len, buf))
			return;
		break;

	case UDDF:
		if (uddf_dive_match(dive, name, len, buf))
			return;
		break;

	default:
		break;
	}

	if (MATCH(".number", get_index, &dive->number))
		return;
	if (MATCH(".tripflag", get_tripflag, &dive->tripflag))
		return;
	if (MATCH(".date", divedate, &dive->when))
		return;
	if (MATCH(".time", divetime, &dive->when))
		return;
	if (MATCH(".datetime", divedatetime, &dive->when))
		return;
	if (MATCH(".maxdepth", depth, &dive->maxdepth))
		return;
	if (MATCH(".meandepth", depth, &dive->meandepth))
		return;
	if (MATCH(".depth.max", depth, &dive->maxdepth))
		return;
	if (MATCH(".depth.mean", depth, &dive->meandepth))
		return;
	if (MATCH(".duration", duration, &dive->duration))
		return;
	if (MATCH(".divetime", duration, &dive->duration))
		return;
	if (MATCH(".divetimesec", duration, &dive->duration))
		return;
	if (MATCH(".surfacetime", duration, &dive->surfacetime))
		return;
	if (MATCH(".airtemp", temperature, &dive->airtemp))
		return;
	if (MATCH(".watertemp", temperature, &dive->watertemp))
		return;
	if (MATCH(".temperature.air", temperature, &dive->airtemp))
		return;
	if (MATCH(".temperature.water", temperature, &dive->watertemp))
		return;
	if (MATCH(".surface.pressure", pressure, &dive->surface_pressure))
		return;
	if (MATCH(".water.salinity", salinity, &dive->salinity))
		return;
	if (MATCH(".cylinderstartpressure", pressure, &dive->cylinder[0].start))
		return;
	if (MATCH(".cylinderendpressure", pressure, &dive->cylinder[0].end))
		return;
	if (MATCH(".gps", gps_location, dive))
		return;
	if (MATCH(".location", utf8_string, &dive->location))
		return;
	if (MATCH(".suit", utf8_string, &dive->suit))
		return;
	if (MATCH(".divesuit", utf8_string, &dive->suit))
		return;
	if (MATCH(".notes", utf8_string, &dive->notes))
		return;
	if (MATCH(".divemaster", utf8_string, &dive->divemaster))
		return;
	if (MATCH(".buddy", utf8_string, &dive->buddy))
		return;
	if (MATCH(".rating", get_index, &dive->rating))
		return;
	if (MATCH(".visibility", get_index, &dive->visibility))
		return;
	if (MATCH(".cylinder.size", cylindersize, &dive->cylinder[cur_cylinder_index].type.size))
		return;
	if (MATCH(".cylinder.workpressure", pressure, &dive->cylinder[cur_cylinder_index].type.workingpressure))
		return;
	if (MATCH(".cylinder.description", utf8_string, &dive->cylinder[cur_cylinder_index].type.description))
		return;
	if (MATCH(".cylinder.start", pressure, &dive->cylinder[cur_cylinder_index].start))
		return;
	if (MATCH(".cylinder.end", pressure, &dive->cylinder[cur_cylinder_index].end))
		return;
	if (MATCH(".weightsystem.description", utf8_string, &dive->weightsystem[cur_ws_index].description))
		return;
	if (MATCH(".weightsystem.weight", weight, &dive->weightsystem[cur_ws_index].weight))
		return;
	if (MATCH("weight", weight, &dive->weightsystem[cur_ws_index].weight))
		return;
	if (MATCH(".o2", gasmix, &dive->cylinder[cur_cylinder_index].gasmix.o2))
		return;
	if (MATCH(".n2", gasmix_nitrogen, &dive->cylinder[cur_cylinder_index].gasmix))
		return;
	if (MATCH(".he", gasmix, &dive->cylinder[cur_cylinder_index].gasmix.he))
		return;

	nonmatch("dive", name, buf);
}

/* We're in the top-level trip xml. Try to convert whatever value to a trip value */
static void try_to_fill_trip(dive_trip_t **dive_trip_p, const char *name, char *buf)
{
	int len = strlen(name);

	start_match("trip", name, buf);

	dive_trip_t *dive_trip = *dive_trip_p;

	if (MATCH(".date", divedate, &dive_trip->when))
		return;
	if (MATCH(".time", divetime, &dive_trip->when))
		return;
	if (MATCH(".location", utf8_string, &dive_trip->location))
		return;
	if (MATCH(".notes", utf8_string, &dive_trip->notes))
		return;

	nonmatch("trip", name, buf);
}

/*
 * While in some formats file boundaries are dive boundaries, in many
 * others (as for example in our native format) there are
 * multiple dives per file, so there can be other events too that
 * trigger a "new dive" marker and you may get some nesting due
 * to that. Just ignore nesting levels.
 * On the flipside it is possible that we start an XML file that ends
 * up having no dives in it at all - don't create a bogus empty dive
 * for those. It's not entirely clear what is the minimum set of data
 * to make a dive valid, but if it has no location, no date and no
 * samples I'm pretty sure it's useless.
 */
static gboolean is_dive(void)
{
	return (cur_dive &&
		(cur_dive->location || cur_dive->when || cur_dive->dc.samples));
}

static void dive_start(void)
{
	if (cur_dive)
		return;
	cur_dive = alloc_dive();
	memset(&cur_tm, 0, sizeof(cur_tm));
	if (cur_trip) {
		add_dive_to_trip(cur_dive, cur_trip);
		cur_dive->tripflag = IN_TRIP;
	}
}

static void dive_end(void)
{
	if (!cur_dive)
		return;
	if (!is_dive())
		free(cur_dive);
	else
		record_dive(cur_dive);
	cur_dive = NULL;
	cur_dc = NULL;
	cur_cylinder_index = 0;
	cur_ws_index = 0;
}

static void trip_start(void)
{
	if (cur_trip)
		return;
	dive_end();
	cur_trip = calloc(sizeof(dive_trip_t),1);
	memset(&cur_tm, 0, sizeof(cur_tm));
}

static void trip_end(void)
{
	if (!cur_trip)
		return;
	insert_trip(&cur_trip);
	cur_trip = NULL;
}

static void event_start(void)
{
	memset(&cur_event, 0, sizeof(cur_event));
	cur_event.active = 1;
}

/*
 * If we don't have an explicit dive computer,
 * we use the implicit one that every dive has..
 */
static struct divecomputer *get_dc(void)
{
	return cur_dc ? : &cur_dive->dc;
}

static void event_end(void)
{
	struct divecomputer *dc = get_dc();
	if (cur_event.name && strcmp(cur_event.name, "surface") != 0)
		add_event(dc, cur_event.time.seconds,
			cur_event.type, cur_event.flags,
			cur_event.value, cur_event.name);
	cur_event.active = 0;
}

static void cylinder_start(void)
{
}

static void cylinder_end(void)
{
	cur_cylinder_index++;
}

static void ws_start(void)
{
}

static void ws_end(void)
{
	cur_ws_index++;
}

static void sample_start(void)
{
	cur_sample = prepare_sample(get_dc());
	cur_sample->ndl.seconds = lastndl;
	cur_sample->stoptime.seconds = laststoptime;
	cur_sample->stopdepth.mm = laststopdepth;
	cur_sample->cns = lastcns;
	cur_sample->po2 = lastpo2;
}

static void sample_end(void)
{
	if (!cur_dive)
		return;

	finish_sample(get_dc());
	lastndl = cur_sample->ndl.seconds;
	laststoptime = cur_sample->stoptime.seconds;
	laststopdepth = cur_sample->stopdepth.mm;
	lastcns = cur_sample->cns;
	lastpo2 = cur_sample->po2;
	cur_sample = NULL;
}

static void divecomputer_start(void)
{
	struct divecomputer *dc;

	/* Start from the previous dive computer */
	dc = &cur_dive->dc;
	while (dc->next)
		dc = dc->next;

	/* Did we already fill that in? */
	if (dc->samples || dc->model || dc->when) {
		struct divecomputer *newdc = calloc(1, sizeof(*newdc));
		if (newdc) {
			dc->next = newdc;
			dc = newdc;
		}
	}

	/* .. this is the one we'll use */
	cur_dc = dc;

	lastcns = lastpo2 = lastndl = laststoptime = laststopdepth = 0;
}

static void divecomputer_end(void)
{
	if (!cur_dc->when)
		cur_dc->when = cur_dive->when;
	cur_dc = NULL;
}

static void entry(const char *name, int size, const char *raw)
{
	char *buf = malloc(size+1);

	if (!buf)
		return;
	memcpy(buf, raw, size);
	buf[size] = 0;
	if (cur_event.active) {
		try_to_fill_event(name, buf);
		return;
	}
	if (cur_sample) {
		try_to_fill_sample(cur_sample, name, buf);
		return;
	}
	if (cur_dc) {
		try_to_fill_dc(cur_dc, name, buf);
		return;
	}
	if (cur_dive) {
		try_to_fill_dive(cur_dive, name, buf);
		return;
	}
	if (cur_trip) {
		try_to_fill_trip(&cur_trip, name, buf);
		return;
	}
	free(buf);
}

static const char *nodename(xmlNode *node, char *buf, int len)
{
	if (!node || !node->name)
		return "root";

	buf += len;
	*--buf = 0;
	len--;

	for(;;) {
		const char *name = node->name;
		int i = strlen(name);
		while (--i >= 0) {
			unsigned char c = name[i];
			*--buf = tolower(c);
			if (!--len)
				return buf;
		}
		node = node->parent;
		if (!node || !node->name)
			return buf;
		*--buf = '.';
		if (!--len)
			return buf;
	}
}

#define MAXNAME 64

static void visit_one_node(xmlNode *node)
{
	int len;
	const unsigned char *content;
	char buffer[MAXNAME];
	const char *name;

	content = node->content;
	if (!content)
		return;

	/* Trim whitespace at beginning */
	while (isspace(*content))
		content++;

	/* Trim whitespace at end */
	len = strlen(content);
	while (len && isspace(content[len-1]))
		len--;

	if (!len)
		return;

	/* Don't print out the node name if it is "text" */
	if (!strcmp(node->name, "text"))
		node = node->parent;

	name = nodename(node, buffer, sizeof(buffer));

	entry(name, len, content);
}

static void traverse(xmlNode *root);

static void traverse_properties(xmlNode *node)
{
	xmlAttr *p;

	for (p = node->properties; p; p = p->next)
		traverse(p->children);
}

static void visit(xmlNode *n)
{
	visit_one_node(n);
	traverse_properties(n);
	traverse(n->children);
}

static void DivingLog_importer(void)
{
	import_source = DIVINGLOG;

	/*
	 * Diving Log units are really strange.
	 *
	 * Temperatures are in C, except in samples,
	 * when they are in Fahrenheit. Depths are in
	 * meters, an dpressure is in PSI in the samples,
	 * but in bar when it comes to working pressure.
	 *
	 * Crazy f*%^ morons.
	 */
	input_units = SI_units;
}

static void uddf_importer(void)
{
	import_source = UDDF;
	input_units = SI_units;
	input_units.pressure = PASCAL;
	input_units.temperature = KELVIN;
}

/*
 * I'm sure this could be done as some fancy DTD rules.
 * It's just not worth the headache.
 */
static struct nesting {
	const char *name;
	void (*start)(void), (*end)(void);
} nesting[] = {
	{ "dive", dive_start, dive_end },
	{ "Dive", dive_start, dive_end },
	{ "trip", trip_start, trip_end },
	{ "sample", sample_start, sample_end },
	{ "waypoint", sample_start, sample_end },
	{ "SAMPLE", sample_start, sample_end },
	{ "reading", sample_start, sample_end },
	{ "event", event_start, event_end },
	{ "gasmix", cylinder_start, cylinder_end },
	{ "cylinder", cylinder_start, cylinder_end },
	{ "weightsystem", ws_start, ws_end },
	{ "divecomputer", divecomputer_start, divecomputer_end },
	{ "P", sample_start, sample_end },

	/* Import type recognition */
	{ "Divinglog", DivingLog_importer },
	{ "uddf", uddf_importer },

	{ NULL, }
};

static void traverse(xmlNode *root)
{
	xmlNode *n;

	for (n = root; n; n = n->next) {
		struct nesting *rule = nesting;

		do {
			if (!strcmp(rule->name, n->name))
				break;
			rule++;
		} while (rule->name);

		if (rule->start)
			rule->start();
		visit(n);
		if (rule->end)
			rule->end();
	}
}

/* Per-file reset */
static void reset_all(void)
{
	/*
	 * We reset the units for each file. You'd think it was
	 * a per-dive property, but I'm not going to trust people
	 * to do per-dive setup. If the xml does have per-dive
	 * data within one file, we might have to reset it per
	 * dive for that format.
	 */
	input_units = SI_units;
	import_source = UNKNOWN;
}

void parse_xml_buffer(const char *url, const char *buffer, int size, GError **error)
{
	xmlDoc *doc;

	doc = xmlReadMemory(buffer, size, url, NULL, 0);
	if (!doc) {
		fprintf(stderr, _("Failed to parse '%s'.\n"), url);
		if (error != NULL)
		{
			*error = g_error_new(g_quark_from_string("subsurface"),
					     DIVE_ERROR_PARSE,
					     _("Failed to parse '%s'"),
					     url);
		}
		return;
	}
	reset_all();
	dive_start();
#ifdef XSLT
	doc = test_xslt_transforms(doc);
#endif
	traverse(xmlDocGetRootElement(doc));
	dive_end();
	xmlFreeDoc(doc);
}

void parse_xml_init(void)
{
	LIBXML_TEST_VERSION
}

void parse_xml_exit(void)
{
	xmlCleanupParser();
}

#ifdef XSLT

/* Maybe we'll want a environment variable that can override this.. */
static const char *xslt_path = XSLT ":xslt:.";

static xsltStylesheetPtr try_get_stylesheet(const char *path, int len, const char *name)
{
	xsltStylesheetPtr ret;
	int namelen = strlen(name);
	char *filename = malloc(len+1+namelen+1);

	if (!filename)
		return NULL;

	memcpy(filename, path, len);
	filename[len] = G_DIR_SEPARATOR;
	memcpy(filename + len + 1, name, namelen+1);

	ret = NULL;
	if (!access(filename, R_OK))
		ret = xsltParseStylesheetFile(filename);
	free(filename);

	return ret;
}

static xsltStylesheetPtr get_stylesheet(const char *name)
{
	const char *path, *next;

	path = getenv("SUBSURFACE_XSLT_PATH");
	if (!path)
		path = xslt_path;

	do {
		int len;
		xsltStylesheetPtr ret;

		next = strchr(path, ':');
		len = strlen(path);
		if (next) {
			len = next - path;
			next++;
		}
		ret = try_get_stylesheet(path, len, name);
		if (ret)
			return ret;
	} while ((path = next) != NULL);

	return NULL;
}

static struct xslt_files {
	const char *root;
	const char *file;
} xslt_files[] = {
	{ "SUUNTO", "SuuntoSDM.xslt" },
	{ "JDiveLog", "jdivelog2subsurface.xslt" },
	{ NULL, }
};

xmlDoc *test_xslt_transforms(xmlDoc *doc)
{
	struct xslt_files *info = xslt_files;
	xmlDoc *transformed;
	xsltStylesheetPtr xslt = NULL;
	xmlNode *root_element = xmlDocGetRootElement(doc);

	while ((info->root) && (strcasecmp(root_element->name, info->root) != 0)) {
		info++;
	}

	if (info->root) {
		xmlSubstituteEntitiesDefault(1);
		xslt = get_stylesheet(info->file);
		if (xslt == NULL)
			return doc;
		transformed = xsltApplyStylesheet(xslt, doc, NULL);
		xmlFreeDoc(doc);
		xsltFreeStylesheet(xslt);
		return transformed;
	}
	return doc;
}
#endif
