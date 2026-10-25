// setInterval/clearInterval, setTimeout, method intervals and getTimer.
class Timers {
    static function main(mc:MovieClip):Void {
        var t0:Number = getTimer();
        trace(typeof t0);
        var count:Number = 0;
        var id;
        id = setInterval(function() {
            count++;
            trace("tick " + count);
            if (count == 3) {
                clearInterval(id);
                trace("cleared");
            }
        }, 100);
        trace(id > 0);
        var timeout:Function = _global["setTimeout"];
        timeout(function(a, b) { trace("timeout " + a + b); }, 250, "x", "y");
        var calls:Number = 0;
        var obj:Object = {name: "o"};
        obj.hello = function(s) {
            calls++;
            if (calls == 1) trace("method " + this.name + " " + s);
        };
        var mid = setInterval(obj, "hello", 50, "arg");
        var frames:Number = 0;
        mc.onEnterFrame = function() {
            frames++;
            if (frames == 10) {
                clearInterval(mid);
                trace("method calls >= 3: " + (calls >= 3));
            }
            if (frames == 30) {
                trace("elapsed >= 900: " + (getTimer() - t0 >= 900));
                trace("calls stopped: " + (calls < 10));
                delete this.onEnterFrame;
            }
        };
    }
}
