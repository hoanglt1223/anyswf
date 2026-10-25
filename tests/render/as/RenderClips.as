// base: library
// Script-made display lists: attachMovie with transforms and alpha, duplicateMovieClip,
// swapDepths, Color tints, nested empty clips with drawings, and a text field's border
// and background (no text: device fonts differ between players).
class RenderClips {
    static function main(mc:MovieClip):Void {
        var a:MovieClip = mc.attachMovie("box", "a", 10, {_x: 30, _y: 30});
        var b:MovieClip = mc.attachMovie("box", "b", 11, {_x: 50, _y: 50, _alpha: 50});
        var c:MovieClip = mc.attachMovie("box", "c", 12, {_x: 150, _y: 40, _rotation: 30, _xscale: 150});
        var d:MovieClip = c.duplicateMovieClip("d", 13, {_x: 250});
        var dc:Color = new Color(d);
        dc.setRGB(0x00CC66);
        var e:MovieClip = mc.attachMovie("box", "e", 14, {_x: 330, _y: 40});
        var f:MovieClip = mc.attachMovie("box", "f", 15, {_x: 350, _y: 60});
        var fc:Color = new Color(f);
        fc.setTransform({ra: 0, rb: 40, ga: 50, gb: 0, ba: 100, bb: 100, aa: 100, ab: 0});
        e.swapDepths(f);
        var outer:MovieClip = mc.createEmptyMovieClip("outer", 20);
        outer._x = 60;
        outer._y = 150;
        outer._rotation = -15;
        var inner:MovieClip = outer.createEmptyMovieClip("inner", 1);
        inner._xscale = 200;
        inner.beginFill(0x663399);
        inner.moveTo(0, 0);
        inner.lineTo(40, 0);
        inner.lineTo(20, 40);
        inner.endFill();
        mc.createTextField("t", 30, 250, 180, 150, 50);
        var t:Object = mc["t"];
        t.border = true;
        t.borderColor = 0xFF0000;
        t.background = true;
        t.backgroundColor = 0xCCEEFF;
    }
}
