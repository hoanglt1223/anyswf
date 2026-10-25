// The cursor (scripted at screen pixels) against a drawn clip: rollOver/rollOut,
// press/release, dragOut/releaseOutside, Mouse listeners, _xmouse and hitTest.
class MouseInput {
    static function main(mc:MovieClip):Void {
        var b:MovieClip = mc.createEmptyMovieClip("b", 1);
        b.beginFill(0xFF0000);
        b.moveTo(50, 50);
        b.lineTo(100, 50);
        b.lineTo(100, 100);
        b.lineTo(50, 100);
        b.endFill();
        b.onRollOver = function() { trace("rollOver"); };
        b.onRollOut = function() { trace("rollOut"); };
        b.onPress = function() { trace("press " + this._xmouse + "," + this._ymouse); };
        b.onRelease = function() { trace("release"); };
        b.onReleaseOutside = function() { trace("releaseOutside"); };
        b.onDragOut = function() { trace("dragOut"); };
        b.onDragOver = function() { trace("dragOver"); };
        var ml:Object = {};
        ml.onMouseDown = function() { trace("mouse down " + _root._xmouse + "," + _root._ymouse); };
        ml.onMouseUp = function() { trace("mouse up"); };
        Mouse.addListener(ml);
        trace(b.hitTest(60, 60, true) + " " + b.hitTest(10, 10, true) + " " + b.hitTest(75, 75, false));
        trace(Mouse.hide() + " " + Mouse.show());
    }
}
