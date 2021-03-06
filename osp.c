#include "osp.h"
#include "endian.h"
//#include "geodesy.h"

#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <signal.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <ctype.h>
#include <time.h>
#include <syslog.h>
#include <termios.h>
#include <math.h>

/* UTC and GPS time differ because of leap seconds.
 * In 2017 UTC is 18 seconds forward comparing GPS time. */
#define GPS_CLOCK_OFFSET 18
#define GPS_EPOCH 315964800
#define SECONDS_PER_WEEK (7*24*60*60)

#define min(a,b) \
    ({ typeof (a) _a = (a); \
       typeof (b) _b = (b); \
       _a > _b ? _a : _b; })


enum {
    SCAN_SKIPPED,
    SCAN_CONSUMED,
    SCAN_FINISHED,
};
typedef int (*scanner_f)(osp_t *osp, void *arg, osp_frame_t *frame, size_t len);

struct osp {
    driver_t *driver;
    osp_frame_t input;
    osp_frame_t output;

    bool busy;
    pthread_mutex_t lock;
    pthread_cond_t signal;

    /* callback */
    void *arg;
    const osp_callbacks_t* callbacks;

    /* scanner */
    void *scan_arg;
    scanner_f scanner;

    /* cache */
    struct {
        struct osp_position position;
        int32_t clock_drift;
        bool valid;
    } cache;
};

#if CONFIG_DUMP_PROTOCOL
static void log_line(char dir, void *frame, size_t length) {
    char message[256];
    int l;
    l = snprintf(message, sizeof(message), "%c (mid: %3d, length: %3d) ", dir, ((uint8_t*)frame)[0], length);
    int i;
    for(i = 0; i < length; i++)
        l += snprintf(message + l, sizeof(message) - l, "%02x", ((uint8_t*)frame)[i]);
    syslog(LOG_DEBUG | LOG_LOCAL0, message);
}
#else
#   define log_line(...)
#endif

static inline void utc_to_gps(uint16_t *wn, uint32_t *tow, time_t utc)
{
    uint32_t gps = utc - GPS_EPOCH + GPS_CLOCK_OFFSET;
    *wn =  gps / SECONDS_PER_WEEK;
    *tow = gps % SECONDS_PER_WEEK;
}

static inline int osp_send(osp_t *osp, size_t length)
{
    log_line('>', &osp->output, length);
    return driver_send(osp->driver, &osp->output, length);
}

/* OSP internal callbacks */
static void osp_hw_config_request(osp_t *osp)
{
    memset(&osp->output.mid214, 0, sizeof(struct mid214));
    osp->output.mid = 214;
    osp->output.mid214.hw_config.rtc_available = true;
    osp->output.mid214.hw_config.rtc_internal = true;
    osp->output.mid214.hw_config.coarse_time_ta = true;
    osp_send(osp, 1 + sizeof(struct mid214));
}

static void osp_position_transfer_request(osp_t *osp)
{
    if (osp->cache.valid) {
        memset(&osp->output.mid215, 0, sizeof(struct mid215));
        osp->output.mid = 215;
        osp->output.mid215.sid = 1;

        int64_t lat = osp->cache.position.lat;
        lat <<= 32;
        lat /= 180*10000000ll;
        int64_t lon = osp->cache.position.lon;
        lon <<=32;
        lon /= 360*10000000ll;
        int32_t alt = osp->cache.position.alt;
        alt /= 100;
        alt += 500;
        alt *= 10;
        osp->output.mid215.sid1.latitude = htobe32((int32_t)lat);
        osp->output.mid215.sid1.longitude =  htobe32((int32_t)lon);
        osp->output.mid215.sid1.altitude = htobe16((int16_t)alt);
        osp->output.mid215.sid1.est_hor_err = 0x50; /* ~120m */
        osp->output.mid215.sid1.est_ver_err = htobe16(100);
        osp->output.mid215.sid1.use_alt_aiding = false;
        osp_send(osp, 1 + 1 + sizeof(osp->output.mid215.sid1));
    } else {
        memset(&osp->output.mid216, 0, sizeof(struct mid216));
        osp->output.mid = 216;
        osp->output.mid216.sid = 2;
        osp->output.mid216.rmid = 73;
        osp->output.mid216.rsid = 1;
        osp->output.mid216.reason = 0x04;
        osp_send(osp, 1 + sizeof(struct mid216));
        syslog(LOG_DEBUG, "skip. cache-invalid\n");
    }
}

static void osp_time_transfer_request(osp_t *osp)
{
    time_t utc;
    uint64_t result;
    uint32_t tow;
    uint16_t wn;
    uint32_t tow_l;
    uint8_t tow_h;
    memset(&osp->output.mid215, 0, sizeof(struct mid215));

    time(&utc);
    utc_to_gps(&wn, &tow, utc);
    result = (uint64_t)tow * 1000000l;

    tow_l = result & 0xFFFFFFFFl;
    tow_h = (result>>32) & 0xFFl;

    osp->output.mid = 215;
    osp->output.mid215.sid = 2;
    osp->output.mid215.sid2.tt_type = 0; /* Coarse*/
    osp->output.mid215.sid2.week_number = htobe16(wn);
    osp->output.mid215.sid2.gps_time[0] = tow_h;
    *(uint32_t*)&osp->output.mid215.sid2.gps_time[1] = htobe32(tow_l);
    *(uint16_t*)&osp->output.mid215.sid2.deltat_utc[1] = htobe16(18*1000);
    osp->output.mid215.sid2.time_accuracy = 0xB0; /* shitty 1byte float */

    osp_send(osp, 1 + 1 + sizeof(osp->output.mid215.sid2));
}

static void osp_transfer_request(osp_t *osp)
{
    uint8_t sid = osp->input.mid73.sid;
    if (sid == 1)
        osp_position_transfer_request(osp);
    else if (sid == 2)
        osp_time_transfer_request(osp);
    else
        syslog(LOG_WARNING, "unhandled transfer request: %d\n", sid);
}

static void osp_geodetic_nav_data(osp_t *osp)
{
    struct tm utc = {
        .tm_sec = be16toh(osp->input.mid41.utc.second)/1000,
        .tm_min = osp->input.mid41.utc.minute,
        .tm_hour = osp->input.mid41.utc.hour,
        .tm_mday = osp->input.mid41.utc.day,
        .tm_mon = osp->input.mid41.utc.month - 1,
        .tm_year = be16toh(osp->input.mid41.utc.year) - 1900,
    };
    struct mid41 *mid = &osp->input.mid41;

    if (osp->input.mid41.svs_in_fix) {
        osp->cache.clock_drift = be32toh(mid->clock_drift);
    }
#if 0
    uint32_t err_h = be32toh(mid->est_h_pos_error)/100;
    uint32_t err_v = be32toh(mid->est_v_pos_error)/100;

    if (err_h < 200) {
        osp->cache.position.lat = be32toh(mid->latitude);
        osp->cache.position.lon = be32toh(mid->longitude);
        osp->cache.position.alt = be32toh(mid->altitude_msl);
        osp->cache.position.err_h = err_h;
        osp->cache.position.err_v = err_v;
    }
#endif

    syslog(LOG_DEBUG, "[%02d/%02d/%02d %02d:%02d:%02d] " \
           "nav valid: 0x%04x, nav type: 0x%04x, in fix: %d (%d, %d, %d)(~%d)\n",
            utc.tm_year + 1900, utc.tm_mon + 1, utc.tm_mday,
            utc.tm_hour, utc.tm_min, utc.tm_sec,
            be16toh(mid->nav_valid.word),
            be16toh(mid->nav_type.word),
            mid->svs_in_fix,
            be32toh(mid->latitude),
            be32toh(mid->longitude),
            be32toh(mid->altitude_msl),
            be32toh(mid->est_h_pos_error)
            );

    /* mktime is broken. Substract 1 from month */
    time_t timestamp = mktime(&utc);
    if (osp->callbacks && osp->callbacks->location)
        osp->callbacks->location(osp->arg,
                mid->svs_in_fix,
                be32toh(mid->latitude),
                be32toh(mid->longitude),
                timestamp);
}

static void osp_measure_nav_data_out(osp_t *osp)
{
#if 0
    struct mid2 *mid = &osp->input.mid2;
    printf("cache: %d, location updated (%d, %d, %d)\n",
            mid->svs_in_fix);
#endif
}

static void osp_measure_tracker_data_out(osp_t *osp)
{
    char buf[512];
    int l = 0;
    struct mid4 *mid = &osp->input.mid4;
    int i, j;
    l += sprintf(buf + l, "CN0: ");
    for(i = 0; i < mid->chans; i++) {
        int avg = 0;
        for(j = 0; j < 10; j++)
            avg += mid->channel[i].CN0[j];
        avg /= 10;
        uint16_t state = be16toh(mid->channel[i].state);
        struct mid4_ch_state *flags = (struct mid4_ch_state*)&state;
        l += sprintf(buf + l, "%d(%04x, %s, %d), ", mid->channel[i].svid,
                mid->channel[i].state,
                flags->ephemeris ? "eph" : "!eph",
                avg);
    }
    l += sprintf(buf + l, "\n");
    syslog(LOG_DEBUG, buf);
}

static void osp_clock_status_data(osp_t *osp)
{
}


static void osp_visible_list(osp_t *osp)
{
    int i;
    printf("Number of visible satellites: %d\n", osp->input.mid13.svs);
    for(i = 0; i < osp->input.mid13.svs; i++)
        printf("SVID: %d, (%d, %d)\n",
                osp->input.mid13.ch[i].svid,
                be16toh(osp->input.mid13.ch[i].azimuth),
                be16toh(osp->input.mid13.ch[i].elevation));
}

static void osp_nav_lib_data(osp_t *osp)
{
}

static void osp_dispatch(osp_t *osp, osp_frame_t *frame, size_t length)
{
    log_line('<', &osp->input, length);
    if (osp->scanner) {
        int srv = osp->scanner(osp, osp->scan_arg, frame, length);
        if (srv == SCAN_FINISHED) {
            pthread_mutex_lock(&osp->lock);
            pthread_cond_signal(&osp->signal);
            pthread_mutex_unlock(&osp->lock);
        } else if (srv == SCAN_CONSUMED) {
            return;
        }
    }

    switch(frame->mid) {
        case 2:
            osp_measure_nav_data_out(osp);
            break;
        case 4:
            osp_measure_tracker_data_out(osp);
            break;
        case 7:
            osp_clock_status_data(osp);
            break;
        case 13:
            osp_visible_list(osp);
            break;
        case 28:
            osp_nav_lib_data(osp);
            break;
        case 41:
            osp_geodetic_nav_data(osp);
            break;
        case 71:
            osp_hw_config_request(osp);
            break;
        case 73:
            osp_transfer_request(osp);
            break;
    }
}

static void adapter_osp_dispatch(void *arg, void* payload, size_t len)
{
    osp_dispatch((osp_t*)arg, (osp_frame_t*)payload, len);
}

osp_t* osp_alloc(driver_t* driver, const osp_callbacks_t *cb, void *cb_arg)
{
    osp_t *osp = malloc(sizeof(osp_t));
    if (!osp) {
        errno = ENOMEM;
        return NULL;
    }
    memset(osp, 0, sizeof(osp_t));
    osp->driver = driver;
    osp->callbacks = cb;
    osp->arg = cb_arg;
    pthread_mutex_init(&osp->lock, NULL);
    pthread_cond_init(&osp->signal, NULL);
    /* configure driver */
    driver_buffer(osp->driver, &osp->input, sizeof(osp->input));
    driver_dispatcher(osp->driver, adapter_osp_dispatch, osp);
    return osp;
}

int osp_start(osp_t *osp)
{
    /* TODO: ignore gps incoming data till initialization */
    driver_enable(osp->driver);
    return 0;
}

inline int osp_running(osp_t *osp)
{
}

int osp_stop(osp_t *osp)
{
    driver_disable(osp->driver);
    return 0;
}

inline static void set_scanner(osp_t *osp, scanner_f func, void *arg)
{
    osp->scanner = func;
    osp->scan_arg = arg;
}

inline static void clr_scanner(osp_t *osp)
{
    set_scanner(osp, NULL, NULL);
}

static int transfer(osp_t *osp, size_t length, void *scanner, void *response)
{
    int retval;
    struct timespec tow;

    if (!(retval = osp_send(osp, length)) && scanner) {
        clock_gettime(CLOCK_REALTIME, &tow);
        tow.tv_sec += 8;
        tow.tv_nsec = 0;
        set_scanner(osp, scanner, response);
        retval = pthread_cond_timedwait(&osp->signal, &osp->lock, &tow);
        clr_scanner(osp);
    }
    return retval;
}

static int ack_scanner(osp_t *osp, void *arg, osp_frame_t *frame, size_t len)
{
    int rv = SCAN_SKIPPED;
    int *ack = (int*)arg;
    if (frame->mid == 11) {
        *ack = 0;
        rv = SCAN_FINISHED;
    } else if (frame->mid == 12) {
        *ack = frame->mid12.nacid | 0x80;
        rv = SCAN_FINISHED;
    }
    return rv;
}

int osp_init(osp_t *osp, bool reset, osp_position_t *seed, uint32_t clock_drift)
{
    int retval = EBUSY;
    int ack = -1;

    pthread_mutex_lock(&osp->lock);
    if (!osp->busy) {
        osp->busy = true;

        osp_frame_t *frame = &osp->output;
        memset(frame, 0, 1 + sizeof(struct mid128));
        frame->mid = 128;
        frame->mid128.channels = 12;
        if (seed) {
            syslog(LOG_DEBUG, "init from seed");
            osp->cache.position.lat = seed->lat;
            osp->cache.position.lon = seed->lon;
            osp->cache.position.alt = seed->alt;
            osp->cache.clock_drift = clock_drift;
            osp->cache.valid = true;
        }

        frame->mid128.soft.system_reset = reset;
        frame->mid128.soft.cold = true;

        retval = transfer(osp, 1 + sizeof(struct mid128), ack_scanner, &ack);
        if (!retval && ack) {
            syslog(LOG_DEBUG, "osp_init nack: %d\n", ack);
            retval = EAGAIN;
        }
        osp->busy = false;
    }
    pthread_mutex_unlock(&osp->lock);
    return retval;
}

int osp_factory(osp_t *osp, bool keep_prom, bool keep_xocw)
{
    int retval = EBUSY;
    int ack = -1;

    pthread_mutex_lock(&osp->lock);
    if (!osp->busy) {
        osp->busy = true;

        osp_frame_t *frame = &osp->output;
        memset(frame, 0, 1 + sizeof(struct mid128));
        frame->mid = 128;
        frame->mid128.factory.factory = true;
        frame->mid128.factory.protocol = 0;
        frame->mid128.factory.clr_xocw = !keep_xocw;
        frame->mid128.factory.keep_rom = keep_prom;

        retval = transfer(osp, 1 + sizeof(struct mid128), ack_scanner, &ack);
        if (!retval && ack) {
            syslog(LOG_DEBUG, "osp_factory nack: %d\n", ack);
            retval = EAGAIN;
        }
        osp->busy = false;
    }
    pthread_mutex_unlock(&osp->lock);
    return retval;
}

static int ok_to_send_scanner(osp_t *osp, void *arg, osp_frame_t *frame, size_t len)
{
    int retval = SCAN_SKIPPED;
    if (frame->mid == 18) {
        retval = SCAN_FINISHED;
    }
    return retval;
}

int osp_wait_for_ready(osp_t *osp)
{
    int retval = EBUSY;
    struct timespec tow;
    pthread_mutex_lock(&osp->lock);
    clock_gettime(CLOCK_REALTIME, &tow);
    tow.tv_sec += 5;
    tow.tv_nsec = 0;
    set_scanner(osp, ok_to_send_scanner, NULL);
    retval = pthread_cond_timedwait(&osp->signal, &osp->lock, &tow);
    clr_scanner(osp);
    pthread_mutex_unlock(&osp->lock);
    return retval;
}

static int session_scanner(osp_t *osp, void *arg, osp_frame_t *frame, size_t len)
{
    int retval = SCAN_SKIPPED;
    uint8_t *response = (uint8_t*)arg;
    if (frame->mid == 74) {
        response[0] = frame->mid74.sid;
        response[1] = frame->mid74.status;
        retval = SCAN_FINISHED;
    } else if (frame->mid == 75) {
        response[0] = 3;
        retval = SCAN_FINISHED;
    }
    return retval;
}

int osp_open_session(osp_t *osp, bool resume)
{
    int retval = EBUSY;
    uint8_t response[2];
    struct timespec tow;
    clock_gettime(CLOCK_REALTIME, &tow);
    tow.tv_sec += 5;
    tow.tv_nsec = 0;

    pthread_mutex_lock(&osp->lock);
    if (!osp->busy) {
        osp->busy = true;

        osp->output.mid = 213;
        osp->output.mid213.sid = SESSION_OPENING_REQUEST;
        osp->output.mid213.request= resume ? SESSION_RESUME_REQUEST
                                               : SESSION_OPEN_REQUEST;

        retval = transfer(osp, 1 + sizeof(struct mid213), session_scanner, response);

        if (!retval && (response[0] != 1 || response[1] != 0)) {
            retval = -1;
        }
        osp->busy = false;
    }
    pthread_mutex_unlock(&osp->lock);
    return retval;
}

int osp_close_session(osp_t *osp, bool suspend)
{
    int retval = EBUSY;
    uint8_t response[2];

    pthread_mutex_lock(&osp->lock);
    if (!osp->busy) {
        osp->busy = true;

        osp->output.mid = 213;
        osp->output.mid213.sid = SESSION_CLOSING_REQUEST;
        osp->output.mid213.request= suspend ? SESSION_SUSPEND_REQUEST
                                               : SESSION_CLOSE_REQUEST;

        retval = transfer(osp, 1 + sizeof(struct mid213), session_scanner, response);
        if (!retval && (response[0] != 2 || response[1] != 0)) {
            retval = -1;
        }
        osp->busy = false;
    }
    pthread_mutex_unlock(&osp->lock);
    return retval;
}

static int pwr_ack_scanner(osp_t *osp, void *arg, osp_frame_t *frame, size_t len)
{
    int rv = SCAN_SKIPPED;
    uint8_t *data = (uint8_t*)arg;
    if (frame->mid == 90) {
        data[0] = frame->mid90.sid;
        data[1] = frame->mid90.error_code;
        rv = SCAN_FINISHED;
    }
    return rv;
}

int osp_pwr_ptf(osp_t *osp, uint32_t period, uint32_t m_search, uint32_t m_off)
{
    int retval = EBUSY;
    uint8_t response[2];

    pthread_mutex_lock(&osp->lock);
    if (!osp->busy) {
        osp->busy = true;

        osp_frame_t *frame = &osp->output;
        memset(frame, 0, 1 + sizeof(struct mid218));
        frame->mid = 218;
        frame->mid218.sid = 4;
        frame->mid218.ptf.period = htobe32(period);
        frame->mid218.ptf.max_search_time = htobe32(m_search);
        frame->mid218.ptf.max_off_time = htobe32(m_off);

        retval = transfer(osp, 1 + 1 + sizeof(struct ptf), pwr_ack_scanner, response);
        if (!retval) {
            retval = (response[0] != 4) ? EINVAL : response[1];
        }
        osp->busy = false;
    }
    pthread_mutex_unlock(&osp->lock);
    return retval;
}

int osp_pwr_full(osp_t *osp)
{
    int retval = EBUSY;
    uint8_t response[2];
    pthread_mutex_lock(&osp->lock);
    if (!osp->busy) {
        osp->busy = true;

        osp_frame_t *frame = &osp->output;
        memset(frame, 0, 1 + sizeof(struct mid218));
        frame->mid = 218;
        frame->mid218.sid = 0;

        retval = transfer(osp, 1 + 1, pwr_ack_scanner, response);
        if (!retval && (response[0] || response[1])) {
            retval = response[1];
        }
        osp->busy = false;
    }
    pthread_mutex_unlock(&osp->lock);
    return retval;
}

static int poll_almanac_scanner(osp_t *osp, void *arg, osp_frame_t *frame, size_t len)
{
    int rv = SCAN_SKIPPED;
    uint8_t *data = (uint8_t*)arg;
    if (frame->mid == 14) {
        uint8_t svid = frame->mid14.svid - 1;
        if (svid < 32) {
            size_t size = sizeof(struct almanac_row);
            uint8_t *row = (uint8_t*)&frame->mid14.row;
            int offset = svid * size;
            memcpy(&data[offset], row, size);
            rv = SCAN_CONSUMED;
        }
    } else if (frame->mid == 11 && frame->mid11.sid == 146) {
        rv = SCAN_FINISHED;
    }
    return rv;
}

int osp_almanac_poll(osp_t *osp, almanac_t *almanac)
{
    int retval = EBUSY;

    pthread_mutex_lock(&osp->lock);
    if (!osp->busy) {
        osp->busy = true;

        osp_frame_t *frame = &osp->output;
        memset(frame, 0, 1 + sizeof(struct mid146));
        frame->mid = 146;
        frame->mid146.control = 0;

        retval = transfer(osp, 1 + sizeof(struct mid146), poll_almanac_scanner, almanac);

        osp->busy = false;
    }
    pthread_mutex_unlock(&osp->lock);
    return retval;
}

int osp_almanac_set(osp_t *osp, almanac_t *almanac)
{
    int retval = EBUSY;
    int ack = -1;

    pthread_mutex_lock(&osp->lock);
    if (!osp->busy) {
        osp->busy = true;

        osp_frame_t *frame = &osp->output;
        frame->mid = 130;
        memcpy(frame->mid130.rows, almanac, sizeof(struct mid130));
        retval = transfer(osp, 1 + sizeof(struct mid130), ack_scanner, &ack);
        if (!retval && ack) {
            syslog(LOG_DEBUG, "osp_factory nack: %d\n", ack);
            retval = EAGAIN;
        }
        osp->busy = false;
    }
    pthread_mutex_unlock(&osp->lock);
    return retval;
}

struct poll_eph_result {
    ephemeris_t *eph;
    int count;
};

static int poll_eph_scanner(osp_t *osp, void *arg, osp_frame_t *frame, size_t len)
{
    int rv = SCAN_SKIPPED;
    if (frame->mid == 15) {
        struct poll_eph_result *result = arg;

        result->eph[result->count].svid = frame->mid15.svid;
        memcpy(result->eph[result->count].data,
                frame->mid15.data, sizeof(uint16_t)*45); /* FIXME: hardcoded size */
        result->count++;
        rv = SCAN_CONSUMED;
    } else if (frame->mid == 11 && frame->mid11.sid == 147) {
        rv = SCAN_FINISHED;
    }
    return rv;
}

int osp_ephemeris_poll(osp_t *osp, int svid, ephemeris_t eph[12])
{
    int retval = EBUSY;
    struct poll_eph_result result;
    result.eph = eph;
    result.count = 0;
    pthread_mutex_lock(&osp->lock);
    if (!osp->busy) {
        osp->busy = true;

        osp_frame_t *frame = &osp->output;
        memset(eph, 0, sizeof(eph));
        memset(frame, 0, 1 + sizeof(struct mid147));
        frame->mid = 147;
        frame->mid147.svid = svid;

        retval = transfer(osp, 1 + sizeof(struct mid147), poll_eph_scanner, &result);
        if (!retval) {
            retval = result.count;
        }
        osp->busy = false;
    }
    pthread_mutex_unlock(&osp->lock);
    return retval;

}

int osp_ephemeris_set(osp_t *osp, ephemeris_t *eph)
{
    int retval = EBUSY;
    int ack = -1;

    pthread_mutex_lock(&osp->lock);
    if (!osp->busy) {
        osp->busy = true;

        osp_frame_t *frame = &osp->output;
        frame->mid = 149;
        memcpy(&frame->mid149, eph->data, sizeof(struct mid149));
        retval = transfer(osp, 1 + sizeof(struct mid149), ack_scanner, &ack);
        if (!retval && ack) {
            syslog(LOG_DEBUG, "osp_ephemeris_set nack: %d\n", ack);
            retval = EAGAIN;
        }
        osp->busy = false;
    }
    pthread_mutex_unlock(&osp->lock);
    return retval;
}

int osp_ephemeris_status(osp_t *osp, eph_status_t eph_status[12])
{
    int retval = EBUSY;
    int ack = -1;

    pthread_mutex_lock(&osp->lock);
    if (!osp->busy) {
        osp->busy = true;

        osp_frame_t *frame = &osp->output;
        frame->mid = 232;
        frame->mid232.sid = 2;
        frame->mid232.svid_mask = htobe32(0xFF);
        retval = transfer(osp, 1 + sizeof(struct mid232), NULL, NULL);
        osp->busy = false;
    }
    pthread_mutex_unlock(&osp->lock);
    return retval;

}

static int cw_scanner(osp_t *osp, void *arg, osp_frame_t *frame, size_t len)
{
    int rv = SCAN_SKIPPED;
    if (frame->mid == 75) {
        printf("osp_cw: confirmed sid:%d: (%d, %d), %d\n",
                frame->mid75.sid,
                frame->mid75.echo_mid,
                frame->mid75.echo_sid,
                frame->mid75.ack);

        rv = SCAN_FINISHED;
    }
    return rv;
}

int osp_cw(osp_t *osp, bool enable)
{
    int retval = EBUSY;

    pthread_mutex_lock(&osp->lock);
    if (!osp->busy) {
        osp->busy = true;

        osp_frame_t *frame = &osp->output;
        frame->mid = 220;
        frame->mid220.sid = 1;
        frame->mid220.cw_mode = CW_MODE_SCAN_AUTO;
        retval = transfer(osp, 1 + sizeof(struct mid220), cw_scanner, NULL);
        osp->busy = false;
    }
    pthread_mutex_unlock(&osp->lock);
    return retval;
}

int osp_set_msg_rate(osp_t *osp, uint8_t mid, uint8_t mode, uint8_t rate)
{
    int retval = EBUSY;

    pthread_mutex_lock(&osp->lock);
    if (!osp->busy) {
        osp->busy = true;

        osp_frame_t *frame = &osp->output;
        frame->mid = 166;
        frame->mid166.mode = mode;
        frame->mid166.mid_to_set = mid;
        frame->mid166.update_rate = rate;
        retval = transfer(osp, 1 + sizeof(struct mid166), NULL, NULL);
        osp->busy = false;
    }
    pthread_mutex_unlock(&osp->lock);
    return retval;
}

static int version_scanner(osp_t *osp, void *arg, osp_frame_t *frame, size_t len)
{
    int rv = SCAN_SKIPPED;
    if (frame->mid == 6) {
        int bytes = min(len - 1, 80);
        memcpy(arg, frame->mid6.version, bytes);
        rv = SCAN_FINISHED;
    }
    return rv;
}

int osp_version(osp_t *osp, char *version)
{
    int retval = EBUSY;

    pthread_mutex_lock(&osp->lock);
    if (!osp->busy) {
        osp->busy = true;

        osp_frame_t *frame = &osp->output;
        frame->mid = 132;
        frame->mid132.reserved = 0;
        retval = transfer(osp, 1 + sizeof(struct mid132), version_scanner, version);
        osp->busy = false;
    }
    pthread_mutex_unlock(&osp->lock);
    return retval;

}

/* vim: set ts=4 sw=4 et: */
