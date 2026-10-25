// Drawing API render case: solid, translucent and holed fills, curves, strokes of
// several widths, and linear/radial gradient fills.
class RenderDrawing {
    static function rect(m:MovieClip, x:Number, y:Number, w:Number, h:Number):Void {
        m.moveTo(x, y);
        m.lineTo(x + w, y);
        m.lineTo(x + w, y + h);
        m.lineTo(x, y + h);
        m.lineTo(x, y);
    }

    static function circle(m:MovieClip, cx:Number, cy:Number, r:Number):Void {
        var k:Number = Math.tan(Math.PI / 8);
        m.moveTo(cx + r, cy);
        for (var i:Number = 1; i <= 8; i++) {
            var a:Number = i * Math.PI / 4;
            var c:Number = (i - 0.5) * Math.PI / 4;
            m.curveTo(cx + r / Math.cos(Math.PI / 8) * Math.cos(c), cy + r / Math.cos(Math.PI / 8) * Math.sin(c),
                      cx + r * Math.cos(a), cy + r * Math.sin(a));
        }
    }

    static function main(mc:MovieClip):Void {
        var d:MovieClip = mc.createEmptyMovieClip("d", 1);
        d.beginFill(0x3366CC);
        rect(d, 20, 20, 100, 60);
        d.endFill();
        d.beginFill(0xFF0000, 50);
        rect(d, 70, 50, 100, 60);
        d.endFill();
        d.beginFill(0x00AA00);
        rect(d, 200, 20, 100, 100);
        rect(d, 225, 45, 50, 50);
        d.endFill();
        d.beginFill(0xFFCC00);
        circle(d, 380, 70, 50);
        d.endFill();
        d.lineStyle(0, 0x000000);
        d.moveTo(20, 140);
        d.lineTo(460, 140);
        d.lineStyle(4, 0x990099);
        d.moveTo(20, 160);
        d.lineTo(120, 200);
        d.lineTo(220, 160);
        d.lineStyle(12, 0x0066FF, 80);
        d.moveTo(260, 170);
        d.curveTo(330, 120, 400, 170);
        d.lineStyle();
        var g:MovieClip = mc.createEmptyMovieClip("g", 2);
        var box:Object = {matrixType: "box", x: 20, y: 215, w: 200, h: 45, r: 0};
        g.beginGradientFill("linear", [0xFF0000, 0x0000FF], [100, 100], [0, 255], box);
        rect(g, 20, 215, 200, 45);
        g.endFill();
        var rbox:Object = {matrixType: "box", x: 300, y: 190, w: 80, h: 80, r: 0};
        g.beginGradientFill("radial", [0xFFFFFF, 0x000000], [100, 100], [0, 255], rbox);
        circle(g, 340, 230, 40);
        g.endFill();
    }
}
