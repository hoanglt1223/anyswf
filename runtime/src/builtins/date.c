/* Date, ported from Ruffle's avm1/globals/date.rs (MIT/Apache-2.0): ECMA-262 day/time
 * arithmetic on a millisecond time value, local time through the platform's UTC offset
 * (sampled once at start; no DST transitions). */
#include "../avm1/vm_internal.h"

#include <math.h>
#include <stdio.h>

#define MS_PER_SECOND 1000.0
#define MS_PER_MINUTE 60000.0
#define MS_PER_HOUR 3600000.0
#define MS_PER_DAY 86400000.0

static const uint16_t MONTH_OFFSETS[2][13] = {
    {0, 31, 59, 90, 120, 151, 181, 212, 243, 273, 304, 334, 365},
    {0, 31, 60, 91, 121, 152, 182, 213, 244, 274, 305, 335, 366}};

static int32_t clamp_i32(double d)
{
    if (isnan(d)) return 0;
    if (d >= 2147483647.0) return 2147483647;
    if (d <= -2147483648.0) return INT32_MIN;
    return (int32_t)d;
}

static int32_t rem_euclid(double lhs, int32_t rhs)
{
    int32_t r = clamp_i32(fmod(lhs, rhs));
    return r < 0 ? r + rhs : r;
}

static double day_of(double t) { return floor(t / MS_PER_DAY); }

static double day_from_year(double y)
{
    return 365.0 * (y - 1970.0) + floor((y - 1969.0) / 4.0) - floor((y - 1901.0) / 100.0) +
           floor((y - 1601.0) / 400.0);
}

static double time_from_year(int32_t y) { return MS_PER_DAY * day_from_year(y); }

static bool is_leap(int32_t y) { return y % 4 == 0 && (y % 100 != 0 || y % 400 == 0); }

static int32_t year_of(double t)
{
    double day = day_of(t);
    int32_t low = clamp_i32(floor(day / (t < 0 ? 365.0 : 366.0))) + 1970;
    int32_t high = clamp_i32(ceil(day / (t < 0 ? 366.0 : 365.0))) + 1970;
    while (low < high) {
        int32_t pivot = clamp_i32(((double)low + high) / 2.0);
        if (time_from_year(pivot) <= t) {
            if (time_from_year(pivot + 1) > t) return pivot;
            low = pivot + 1;
        } else {
            high = pivot - 1;
        }
    }
    return low;
}

static int32_t day_within_year(double t) { return clamp_i32(day_of(t) - day_from_year(year_of(t))); }

static int32_t month_of(double t)
{
    int32_t d = day_within_year(t);
    int leap = is_leap(year_of(t));
    for (int i = 0; i < 11; i++)
        if (d < MONTH_OFFSETS[leap][i + 1]) return i;
    return 11;
}

static int32_t date_of(double t)
{
    int32_t m = month_of(t);
    return day_within_year(t) - MONTH_OFFSETS[is_leap(year_of(t))][m] + 1;
}

static int32_t week_day(double t) { return rem_euclid(day_of(t) + 4.0, 7); }
static int32_t hours_of(double t) { return rem_euclid(floor((t + 0.5) / MS_PER_HOUR), 24); }
static int32_t minutes_of(double t) { return rem_euclid(floor(t / MS_PER_MINUTE), 60); }
static int32_t seconds_of(double t) { return rem_euclid(floor(t / MS_PER_SECOND), 60); }
static int32_t ms_of(double t) { return rem_euclid(t, 1000); }

static double make_time(double h, double m, double s, double ms)
{
    return floor(h) * MS_PER_HOUR + floor(m) * MS_PER_MINUTE + floor(s) * MS_PER_SECOND + floor(ms);
}

static double day_from_month(double year, double month)
{
    int32_t y = clamp_i32(year), m = clamp_i32(floor(month));
    if (m < 0 || m >= 12) return NAN;
    return day_from_year(y) + MONTH_OFFSETS[is_leap(y)][m];
}

static double make_day(double year, double month, double date)
{
    year = floor(year);
    month = floor(month);
    date = floor(date);
    year += floor(month / 12.0);
    month = fmod(month, 12.0);
    if (month < 0) month += 12.0;
    return day_from_month(year, month) + date - 1.0;
}

static double make_date(double day, double time) { return day * MS_PER_DAY + time; }

static double clip_time(double t)
{
    if (!isfinite(t) || fabs(t) > 100000000.0 * MS_PER_DAY) return NAN;
    return floor(t);
}

static double tza_ms(void)
{
    int64_t now;
    int32_t off = 0;
    if (plat_wall_clock(&now, &off) != PLAT_OK) off = 0;
    return off * MS_PER_MINUTE;
}

static double local_of(double t) { return t + tza_ms(); }
static double utc_of(double t) { return t - tza_ms(); }

static double date_now(void)
{
    int64_t now;
    int32_t off;
    return plat_wall_clock(&now, &off) == PLAT_OK ? (double)now : 0.0;
}

static void date_string(double t, char *buf, size_t cap)
{
    if (!isfinite(t)) {
        snprintf(buf, cap, "Invalid Date");
        return;
    }
    static const char *const DAYS[] = {"Sun", "Mon", "Tue", "Wed", "Thu", "Fri", "Sat"};
    static const char *const MONTHS[] = {"Jan", "Feb", "Mar", "Apr", "May", "Jun",
                                         "Jul", "Aug", "Sep", "Oct", "Nov", "Dec"};
    double l = local_of(t);
    int32_t tz = clamp_i32(tza_ms() / MS_PER_MINUTE);
    int32_t atz = tz < 0 ? -tz : tz;
    snprintf(buf, cap, "%s %s %d %02d:%02d:%02d GMT%c%02d%02d %d", DAYS[week_day(l)], MONTHS[month_of(l)],
             (int)date_of(l), (int)hours_of(l), (int)minutes_of(l), (int)seconds_of(l), tz < 0 ? '-' : '+',
             (int)(atz / 60), (int)(atz % 60), (int)year_of(l));
}

/* --- methods (one dispatcher, like Ruffle's) ------------------------------------------------ */

enum {
    GET_FULL_YEAR, GET_YEAR, GET_MONTH, GET_DATE, GET_DAY, GET_HOURS, GET_MINUTES, GET_SECONDS,
    GET_MILLISECONDS, SET_FULL_YEAR, SET_MONTH, SET_DATE, SET_HOURS, SET_MINUTES, SET_SECONDS,
    SET_MILLISECONDS, GET_TIME, SET_TIME, GET_TIMEZONE_OFFSET, TO_STRING, SET_YEAR, UTC_BIT = 128
};

static swf_val method(swf_ctx *cx, swf_val this_, uint32_t argc, const swf_val *argv, int index)
{
    double args[7];
    uint32_t n = 0;
    for (uint32_t i = 0; i < argc && i < 7; i++) {
        if (argv[i].t == SWF_T_UNDEF) break;
        args[n++] = to_number(cx, argv[i]);
    }
    if (this_.t != SWF_T_OBJ || this_.u.o->cls != OC_DATE) return swf_undef();
    swf_obj *o = this_.u.o;
    double t = o->u.date;
    if (index == GET_TIME) return swf_num(t);
    if (index == SET_TIME) {
        o->u.date = clip_time(n ? args[0] : NAN);
        return swf_num(o->u.date);
    }
    if (index == GET_TIMEZONE_OFFSET) return swf_num(-tza_ms() / MS_PER_MINUTE);
    bool utc = index >= UTC_BIT;
    if (utc) index -= UTC_BIT;
    bool is_get = index <= GET_MILLISECONDS;
    if (is_get && isnan(t)) return swf_num(NAN);
    bool set_year = index == SET_YEAR;
    if (set_year) index = SET_FULL_YEAR;
    double d = utc ? t : local_of(t);
#define ARG(i) (args[(i) - index])
#define HAS(i) ((i) >= index && (uint32_t)((i) - index) < n)
    switch (index) {
    case GET_FULL_YEAR: return swf_num(year_of(d));
    case GET_YEAR: return swf_num(year_of(d) - 1900);
    case GET_MONTH: return swf_num(month_of(d));
    case GET_DATE: return swf_num(date_of(d));
    case GET_DAY: return swf_num(week_day(d));
    case GET_HOURS: return swf_num(hours_of(d));
    case GET_MINUTES: return swf_num(minutes_of(d));
    case GET_SECONDS: return swf_num(seconds_of(d));
    case GET_MILLISECONDS: return swf_num(ms_of(d));
    case SET_FULL_YEAR:
    case SET_MONTH:
    case SET_DATE: {
        double year = (double)year_of(d), month = (double)month_of(d), date = (double)date_of(d);
        if (index == SET_FULL_YEAR) {
            year = HAS(SET_FULL_YEAR) ? ARG(SET_FULL_YEAR) : NAN;
            if (set_year && year >= 0 && year <= 99) year += 1900;
        }
        if (index <= SET_MONTH) {
            month = HAS(SET_MONTH) ? ARG(SET_MONTH) : (index == SET_MONTH ? NAN : month);
            if (index == SET_MONTH && isnan(month)) month = 0; /* setMonth() special case */
        }
        if (HAS(SET_DATE)) date = ARG(SET_DATE);
        else if (index == SET_DATE) date = NAN;
        double within = cx->version > 7 ? fmod(fmod(d, MS_PER_DAY) + MS_PER_DAY, MS_PER_DAY) : fmod(d, MS_PER_DAY);
        double nt = make_date(make_day(year, month, date), within);
        if (!utc) nt = utc_of(nt);
        o->u.date = clip_time(nt);
        return swf_num(o->u.date);
    }
    case SET_HOURS:
    case SET_MINUTES:
    case SET_SECONDS:
    case SET_MILLISECONDS: {
        double h = hours_of(d), mi = minutes_of(d), s = seconds_of(d), ms = ms_of(d);
        if (index == SET_HOURS) h = HAS(SET_HOURS) ? ARG(SET_HOURS) : NAN;
        if (index <= SET_MINUTES) mi = HAS(SET_MINUTES) ? ARG(SET_MINUTES) : (index == SET_MINUTES ? NAN : mi);
        if (index <= SET_SECONDS) s = HAS(SET_SECONDS) ? ARG(SET_SECONDS) : (index == SET_SECONDS ? NAN : s);
        if (index <= SET_MILLISECONDS)
            ms = HAS(SET_MILLISECONDS) ? ARG(SET_MILLISECONDS) : (index == SET_MILLISECONDS ? NAN : ms);
        if (index == SET_MINUTES) {
            mi = clamp_i32(mi);
            s = clamp_i32(s);
            ms = clamp_i32(ms);
        }
        double nt = make_date(day_of(d), make_time(h, mi, s, ms));
        if (!utc) nt = utc_of(nt);
        o->u.date = clip_time(nt);
        return swf_num(o->u.date);
    }
    case TO_STRING: {
        char buf[64];
        date_string(t, buf, sizeof buf);
        return swf_strv(str_from_ascii(cx->vm, buf));
    }
    default: return swf_undef();
    }
#undef ARG
#undef HAS
}

#define DATE_FN(name, idx)                                                                      \
    static swf_val name(swf_ctx *cx, swf_val this_, uint32_t argc, const swf_val *argv)         \
    {                                                                                           \
        return method(cx, this_, argc, argv, idx);                                              \
    }

DATE_FN(d_get_full_year, GET_FULL_YEAR)
DATE_FN(d_get_year, GET_YEAR)
DATE_FN(d_get_month, GET_MONTH)
DATE_FN(d_get_date, GET_DATE)
DATE_FN(d_get_day, GET_DAY)
DATE_FN(d_get_hours, GET_HOURS)
DATE_FN(d_get_minutes, GET_MINUTES)
DATE_FN(d_get_seconds, GET_SECONDS)
DATE_FN(d_get_ms, GET_MILLISECONDS)
DATE_FN(d_set_full_year, SET_FULL_YEAR)
DATE_FN(d_set_month, SET_MONTH)
DATE_FN(d_set_date, SET_DATE)
DATE_FN(d_set_hours, SET_HOURS)
DATE_FN(d_set_minutes, SET_MINUTES)
DATE_FN(d_set_seconds, SET_SECONDS)
DATE_FN(d_set_ms, SET_MILLISECONDS)
DATE_FN(d_get_time, GET_TIME)
DATE_FN(d_set_time, SET_TIME)
DATE_FN(d_get_tz, GET_TIMEZONE_OFFSET)
DATE_FN(d_to_string, TO_STRING)
DATE_FN(d_set_year, SET_YEAR)
DATE_FN(d_get_utc_full_year, UTC_BIT + GET_FULL_YEAR)
DATE_FN(d_get_utc_year, UTC_BIT + GET_YEAR)
DATE_FN(d_get_utc_month, UTC_BIT + GET_MONTH)
DATE_FN(d_get_utc_date, UTC_BIT + GET_DATE)
DATE_FN(d_get_utc_day, UTC_BIT + GET_DAY)
DATE_FN(d_get_utc_hours, UTC_BIT + GET_HOURS)
DATE_FN(d_get_utc_minutes, UTC_BIT + GET_MINUTES)
DATE_FN(d_get_utc_seconds, UTC_BIT + GET_SECONDS)
DATE_FN(d_get_utc_ms, UTC_BIT + GET_MILLISECONDS)
DATE_FN(d_set_utc_full_year, UTC_BIT + SET_FULL_YEAR)
DATE_FN(d_set_utc_month, UTC_BIT + SET_MONTH)
DATE_FN(d_set_utc_date, UTC_BIT + SET_DATE)
DATE_FN(d_set_utc_hours, UTC_BIT + SET_HOURS)
DATE_FN(d_set_utc_minutes, UTC_BIT + SET_MINUTES)
DATE_FN(d_set_utc_seconds, UTC_BIT + SET_SECONDS)
DATE_FN(d_set_utc_ms, UTC_BIT + SET_MILLISECONDS)

static uint32_t number_args(swf_ctx *cx, uint32_t argc, const swf_val *argv, double out[7])
{
    uint32_t n = 0;
    for (uint32_t i = 0; i < argc && i < 7; i++) {
        if (argv[i].t == SWF_T_UNDEF) break;
        out[n++] = to_number(cx, argv[i]);
    }
    return n;
}

static double from_parts(const double *a, uint32_t n)
{
    double year = a[0];
    if (year < 100.0) year += 1900.0;
    return make_date(make_day(year, a[1], n > 2 ? a[2] : 1.0),
                     make_time(n > 3 ? a[3] : 0, n > 4 ? a[4] : 0, n > 5 ? a[5] : 0, n > 6 ? a[6] : 0));
}

static swf_val date_ctor(swf_ctx *cx, swf_val this_, uint32_t argc, const swf_val *argv)
{
    swf_obj *o = to_object(cx, this_);
    if (!o) return swf_undef();
    double a[7];
    uint32_t n = number_args(cx, argc, argv, a);
    double t;
    if (n == 0) {
        t = date_now();
        if (cx->version > 7) t = round(t);
    } else if (n == 1) {
        t = a[0];
    } else {
        t = utc_of(from_parts(a, n));
    }
    o->cls = OC_DATE;
    o->u.date = t;
    return swf_objv(o);
}

static swf_val date_fn(swf_ctx *cx, swf_val this_, uint32_t argc, const swf_val *argv)
{
    (void)this_; (void)argc; (void)argv;
    char buf[64];
    date_string(date_now(), buf, sizeof buf);
    return swf_strv(str_from_ascii(cx->vm, buf));
}

static swf_val date_utc(swf_ctx *cx, swf_val this_, uint32_t argc, const swf_val *argv)
{
    (void)this_;
    double a[7];
    uint32_t n = number_args(cx, argc, argv, a);
    if (n < 2) return swf_undef();
    return swf_num(from_parts(a, n));
}

void builtins_date(swf_vm *vm)
{
    swf_obj *p = obj_new(vm, vm->object_proto);
    vm->date_proto = p;
    swf_obj *c = func_new_native(vm, date_fn, NULL, p);
    /* `new Date(...)` initialises and returns `this`. */
    c->u.fn.ctor = date_ctor;
    obj_define_ascii(vm, vm->global, "Date", swf_objv(c), ATTR_DONT_ENUM);
    native_method(vm, c, "UTC", date_utc, ATTR_DONT_ENUM | ATTR_DONT_DELETE | ATTR_READ_ONLY);
    const uint16_t A = ATTR_DONT_ENUM | ATTR_DONT_DELETE;
    static const struct {
        const char *name;
        swf_native_fn fn;
    } M[] = {{"getFullYear", d_get_full_year}, {"getYear", d_get_year}, {"getMonth", d_get_month},
             {"getDate", d_get_date}, {"getDay", d_get_day}, {"getHours", d_get_hours},
             {"getMinutes", d_get_minutes}, {"getSeconds", d_get_seconds}, {"getMilliseconds", d_get_ms},
             {"setFullYear", d_set_full_year}, {"setMonth", d_set_month}, {"setDate", d_set_date},
             {"setHours", d_set_hours}, {"setMinutes", d_set_minutes}, {"setSeconds", d_set_seconds},
             {"setMilliseconds", d_set_ms}, {"getTime", d_get_time}, {"setTime", d_set_time},
             {"getTimezoneOffset", d_get_tz}, {"toString", d_to_string}, {"setYear", d_set_year},
             {"getUTCFullYear", d_get_utc_full_year}, {"getUTCYear", d_get_utc_year},
             {"getUTCMonth", d_get_utc_month}, {"getUTCDate", d_get_utc_date}, {"getUTCDay", d_get_utc_day},
             {"getUTCHours", d_get_utc_hours}, {"getUTCMinutes", d_get_utc_minutes},
             {"getUTCSeconds", d_get_utc_seconds}, {"getUTCMilliseconds", d_get_utc_ms},
             {"setUTCFullYear", d_set_utc_full_year}, {"setUTCMonth", d_set_utc_month},
             {"setUTCDate", d_set_utc_date}, {"setUTCHours", d_set_utc_hours},
             {"setUTCMinutes", d_set_utc_minutes}, {"setUTCSeconds", d_set_utc_seconds},
             {"setUTCMilliseconds", d_set_utc_ms}, {"valueOf", d_get_time}};
    for (size_t i = 0; i < sizeof M / sizeof M[0]; i++) native_method(vm, p, M[i].name, M[i].fn, A);
}
