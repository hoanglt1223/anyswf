// base: library
// A SWF button ("btn", 40x40 at 200,100) with on(release) and on(keyPress "<Space>")
// bytecode actions plus Button handler methods, driven by the cursor and Cross/Circle.
class ButtonEvents {
    static function main(mc:MovieClip):Void {
        var b:Button = mc["btn"];
        trace(typeof b + " " + b._name + " " + b.enabled + " " + b.getDepth());
        b.onRollOver = function() { trace("btn over"); };
        b.onPress = function() { trace("btn press"); };
        b.onRelease = function() { trace("btn release method"); };
        var frames:Number = 0;
        mc.onEnterFrame = function() {
            frames++;
            if (frames == 6) {
                b.enabled = false;
                trace("disabled");
            }
        };
    }
}
