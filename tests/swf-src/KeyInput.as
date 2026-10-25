// Key listeners, Key.isDown/getCode/getAscii and constants, driven by mapped PSP buttons
// (default map: D-pad = arrows, Circle = space).
class KeyInput {
    static function main(mc:MovieClip):Void {
        var l:Object = {};
        l.onKeyDown = function() {
            trace("down " + Key.getCode() + " ascii " + Key.getAscii() + " right " + Key.isDown(Key.RIGHT));
        };
        l.onKeyUp = function() { trace("up " + Key.getCode()); };
        Key.addListener(l);
        trace(Key.LEFT + " " + Key.SPACE + " " + Key.ENTER + " " + Key.isDown(Key.UP));
        var f:Number = 0;
        mc.onEnterFrame = function() {
            f++;
            if (f == 13) {
                Key.removeListener(l);
                trace("removed");
            }
            if (f == 14) trace("up held " + Key.isDown(Key.UP));
        };
    }
}
