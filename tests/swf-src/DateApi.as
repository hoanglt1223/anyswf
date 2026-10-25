// Date in UTC (the runner sets TZ=UTC): fields, setters with rollover, UTC, toString.
class DateApi {
    static function main(mc:MovieClip):Void {
        var d:Date = new Date(2000, 0, 31, 13, 45, 30, 250);
        trace(d.getFullYear() + " " + d.getMonth() + " " + d.getDate() + " " + d.getDay() + " " +
              d.getHours() + ":" + d.getMinutes() + ":" + d.getSeconds() + "." + d.getMilliseconds());
        trace(d.getTime());
        trace(d.toString());
        trace(d.getTimezoneOffset() + " " + d.getUTCHours() + " " + d.getYear());
        d.setMonth(1);
        trace(d.getMonth() + " " + d.getDate());
        d.setFullYear(2004, 1, 29);
        trace(d.getFullYear() + " " + d.getMonth() + " " + d.getDate() + " " + d.getDay());
        d.setHours(25);
        trace(d.getDate() + " " + d.getMonth() + " " + d.getHours());
        d.setMinutes(-1);
        trace(d.getHours() + ":" + d.getMinutes());
        trace(Date.UTC(1970, 0, 1, 0, 0, 1));
        var e:Date = new Date(0);
        trace(e.getUTCFullYear() + " " + e.getUTCDay() + " " + e.valueOf());
        var y:Date = new Date(99, 11, 25);
        trace(y.getFullYear() + " " + y.getMonth());
        var bad:Date = new Date(NaN);
        trace(bad.getTime() + " " + bad.toString() + " " + bad.getDate());
        e.setTime(86400000 * 365);
        trace(e.getUTCFullYear() + " " + e.getUTCMonth() + " " + e.getUTCDate());
        var df:Function = _global["Date"];
        trace(typeof df() + " " + ((new Date()).getTime() > 1700000000000));
    }
}
