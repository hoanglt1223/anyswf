// base: library
// attachMovie, createEmptyMovieClip, duplicateMovieClip, removeMovieClip, depths,
// bounds, coordinates, hitTest, registerClass, onEnterFrame and onUnload.
class MovieClipApi {
    static function main(mc:MovieClip):Void {
        var a:MovieClip = mc.attachMovie("box", "a", 5, {_x: 100, _y: 20, tag: "first"});
        trace(a._name + " " + a._x + " " + a._y + " " + a["tag"] + " " + a.getDepth());
        trace(a._width + "x" + a._height + " " + a._totalframes);
        trace(mc.getNextHighestDepth());
        var e:MovieClip = mc.createEmptyMovieClip("e", 10);
        trace(typeof e + " " + e._name + " " + e.getDepth() + " " + e._totalframes + " " + e._width);
        e.lineStyle(2, 0x00FF00, 100);
        e.moveTo(0, 0);
        e.lineTo(50, 0);
        var b:Object = e.getBounds(e);
        trace(b.xMin + " " + b.xMax + " " + b.yMin + " " + b.yMax);
        var d:MovieClip = a.duplicateMovieClip("d", 7, {_x: 300});
        trace(d._name + " " + d._x + " " + d._y + " " + d.getDepth() + " " + (d["tag"] == undefined));
        a.swapDepths(7);
        trace(a.getDepth() + " " + d.getDepth() + " " + (mc.getInstanceAtDepth(7) == a));
        var pt:Object = {x: 10, y: 10};
        a.localToGlobal(pt);
        trace(pt.x + "," + pt.y);
        a.globalToLocal(pt);
        trace(pt.x + "," + pt.y);
        trace(a.hitTest(110, 30, true) + " " + a.hitTest(150, 30, true) + " " + a.hitTest(d));
        var ab:Object = a.getBounds(mc);
        trace(ab.xMin + " " + ab.xMax + " " + ab.yMin + " " + ab.yMax);
        a._name = "renamed";
        trace((mc["renamed"] == a) + " " + a._target);
        Object.registerClass("box", BoxClip);
        var c = mc.attachMovie("box", "c", 20, {tag: "t"});
        trace("attached " + (c instanceof BoxClip) + " " + c.size());
        var n:Number = 0;
        a.onEnterFrame = function() {
            n++;
            trace("a enterFrame " + n);
            if (n == 2) {
                d.removeMovieClip();
                trace("after remove " + (mc["d"] == undefined));
                delete this.onEnterFrame;
            }
        };
        d.onUnload = function() { trace("d unload " + this._name); };
        var t:MovieClip = mc.createEmptyMovieClip("timeline", 0);
        t.removeMovieClip();
        trace("removed depth 0: " + (mc["timeline"] == undefined));
    }
}
